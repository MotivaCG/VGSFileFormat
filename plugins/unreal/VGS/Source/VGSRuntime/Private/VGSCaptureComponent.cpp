#include "VGSCaptureComponent.h"

#include "VGSPlayer.h"
#include "VGSSplatLayout.h"
#include "VGSSplatSource.h"
#include "VGSStats.h"

#include "Async/ParallelFor.h"
#include "HAL/PlatformTime.h"
#include "Engine/World.h"
#include "GaussianSplatViewExtension.h" // GaussianSplat::RegisterExternalProxySource (Internal)
#include "HAL/IConsoleManager.h"
#include "Misc/App.h"
#include "Misc/Paths.h"
#include "RenderingThread.h"
#include "SceneInterface.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(VGSCaptureComponent)

// The bridge's own default, half the machine for each of a capture's two lanes, starves
// inside Unreal: its lanes spawn that many threads per batch against the engine's task
// workers. Measured 2026-11-09 (Ryzen 9 7900X, 288k splats, SH 2, rendered game): 0 showed
// 114-140 of 150 frames due and the game ran at 160-180 fps; 1, 2 and 4 showed all 150 at
// about 300 fps.
static TAutoConsoleVariable<int32> CVarVGSDecodeThreads(
	TEXT("vgs.DecodeThreads"),
	2,
	TEXT("Cores each decoding lane of a VGS capture may use; a capture has two lanes. Default 2.") LINE_TERMINATOR
	TEXT("0 lets vgsbridge choose (half the machine), which competes with the engine's own threads.") LINE_TERMINATOR
	TEXT("Open captures reopen when it changes."),
	ECVF_Default);

// Each capture bringing threads of its own does not add up: two lanes of 2 is 4 per
// capture, and a handful of captures is back to fighting the engine. On the task graph
// every capture shares the engine's workers instead, and the engine decides who runs.
static TAutoConsoleVariable<bool> CVarVGSDecodeOnTaskGraph(
	TEXT("vgs.DecodeOnTaskGraph"),
	true,
	TEXT("Decode VGS captures on the engine's task workers, shared by every capture (default), rather than") LINE_TERMINATOR
	TEXT("on threads of each capture's own. vgs.DecodeThreads is then how many pieces a batch is cut into.") LINE_TERMINATOR
	TEXT("Open captures reopen when it changes."),
	ECVF_Default);

// A capture's MaxWaitMs is fine alone, but every capture waiting its own on the same game
// frame adds up: four that are late cost 40 ms a frame, and the game drops to 25 fps.
// Their waits share this instead, first come first served; one that finds it spent shows
// its previous frame and asks again next tick.
static TAutoConsoleVariable<float> CVarVGSWaitBudgetMs(
	TEXT("vgs.WaitBudgetMs"),
	10.0f,
	TEXT("Milliseconds of one game frame that all playing VGS captures together may spend waiting for") LINE_TERMINATOR
	TEXT("decoded frames. Each capture also waits no longer than its own Max Wait Ms. Default 10."),
	ECVF_Default);

// For comparing the two in a running game without touching the components.
static TAutoConsoleVariable<int32> CVarVGSEvaluateOn(
	TEXT("vgs.EvaluateOn"),
	-1,
	TEXT("Where VGS captures work out each instant: -1 as each component's Evaluate On says (default), 0 the CPU,") LINE_TERMINATOR
	TEXT("1 the GPU. Open captures reopen when it changes."),
	ECVF_Default);

DECLARE_CYCLE_STAT(TEXT("Wait for decoded frame"), STAT_VGS_Wait, STATGROUP_VGS);
DECLARE_CYCLE_STAT(TEXT("Pack frame"), STAT_VGS_Pack, STATGROUP_VGS);
DECLARE_DWORD_COUNTER_STAT(TEXT("Splats on screen"), STAT_VGS_Splats, STATGROUP_VGS);
DECLARE_DWORD_COUNTER_STAT(TEXT("Frames late"), STAT_VGS_Late, STATGROUP_VGS);

namespace
{
	/** Decoded frames the bridge holds at once, the one shown included. */
	constexpr int32 Slots = 4;

	/** How far ahead of the playhead the bridge has chunks decompressed, as in the Blender add-on. */
	constexpr double LookaheadSeconds = 1.5;

