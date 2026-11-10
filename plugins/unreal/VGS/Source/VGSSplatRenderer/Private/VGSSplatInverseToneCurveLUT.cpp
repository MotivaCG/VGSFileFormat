// VGS: a copy of Epic's GaussianSplat renderer (UE 5.9.0, CL 60351404), renamed by plugins/unreal/tools/copy_epic_splat_renderer.py.
// What VGS changed is marked "VGS CHANGE"; see plugins/unreal/EPIC_RENDERER_COPY.md.
// Copyright Epic Games, Inc. All Rights Reserved.

#include "VGSSplatInverseToneCurveLUT.h"
#include "VGSSplatInverseToneCurveLUTCache.h"

#include "Async/Async.h"
#include "HAL/ThreadSafeBool.h"
#include "RenderingThread.h"
#include "RenderResource.h"
#include "RHITextureReference.h"

DEFINE_LOG_CATEGORY(LogVGSSplat);

namespace VGSSplat
{
	namespace
	{
		// Baked asset path for each LUT variant.
		const TSortedMap<EInverseToneCurveLUTVariant, const TCHAR*> GLUTAssetPaths =
		{
			{ EInverseToneCurveLUTVariant::Filmic,            TEXT("/GaussianSplat/LUTs/UE_InverseFilmicTC.UE_InverseFilmicTC") },
			{ EInverseToneCurveLUTVariant::FilmicExpandGamut, TEXT("/GaussianSplat/LUTs/UE_InverseFilmicTC_Full.UE_InverseFilmicTC_Full") },
		};
	}

	/** One per-variant render-thread cache slot: the stable texture reference and one-shot load guard. */
	struct FInverseToneCurveLUTRenderThreadCacheSlot
	{
		/** Stable RHI texture reference. */
		FTextureReferenceRHIRef TextureReferenceRHI;

		/** Render-thread only; gates the one-shot load dispatch. */
		bool bLoadRequested = false;
	};

	/** Render-thread state may be weakly referenced by queued commands after the cache starts shutting down. */
	struct FInverseToneCurveLUTRenderThreadState
	{
		/** Per-variant render-thread cache slots. */
		TSortedMap<EInverseToneCurveLUTVariant, FInverseToneCurveLUTRenderThreadCacheSlot> CacheSlots;

		/** Set during module shutdown so queued render-thread work can early-out. */
		FThreadSafeBool bShuttingDown = false;
	};

	FInverseToneCurveLUTCache::FInverseToneCurveLUTCache()
		: RenderThreadState(MakeShared<FInverseToneCurveLUTRenderThreadState, ESPMode::ThreadSafe>())
	{
	}

	FInverseToneCurveLUTCache::~FInverseToneCurveLUTCache()
	{
		// Assets holds UObject strong pointers, so the cache must only ever be destroyed on the game thread.
		check(IsInGameThread());

		if (RenderThreadState.IsValid())
		{
			RenderThreadState->bShuttingDown = true;

			TSharedPtr<FInverseToneCurveLUTRenderThreadState, ESPMode::ThreadSafe> RenderThreadStateToRelease = MoveTemp(RenderThreadState);
			ENQUEUE_RENDER_COMMAND(ReleaseInverseToneCurveLUTRenderState)(
				[RenderThreadStateToRelease = MoveTemp(RenderThreadStateToRelease)](FRHICommandListImmediate&) mutable
				{
					RenderThreadStateToRelease.Reset();
				});
		}

		Assets.Empty();
	}

	FRHITexture* FInverseToneCurveLUTCache::GetRHI_RenderThread(bool bInvertExpandGamut)
	{
		checkSlow(IsInRenderingThread());
		TSharedPtr<FInverseToneCurveLUTRenderThreadState, ESPMode::ThreadSafe> LocalRenderThreadState = RenderThreadState;
		if (!LocalRenderThreadState.IsValid() || LocalRenderThreadState->bShuttingDown)
		{
			return nullptr;
		}

		// Derive the variant from the bool for now.
		const EInverseToneCurveLUTVariant Variant = static_cast<EInverseToneCurveLUTVariant>(bInvertExpandGamut);
		FInverseToneCurveLUTRenderThreadCacheSlot& Slot = LocalRenderThreadState->CacheSlots.FindOrAdd(Variant);

		if (Slot.TextureReferenceRHI)
		{
			if (FRHITexture* ReferencedTexture = Slot.TextureReferenceRHI->GetReferencedTexture())
			{
				if (FRHITexture* ReferencedTexture3D = ReferencedTexture->GetTexture3D())
				{
					return ReferencedTexture3D;
				}
			}

			// Keep the stable reference cached. The engine may update what it references after
			// load/streaming/resource recreation; resolve the actual texture again next frame.
			return nullptr;
		}

		if (!Slot.bLoadRequested)
		{
			Slot.bLoadRequested = true;

			// Pinning the cache itself proves `this` (and Assets) is alive and keeps it so for the whole task.
			TWeakPtr<FInverseToneCurveLUTCache, ESPMode::ThreadSafe> WeakThis = AsWeak();
			AsyncTask(ENamedThreads::GameThread, [WeakThis, Variant]()
			{
				TSharedPtr<FInverseToneCurveLUTCache, ESPMode::ThreadSafe> PinnedThis = WeakThis.Pin();
				if (!PinnedThis.IsValid())
				{
					return;
				}

				const TCHAR* AssetPath = GLUTAssetPaths[Variant];
				TSoftObjectPtr<UVolumeTexture> SoftPtr{FSoftObjectPath(AssetPath)};

				UVolumeTexture* Loaded = SoftPtr.LoadSynchronous();
				if (!Loaded)
				{
					UE_LOGF(LogVGSSplat, Warning, "InverseToneCurveLUT asset not found at %ls; post-blend pass disabled.", AssetPath);
					return;
				}

				// The LUT is needed immediately by the render-thread pass. In editor, UVolumeTexture can
				// otherwise keep a temporary default/proxy resource while async texture compilation finishes.
#if WITH_EDITOR
				Loaded->BlockOnAnyAsyncBuild();
#endif // WITH_EDITOR

				PinnedThis->Assets.FindOrAdd(Variant).Reset(Loaded);
				FTextureReferenceRHIRef TextureReferenceRHI = Loaded->TextureReference.TextureReferenceRHI;

				// Render state has its own render-thread lifetime, so the capture command still guards on it.
				TWeakPtr<FInverseToneCurveLUTRenderThreadState, ESPMode::ThreadSafe> WeakRenderThreadState = PinnedThis->RenderThreadState;
				ENQUEUE_RENDER_COMMAND(CaptureInverseToneCurveLUTReference)(
					[WeakRenderThreadState, Variant, TextureReferenceRHI](FRHICommandListImmediate&)
					{
						TSharedPtr<FInverseToneCurveLUTRenderThreadState, ESPMode::ThreadSafe> LocalRenderThreadState = WeakRenderThreadState.Pin();
						if (!LocalRenderThreadState.IsValid() || LocalRenderThreadState->bShuttingDown)
						{
							return;
						}

						FInverseToneCurveLUTRenderThreadCacheSlot& Slot = LocalRenderThreadState->CacheSlots.FindOrAdd(Variant);
						if (TextureReferenceRHI)
						{
							Slot.TextureReferenceRHI = TextureReferenceRHI;
						}
						else
						{
							// The texture load can enqueue its own render-thread initialization work. If it has not
							// produced a texture reference yet, retry on a later frame instead of permanently gating off.
							Slot.bLoadRequested = false;
						}
					});
			});
		}

		return nullptr;
	}
}
