#include "VGSCaptureComponent.h"

#include "VGSPlayer.h"
#include "VGSSplatLayout.h"
#include "VGSSplatSource.h"

#include "Async/ParallelFor.h"
#include "Engine/World.h"
#include "GaussianSplatViewExtension.h" // GaussianSplat::RegisterExternalProxySource (Internal)
#include "Misc/App.h"
#include "Misc/Paths.h"
#include "RenderingThread.h"
#include "SceneInterface.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(VGSCaptureComponent)

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

bool UVGSCaptureComponent::EnsurePlayer()
{
	if (Player.IsValid())
	{
		return true;
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
	Player = VGS::FPlayer::Open(Path, bSphericalHarmonics, Slots, Error);
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
	UE_LOG(LogVGS, Log, TEXT("Opened %s: %lld frames at %.3f fps, up to %lld splats, SH degree %d"),
		*Path, FrameCount, FrameRate, MaxSplats, SHDegree);
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
	if (IntensityEV != PackedIntensityEV || GpuLayout != PackedLayout)
	{
		bFrameDirty = true;
	}
	if (!bFrameDirty && Now == ShownSeconds)
	{
		return;
	}

	vgsb_frame Frame = {};
	const int32 Result = Player->Acquire(Now, bAdvancing ? double(MaxWaitMs) : ExactWaitMs, Frame);
	if (Result == VGSB_TIMEOUT)
	{
		// Not decoded yet: the frame before stays on screen, and the request stands.
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

	PackAndSend(Frame);
	Player->Release();

	ShownSeconds = Now;
	bFrameDirty = false;
	Status = FString::Printf(TEXT("%lld splats at %.3f s"), ShownSplats, Now);
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