	/** Paused or scrubbed, the asked-for frame is worth waiting for. */
	constexpr double ExactWaitMs = 250.0;

	/** Packed instants kept for reuse, so a frame does not allocate tens of megabytes. */
	constexpr int32 MaxFrameBuffers = 3;

	/** Splats per parallel task when packing. */
	constexpr int32 SplatsPerTask = 16384;

	/** How often, playing, the log gets a summary of what playback cost. */
	constexpr double SummarySeconds = 5.0;

	/** What is left of vgs.WaitBudgetMs on the game frame GWaitFrame. Game thread only. */
	uint64 GWaitFrame = MAX_uint64;
	double GWaitLeftMs = 0.0;

	double WaitBudgetLeftMs()
	{
		if (GWaitFrame != GFrameCounter)
		{
			GWaitFrame = GFrameCounter;
			GWaitLeftMs = FMath::Max(double(CVarVGSWaitBudgetMs.GetValueOnGameThread()), 0.0);
		}
		return GWaitLeftMs;
	}
}

UVGSCaptureComponent::UVGSCaptureComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
	// After movement and animation, so the transform handed to the renderer is this frame's.
	PrimaryComponentTick.TickGroup = TG_PostUpdateWork;
	bTickInEditor = true;
}

// ---- the capture ------------------------------------------------------------------------

FString UVGSCaptureComponent::ResolvePath() const
{
	const FString Raw = File.FilePath.TrimStartAndEnd();
	if (Raw.IsEmpty())
	{
		return FString();
	}
	if (!FPaths::IsRelative(Raw))
	{
		return Raw;
	}
	for (const FString& Base : { FPaths::ProjectContentDir(), FPaths::ProjectDir() })
	{
		const FString Candidate = FPaths::ConvertRelativePathToFull(Base, Raw);
		if (FPaths::FileExists(Candidate))
		{
			return Candidate;
		}
	}
	// What the file picker writes: relative to the process's base directory.
	return FPaths::ConvertRelativePathToFull(Raw);
}

bool UVGSCaptureComponent::WantsGpu() const
{
	const int32 Override = CVarVGSEvaluateOn.GetValueOnGameThread();
	return Override >= 0 ? Override == 1 : EvaluateOn == EVGSEvaluateOn::GPU;
}

bool UVGSCaptureComponent::EnsurePlayer()
{
	if (Player.IsValid())
	{
		if (Player->GetThreads() == FMath::Max(CVarVGSDecodeThreads.GetValueOnGameThread(), 0)
			&& Player->IsOnTaskGraph() == CVarVGSDecodeOnTaskGraph.GetValueOnGameThread()
			&& Player->IsPacked() == WantsGpu())
		{
			return true;
		}
		ClosePlayer(TEXT("Reopening to decode elsewhere"));
	}

	const FString Path = ResolvePath();
	if (Path.IsEmpty())
	{
		Status = TEXT("No capture file");
		return false;
	}
	if (Path == FailedPath)
	{
		return false;
	}

	FString Error;
	const int32 Threads = CVarVGSDecodeThreads.GetValueOnGameThread();
	const bool bOnTaskGraph = CVarVGSDecodeOnTaskGraph.GetValueOnGameThread();
	Player = WantsGpu()
		? VGS::FPlayer::OpenPacked(Path, bSphericalHarmonics, Threads, bOnTaskGraph, Error)
		: VGS::FPlayer::Open(Path, bSphericalHarmonics, Slots, Threads, bOnTaskGraph, Error);
	if (!Player.IsValid())
	{
		FailedPath = Path;
		Status = FString::Printf(TEXT("Cannot open %s: %s"), *Path, *Error);
		UE_LOG(LogVGS, Warning, TEXT("%s"), *Status);
		return false;
	}

	FailedPath.Reset();
	const vgsb_info& Info = Player->GetInfo();
	Title = VGS::FromUtf8(Info.title);
	Duration = float(Info.duration);
	FrameRate = float(Info.frame_rate);
	FrameCount = int64(Info.frame_count);
	MaxSplats = int64(Info.max_splats);
	SHDegree = int32(Info.sh_degree);
	Player->SetDensity(Density);
	ShownSeconds = -1.0;
	bFrameDirty = true;
	Status = TEXT("Opened");
	UpdateBounds();
	UE_LOG(LogVGS, Log, TEXT("Opened %s: %lld frames at %.3f fps, up to %lld splats, SH degree %d, evaluated on the %s, decoding %s, %d per batch"),
		*Path, FrameCount, FrameRate, MaxSplats, SHDegree, Player->IsPacked() ? TEXT("GPU") : TEXT("CPU"),
		Player->IsOnTaskGraph() ? TEXT("on the task graph") : TEXT("on its own threads"), Player->GetThreads());
	return true;
}

void UVGSCaptureComponent::ClosePlayer(const FString& Why)
{
	Player.Reset();
	Status = Why;
	ShownSeconds = -1.0;
	ShownSplats = 0;
	bFrameDirty = true;

	// Whatever the renderer still shows of the old capture goes with it.
	ShownFrame.Reset();
	ShownChunk.Reset();
	if (Source.IsValid())
	{
		FVGSPackedFrameRef Empty = MakeShared<FVGSPackedFrame, ESPMode::ThreadSafe>();
		ENQUEUE_RENDER_COMMAND(VGSClearFrame)([Target = Source, Empty](FRHICommandList&)
		{
			Target->SetFrame_RenderThread(Empty);
		});
	}
}

void UVGSCaptureComponent::Reload()
{
	ClosePlayer(TEXT("Reloading"));
	FailedPath.Reset();
}

// ---- time -------------------------------------------------------------------------------

EVGSLoopMode UVGSCaptureComponent::EffectiveLoopMode() const
{
	if (LoopMode != EVGSLoopMode::FromCapture || !Player.IsValid())
	{
		return LoopMode == EVGSLoopMode::FromCapture ? EVGSLoopMode::Loop : LoopMode;
	}
	switch (Player->GetInfo().playback_mode)
	{
	case 0: return EVGSLoopMode::Once;
	case 2: return EVGSLoopMode::PingPong;
	default: return EVGSLoopMode::Loop;
	}
}

double UVGSCaptureComponent::CaptureSeconds(double PlaybackSeconds) const
{
	const vgsb_info& Info = Player->GetInfo();
	const double End = Info.duration;
	const double Fps = Info.frame_rate;
	double Seconds = double(Phase) * End + PlaybackSeconds;

	if (End > 0.0)
	{
		switch (EffectiveLoopMode())
		{
		case EVGSLoopMode::Loop:
		{
			// One frame interval past the last instant, so the last and the first frame are
			// not shown back to back as if they were one (as the Blender add-on loops).
			const double Period = End + (Fps > 0.0 ? 1.0 / Fps : 0.0);
			Seconds = FMath::Fmod(Seconds, Period);
			if (Seconds < 0.0)
			{
				Seconds += Period;
			}
			break;
		}
		case EVGSLoopMode::PingPong:
			Seconds = FMath::Fmod(Seconds, 2.0 * End);
			if (Seconds < 0.0)
			{
				Seconds += 2.0 * End;
			}
			if (Seconds > End)
			{
				Seconds = 2.0 * End - Seconds;
			}
			break;
		default:
			break;
		}
	}
	Seconds = FMath::Clamp(Seconds, 0.0, FMath::Max(End, 0.0));

	// Snapped to the capture's own frames. The game's frame times are not the capture's, and
	// the bridge decodes ahead exactly the instants it was told about: on the grid, what is
	// scheduled is what will be asked for.
	if (Fps > 0.0)
	{
		Seconds = FMath::Min(FMath::FloorToDouble(Seconds * Fps + 1e-6) / Fps, End);
	}
	return Seconds;
}

bool UVGSCaptureComponent::ShouldAdvance() const
{
	const UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}
	return World->IsGameWorld() ? bPlaying : bPlayInEditor;
}

void UVGSCaptureComponent::Play()
{
	bPlaying = true;
}

void UVGSCaptureComponent::Pause()
{
	bPlaying = false;
}

void UVGSCaptureComponent::SetPlaybackTime(float Seconds)
{
	Elapsed = Seconds;
}

// ---- ticking ----------------------------------------------------------------------------

void UVGSCaptureComponent::BeginPlay()
{
	Super::BeginPlay();
	bPlaying = bAutoPlay;
}

void UVGSCaptureComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	const bool bAdvancing = ShouldAdvance();
	if (bAdvancing)
	{
		Elapsed += double(DeltaTime) * double(PlayRate);
	}

	if (EnsurePlayer())
	{
		UpdateFrame(bAdvancing, FMath::Abs(double(DeltaTime) * double(PlayRate)));
	}
	PushRenderState();
	SET_DWORD_STAT(STAT_VGS_Splats, uint32(ShownSplats));
	LogSummary(bAdvancing, DeltaTime);
}

void UVGSCaptureComponent::LogSummary(bool bAdvancing, float DeltaTime)
{
	const double Now = FPlatformTime::Seconds();
	if (!bAdvancing || !Player.IsValid())
	{
		Summary = FPlaybackSummary();
		Summary.Start = Now;
		return;
	}
	++Summary.Ticks;
	Summary.TickMs += double(DeltaTime) * 1000.0;
	if (Summary.Start <= 0.0)
	{
		Summary.Start = Now;
		return;
	}
	const double Span = Now - Summary.Start;
	if (Span < SummarySeconds)
	{
		return;
	}

	// Due: the capture frames the clock went through, at the capture's rate and speed.
	const double Due = Span * double(FrameRate) * FMath::Abs(double(PlayRate));
	const int32 Asked = FMath::Max(Summary.Shown + Summary.Late, 1);
	UE_LOG(LogVGS, Log,
		TEXT("%s: %.1f s, game %.1f fps; capture frames due %.0f, shown %d, ticks waiting %d; wait avg %.2f ms max %.1f; pack avg %.2f ms max %.1f; %lld splats, %s"),
		GetOwner() ? *GetOwner()->GetActorNameOrLabel() : *GetName(), Span, Summary.Ticks / FMath::Max(Summary.TickMs / 1000.0, 1e-6),
		Due, Summary.Shown, Summary.Late, Summary.WaitMs / Asked, Summary.WaitMaxMs,
		Summary.PackMs / FMath::Max(Summary.Shown, 1), Summary.PackMaxMs, ShownSplats,
		Player->IsPacked() ? TEXT("evaluated on the GPU") : GpuLayout == EVGSGpuLayout::Quantized ? TEXT("quantized") : TEXT("float"));
	Summary = FPlaybackSummary();
	Summary.Start = Now;
}

void UVGSCaptureComponent::UpdateFrame(bool bAdvancing, double AdvancePerTick)
{
	Player->SetIncludeSH(bSphericalHarmonics);
	Player->SetDensity(Density);

	const vgsb_info& Info = Player->GetInfo();
	const double Now = CaptureSeconds(Elapsed);

	// What the next ticks will ask for, soonest first. Playing, that is the frames ahead in
	// the direction of play, a step per tick or per capture frame, whichever is longer (at
	// twice the speed every other frame is skipped, and decoding it would be wasted).
	// Paused, it is whichever way a scrub goes next.
	ScheduleScratch.Reset();
	ScheduleScratch.Add(Now);
	if (Info.frame_rate > 0.0)
	{
		const double FrameStep = 1.0 / Info.frame_rate;
		if (bAdvancing && PlayRate != 0.0f)
		{
			const double Step = FMath::Max(FrameStep, AdvancePerTick) * (PlayRate > 0.0f ? 1.0 : -1.0);
			const int32 Ahead = FMath::Clamp(FMath::CeilToInt32(LookaheadSeconds / FMath::Abs(Step)), Slots, 256);
			for (int32 K = 1; K <= Ahead; ++K)
			{
				const double Next = CaptureSeconds(Elapsed + K * Step);
				if (Next != ScheduleScratch.Last())
				{
					ScheduleScratch.Add(Next);
				}
			}
		}
		else
		{
			for (const double Next : { CaptureSeconds(Elapsed + FrameStep), CaptureSeconds(Elapsed - FrameStep) })
			{
				ScheduleScratch.AddUnique(Next);
			}
		}
	}
	if (ScheduleScratch != LastSchedule)
	{
		Player->Schedule(ScheduleScratch);
		LastSchedule = ScheduleScratch;
	}

	// Settings that need the instant packed again, wherever they were changed from.
	if (IntensityEV != PackedIntensityEV || (!Player->IsPacked() && GpuLayout != PackedLayout) || (Player->IsPacked() && Density != PackedDensity))
	{
		bFrameDirty = true;
	}
	if (!bFrameDirty && Now == ShownSeconds)
	{
		return;
	}

	if (Player->IsPacked())
	{
		// Packed: once its chunk is on hand any instant is ready, so the wait is only ever
		// for a chunk, and the per-frame work is the basis and a compute pass.
		vgsb_packed_frame Packed = {};
		const double WaitStart = FPlatformTime::Seconds();
		int32 Result;
		{
			SCOPE_CYCLE_COUNTER(STAT_VGS_Wait);
			const double TimeoutMs = bAdvancing ? FMath::Min(double(MaxWaitMs), WaitBudgetLeftMs()) : ExactWaitMs;
			Result = Player->AcquirePacked(Now, TimeoutMs, Packed);
		}
		const double WaitMs = (FPlatformTime::Seconds() - WaitStart) * 1000.0;
		if (bAdvancing)
		{
			GWaitLeftMs = FMath::Max(GWaitLeftMs - WaitMs, 0.0);
		}
		Summary.WaitMs += WaitMs;
		Summary.WaitMaxMs = FMath::Max(Summary.WaitMaxMs, WaitMs);
		if (Result == VGSB_TIMEOUT)
		{
			INC_DWORD_STAT(STAT_VGS_Late);
			++Summary.Late;
			return;
		}
		if (Result != VGSB_OK)
		{
			const FString Error = Player->LastError();
			const FString Path = Player->GetPath();
			UE_LOG(LogVGS, Error, TEXT("%s: %s"), *Path, *Error);
			ClosePlayer(FString::Printf(TEXT("Failed: %s"), *Error));
			FailedPath = Path;
			return;
		}
		const double PackStart = FPlatformTime::Seconds();
		{
			SCOPE_CYCLE_COUNTER(STAT_VGS_Pack);
			SendPacked(Packed);
		}
		const double PackMs = (FPlatformTime::Seconds() - PackStart) * 1000.0;
		Summary.PackMs += PackMs;
		Summary.PackMaxMs = FMath::Max(Summary.PackMaxMs, PackMs);
		++Summary.Shown;
		Player->Release();
		ShownSeconds = Packed.seconds;
		bFrameDirty = false;
		Status = FString::Printf(TEXT("%lld splats at %.3f s, on the GPU"), ShownSplats, Packed.seconds);
		return;
	}

	vgsb_frame Frame = {};
	const double WaitStart = FPlatformTime::Seconds();
	int32 Result;
	{
		SCOPE_CYCLE_COUNTER(STAT_VGS_Wait);
		// Paused or scrubbed waits for the exact frame. Playing waits within the shared budget
		// and then takes a late frame if one is ready: a capture the machine cannot keep up
		// with plays at a lower rate rather than decoding every frame just too late to show.
		if (bAdvancing)
		{
			const double TimeoutMs = FMath::Min(double(MaxWaitMs), WaitBudgetLeftMs());
			Result = ShownSeconds >= 0.0 && !bFrameDirty
				? Player->AcquireBetween(Now, ShownSeconds, TimeoutMs, Frame)
				: Player->Acquire(Now, TimeoutMs, Frame);
		}
		else
		{
			Result = Player->Acquire(Now, ExactWaitMs, Frame);
		}
	}
	const double WaitMs = (FPlatformTime::Seconds() - WaitStart) * 1000.0;
	if (bAdvancing)
	{
		GWaitLeftMs = FMath::Max(GWaitLeftMs - WaitMs, 0.0);
	}
	Summary.WaitMs += WaitMs;
	Summary.WaitMaxMs = FMath::Max(Summary.WaitMaxMs, WaitMs);
	if (Result == VGSB_TIMEOUT)
	{
		// Not decoded yet: the frame before stays on screen, and the request stands.
		INC_DWORD_STAT(STAT_VGS_Late);
		++Summary.Late;
		return;
	}
	if (Result != VGSB_OK)
	{
		const FString Error = Player->LastError();
		const FString Path = Player->GetPath();
		UE_LOG(LogVGS, Error, TEXT("%s: %s"), *Path, *Error);
		ClosePlayer(FString::Printf(TEXT("Failed: %s"), *Error));
		FailedPath = Path;
		return;
	}

	const double PackStart = FPlatformTime::Seconds();
	{
		SCOPE_CYCLE_COUNTER(STAT_VGS_Pack);
		PackAndSend(Frame);
	}
	const double PackMs = (FPlatformTime::Seconds() - PackStart) * 1000.0;
	Summary.PackMs += PackMs;
	Summary.PackMaxMs = FMath::Max(Summary.PackMaxMs, PackMs);
	++Summary.Shown;
	Player->Release();

	ShownSeconds = Frame.seconds;
	bFrameDirty = false;
	Status = FString::Printf(TEXT("%lld splats at %.3f s"), ShownSplats, Frame.seconds);
}

TSharedPtr<FVGSPackedFrame, ESPMode::ThreadSafe> UVGSCaptureComponent::TakeFrameBuffer()
{
	// A buffer nobody else holds: not on screen, not waiting for the render thread, not in
	// an upload RDG has yet to copy.
	for (const TSharedPtr<FVGSPackedFrame, ESPMode::ThreadSafe>& Buffer : FrameBuffers)
	{
		if (Buffer.GetSharedReferenceCount() == 1)
		{
			return Buffer;
		}
	}
	TSharedPtr<FVGSPackedFrame, ESPMode::ThreadSafe> Buffer = MakeShared<FVGSPackedFrame, ESPMode::ThreadSafe>();
	if (FrameBuffers.Num() < MaxFrameBuffers)
	{
		FrameBuffers.Add(Buffer);
	}
	return Buffer;
}

void UVGSCaptureComponent::PackAndSend(const vgsb_frame& Frame)
{
	vgsunreal::FrameView View;
	View.count = size_t(Frame.count);
	View.positions = Frame.positions;
	View.rotations = Frame.rotations;
	View.scales = Frame.scales;
	View.radiance = Frame.radiance;
	int32 Degree = vgsunreal::shDegree(Frame.sh_coefficients);
	if (Degree > 0 && Frame.sh)
	{
		View.shCoefficients = Frame.sh_coefficients;
		View.sh = Frame.sh;
	}
	else
	{
		Degree = 0;
	}

	const bool bQuantized = GpuLayout == EVGSGpuLayout::Quantized;
	const vgsunreal::Gain Gain = vgsunreal::gainFor(FMath::Exp2(IntensityEV));
	const uint32 Stride = bQuantized ? vgsunreal::quantizedStride(View.shCoefficients) : vgsunreal::floatStride(View.shCoefficients);
	const uint64 Bytes = uint64(View.count) * Stride;
	if (Bytes > uint64(MAX_int32))
	{
		UE_LOG(LogVGS, Error, TEXT("A frame of %llu splats does not fit one buffer"), uint64(View.count));
		return;
	}

	TSharedPtr<FVGSPackedFrame, ESPMode::ThreadSafe> Packed = TakeFrameBuffer();
	Packed->Bytes.SetNumUninitialized(int32(Bytes), EAllowShrinking::No);
	Packed->NumSplats = uint32(View.count);
	Packed->Stride = Stride;
	Packed->SHDegree = Degree;
	Packed->bQuantized = bQuantized;
	Packed->Seconds = Frame.seconds;

	const int32 Tasks = FMath::Max(1, int32(FMath::DivideAndRoundUp<uint64>(View.count, SplatsPerTask)));
	const auto Span = [&View](int32 Task, size_t& Begin, size_t& End)
	{
		Begin = size_t(Task) * SplatsPerTask;
		End = FMath::Min(View.count, Begin + SplatsPerTask);
	};
	uint8* Destination = Packed->Bytes.GetData();

	if (bQuantized)
	{
		TArray<vgsunreal::Ranges> Ranges;
		Ranges.SetNum(Tasks);
		ParallelFor(Tasks, [&](int32 Task)
		{
			size_t Begin, End;
			Span(Task, Begin, End);
			Ranges[Task] = vgsunreal::measure(View, Begin, End);
		});
		vgsunreal::Ranges All = vgsunreal::Ranges::empty();
		for (const vgsunreal::Ranges& R : Ranges)
		{
			All.merge(R);
		}
		Packed->Quantization = vgsunreal::quantization(All);
		const vgsunreal::Quantization& Q = Packed->Quantization;
		ParallelFor(Tasks, [&](int32 Task)
		{
			size_t Begin, End;
			Span(Task, Begin, End);
			vgsunreal::packQuantized(View, Q, Begin, End, Destination);
		});
	}
	else
	{
		Packed->Quantization = {};
		ParallelFor(Tasks, [&](int32 Task)
		{
			size_t Begin, End;
			Span(Task, Begin, End);
			vgsunreal::packFloat(View, Begin, End, Destination, Gain);
		});
	}

	if (bQuantized)
	{
		// The bytes are packed as decoded; the renderer's scales and biases carry the gain.
		vgsunreal::applyGain(Packed->Quantization, Gain);
	}
	PackedIntensityEV = IntensityEV;
	PackedLayout = GpuLayout;

	ShownFrame = Packed;
	ShownSplats = int64(View.count);
	if (Source.IsValid())
	{
		ENQUEUE_RENDER_COMMAND(VGSSetFrame)([Target = Source, Frame = FVGSPackedFrameRef(Packed)](FRHICommandList&)
		{
			Target->SetFrame_RenderThread(Frame);
		});
	}
}

void UVGSCaptureComponent::SendPacked(const vgsb_packed_frame& Frame)
{
	const vgsb_packed_chunk& Chunk = *Frame.chunk;
	if (!ShownChunk.IsValid() || ShownChunk->Get().serial != Chunk.serial)
	{
		// Retained past this acquire, for as long as the render thread needs it.
		ShownChunk = MakeShared<const VGS::FPackedChunk, ESPMode::ThreadSafe>(Frame.chunk);
	}

	TSharedRef<FVGSGpuFrame, ESPMode::ThreadSafe> Gpu = MakeShared<FVGSGpuFrame, ESPMode::ThreadSafe>();
	Gpu->Chunk = ShownChunk;
	Gpu->Frame = Frame.frame;
	Gpu->Alpha = Frame.alpha;
	Gpu->Normalized = Frame.normalized;
	Gpu->Seconds = Frame.seconds;
	const int32 Texels = FMath::Max(Chunk.basis_texels, 0);
	Gpu->Basis.SetNumUninitialized(Texels);
	FMemory::Memcpy(Gpu->Basis.GetData(), Frame.basis, size_t(Texels) * sizeof(FVector4f));

	const vgsunreal::Gain Gain = vgsunreal::gainFor(FMath::Exp2(IntensityEV));
	Gpu->Gain = Gain.m;
	Gpu->DcOffset = Gain.dcOffset;
	// As vgsbridge thins a CPU frame (vgsb_set_density): the same records, the same opacity.
	const float Kept = FMath::Clamp(Density, 0.01f, 1.0f);
	Gpu->bThinned = Kept < 1.0f;
	Gpu->DensityThreshold = Gpu->bThinned ? uint32(FMath::Min(double(Kept) * 4294967296.0, 4294967295.0)) : MAX_uint32;
	Gpu->DensityExponent = Gpu->bThinned ? 1.0f / Kept : 1.0f;

	PackedIntensityEV = IntensityEV;
	PackedDensity = Density;
	ShownFrame.Reset();
	// Every record of the chunk is drawn; those not alive at the instant have no size.
	ShownSplats = int64(Chunk.splats);
	if (Source.IsValid())
	{
		ENQUEUE_RENDER_COMMAND(VGSSetGpuFrame)([Target = Source, Frame = FVGSGpuFrameRef(Gpu)](FRHICommandList&)
		{
			Target->SetGpuFrame_RenderThread(Frame);
		});
	}
}

// ---- the renderer -----------------------------------------------------------------------

FMatrix UVGSCaptureComponent::CaptureToComponent() const
{
	float Rows[3][3];
	vgsunreal::captureToUnreal(UpAxis == EVGSUpAxis::Z ? vgsunreal::UpAxis::Z : vgsunreal::UpAxis::Y, UnitScale, Rows);
	return FMatrix(
		FPlane(Rows[0][0], Rows[0][1], Rows[0][2], 0.0),
		FPlane(Rows[1][0], Rows[1][1], Rows[1][2], 0.0),
		FPlane(Rows[2][0], Rows[2][1], Rows[2][2], 0.0),
		FPlane(0.0, 0.0, 0.0, 1.0));
}

void UVGSCaptureComponent::PushRenderState()
{
	if (!Source.IsValid())
	{
		return;
	}

	// Row vectors: capture axes and metres first, then the component's transform.
	const FMatrix LocalToWorld = CaptureToComponent() * GetComponentTransform().ToMatrixWithScale();

	FVGSRenderState State;
	State.LocalToWorld = FMatrix44f(LocalToWorld);
	State.WorldToLocal = FMatrix44f(LocalToWorld.Inverse());
	if (Player.IsValid())
	{
		const double* B = Player->GetInfo().bounds;
		State.LocalBounds = FBox3f(FVector3f(float(B[0]), float(B[1]), float(B[2])), FVector3f(float(B[3]), float(B[4]), float(B[5])));
	}
	State.bVisible = ShouldRender();
	State.PointScaleFactor = PointScaleFactor;
	State.OpacityScale = OpacityScale;
	State.InverseToneCurveStrength = bInvertToneCurve ? InverseToneCurveStrength : 0.0f;
	State.bInvertExpandGamut = bInvertExpandGamut;

	ENQUEUE_RENDER_COMMAND(VGSSetState)([Target = Source, State](FRHICommandList&)
	{
		Target->SetState_RenderThread(State);
	});
}

void UVGSCaptureComponent::RegisterSource()
{
	if (Source.IsValid() || IsTemplate() || !FApp::CanEverRender())
	{
		return;
	}
	UWorld* World = GetWorld();
	if (!World || !World->Scene)
	{
		return;
	}

	Source = MakeShared<FVGSSplatSource, ESPMode::ThreadSafe>();
	GaussianSplat::RegisterExternalProxySource(World,
		FOnCollectGaussianSplatProxies::FDelegate::CreateSP(Source.ToSharedRef(), &FVGSSplatSource::Collect_RenderThread));

	// Registered again after an edit: show what was on screen without waiting for a new frame.
	if (ShownFrame.IsValid())
	{
		ENQUEUE_RENDER_COMMAND(VGSResendFrame)([Target = Source, Frame = FVGSPackedFrameRef(ShownFrame)](FRHICommandList&)
		{
			Target->SetFrame_RenderThread(Frame);
		});
	}
	PushRenderState();
}

void UVGSCaptureComponent::ReleaseSource()
{
	if (!Source.IsValid())
	{
		return;
	}
	// The renderer's binding expires with the source; letting go of it on the render thread
	// is what keeps the renderer from calling into it half destroyed.
	ENQUEUE_RENDER_COMMAND(VGSReleaseSource)([Released = MoveTemp(Source)](FRHICommandList&) mutable
	{
		Released.Reset();
	});
	Source.Reset();
}

// ---- component lifetime -----------------------------------------------------------------

void UVGSCaptureComponent::OnRegister()
{
	Super::OnRegister();
	RegisterSource();
}

void UVGSCaptureComponent::OnUnregister()
{
	// The capture stays open: in the editor every edit unregisters and registers again.
	ReleaseSource();
	Super::OnUnregister();
}

void UVGSCaptureComponent::OnComponentDestroyed(bool bDestroyingHierarchy)
{
	ReleaseSource();
	Player.Reset();
	Super::OnComponentDestroyed(bDestroyingHierarchy);
}

void UVGSCaptureComponent::BeginDestroy()
{
	ReleaseSource();
	Player.Reset();
	Super::BeginDestroy();
}

void UVGSCaptureComponent::OnUpdateTransform(EUpdateTransformFlags UpdateTransformFlags, ETeleportType Teleport)
{
	Super::OnUpdateTransform(UpdateTransformFlags, Teleport);
	PushRenderState();
}

FBoxSphereBounds UVGSCaptureComponent::CalcBounds(const FTransform& LocalToWorld) const
{
	if (Player.IsValid())
	{
		const double* B = Player->GetInfo().bounds;
		const FBox Box(FVector(B[0], B[1], B[2]), FVector(B[3], B[4], B[5]));
		return FBoxSphereBounds(Box.TransformBy(CaptureToComponent() * LocalToWorld.ToMatrixWithScale()));
	}
	return FBoxSphereBounds(LocalToWorld.GetLocation(), FVector(50.0), 50.0);
}

#if WITH_EDITOR
void UVGSCaptureComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	const FName Name = PropertyChangedEvent.GetMemberPropertyName();
	if (Name == GET_MEMBER_NAME_CHECKED(UVGSCaptureComponent, File))
	{
		ClosePlayer(TEXT("Opening"));
		FailedPath.Reset();
	}
	// Harmonics, density, layout, phase: the frame on screen is no longer the one asked for.
	bFrameDirty = true;
	UpdateBounds();
	PushRenderState();
}
#endif
