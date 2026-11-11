#include "VGSStaticSplatComponent.h"

#include "VGSSplatLayout.h"
#include "VGSStaticSplat.h"
#include "VGSStaticSplatGpu.h"
#include "VGSStaticSplatSceneProxy.h"
#include "VGSStaticSplatSource.h"
#include "VGSStaticSplatViewExtension.h" // VGSStaticSplat::RegisterExternalProxySource (VGSStaticSplatRenderer)

#include "Engine/CollisionProfile.h"
#include "Engine/World.h"
#include "Misc/App.h"
#include "RenderingThread.h"
#include "SceneInterface.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(VGSStaticSplatComponent)

UVGSStaticSplatComponent::UVGSStaticSplatComponent()
{
	// Only to notice settings changed from Blueprint: comparing a few dozen bytes, and sending
	// them to the render thread only when they differ. Transform and visibility changes are
	// sent as they happen.
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
	PrimaryComponentTick.TickGroup = TG_PostUpdateWork;
	bTickInEditor = true;

	// A primitive for its shadows only: no collision, navigation or occlusion.
	SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
	SetGenerateOverlapEvents(false);
	SetCanEverAffectNavigation(false);
	bUseAsOccluder = false;
	CastShadow = true;
	bCastDynamicShadow = true;
	bCastStaticShadow = false;
	bAffectDynamicIndirectLighting = false;
	bAffectDistanceFieldLighting = false;
}

namespace
{
	FVGSStaticShadowSettings ShadowSettingsOf(const FMatrix& FileToComponent, const UVGSStaticSplatComponent& Component)
	{
		FVGSStaticShadowSettings Settings;
		Settings.FileToComponent = FMatrix44f(FileToComponent);
		Settings.PointScale = Component.PointScaleFactor;
		Settings.OpacityScale = Component.OpacityScale;
		Settings.OpacityCutoff = Component.ShadowOpacityCutoff;
		Settings.Inset = Component.ShadowInset;
		return Settings;
	}
}

FPrimitiveSceneProxy* UVGSStaticSplatComponent::CreateSceneProxy()
{
	const FVGSStaticShadowSettings Settings = ShadowSettingsOf(FileToComponent(), *this);
	ProxyState.SetNumUninitialized(sizeof(Settings));
	FMemory::Memcpy(ProxyState.GetData(), &Settings, sizeof(Settings));
	TSharedPtr<FVGSStaticSplatGpu, ESPMode::ThreadSafe> Gpu = Splat ? Splat->GetGpu() : nullptr;
	if (!Gpu.IsValid() || Gpu->IsEmpty())
	{
		return nullptr;
	}
	return new FVGSStaticSplatSceneProxy(this, MoveTemp(Gpu), Settings);
}

void UVGSStaticSplatComponent::SetSplat(UVGSStaticSplat* NewSplat)
{
	if (Splat == NewSplat)
	{
		return;
	}
	Splat = NewSplat;
	RefreshInfo();
	UpdateBounds();
	MarkRenderStateDirty();
	if (Source.IsValid())
	{
		ENQUEUE_RENDER_COMMAND(VGSStaticSetGpu)([Target = Source, Gpu = Splat ? Splat->GetGpu() : nullptr](FRHICommandList&)
		{
			Target->SetGpu_RenderThread(Gpu);
		});
	}
	PushRenderState(true);
}

void UVGSStaticSplatComponent::RefreshInfo()
{
	const TSharedPtr<const FVGSStaticSplatData, ESPMode::ThreadSafe> Data = Splat ? Splat->GetData() : nullptr;
	Splats = Data.IsValid() ? int64(Data->NumSplats) : 0;
	SHDegree = Data.IsValid() ? FMath::Max(vgsunreal::shDegree(Data->SHCoefficients), 0) : 0;
	if (!Data.IsValid())
	{
		Normals = Splat ? TEXT("No splats in the asset") : FString();
	}
	else if (Splat->Normals == EVGSStaticNormals::None)
	{
		Normals = TEXT("None: Shaded lights each splat by its shortest axis, which is much noisier");
	}
	else
	{
		Normals = StaticEnum<EVGSStaticNormals>()->GetDisplayNameTextByValue(int64(Splat->Normals)).ToString();
		if (!Splat->NormalsNote.IsEmpty())
		{
			Normals += TEXT(": ") + Splat->NormalsNote;
		}
	}
}

FMatrix UVGSStaticSplatComponent::FileToComponent() const
{
	float Rows[3][3] = {};
	const float S = UnitScale;
	if (UpAxis == EVGSStaticUpAxis::YDown)
	{
		Rows[0][1] = S;  // file +X (right)   -> Unreal +Y (right)
		Rows[1][2] = -S; // file +Y (down)    -> Unreal -Z
		Rows[2][0] = S;  // file +Z (forward) -> Unreal +X (forward)
	}
	else
	{
		vgsunreal::captureToUnreal(UpAxis == EVGSStaticUpAxis::Z ? vgsunreal::UpAxis::Z : vgsunreal::UpAxis::Y, S, Rows);
	}
	return FMatrix(
		FPlane(Rows[0][0], Rows[0][1], Rows[0][2], 0.0),
		FPlane(Rows[1][0], Rows[1][1], Rows[1][2], 0.0),
		FPlane(Rows[2][0], Rows[2][1], Rows[2][2], 0.0),
		FPlane(0.0, 0.0, 0.0, 1.0));
}

void UVGSStaticSplatComponent::PushRenderState(bool bForce)
{
	// The shadow casters' own copy of what shapes them: a new proxy when it changed.
	const FVGSStaticShadowSettings Shadows = ShadowSettingsOf(FileToComponent(), *this);
	if (ProxyState.Num() == sizeof(Shadows) && FMemory::Memcmp(ProxyState.GetData(), &Shadows, sizeof(Shadows)) != 0)
	{
		MarkRenderStateDirty();
	}

	if (!Source.IsValid())
	{
		return;
	}

	FVGSStaticLook Look;
	FMemory::Memzero(Look); // so that two looks compare byte for byte
	// Row vectors: file axes and units first, then the component's transform.
	const FMatrix LocalToWorld = FileToComponent() * GetComponentTransform().ToMatrixWithScale();
	Look.LocalToWorld = FMatrix44f(LocalToWorld);
	Look.WorldToLocal = FMatrix44f(LocalToWorld.Inverse());
	Look.bVisible = ShouldRender() ? 1u : 0u;
	Look.bSphericalHarmonics = bSphericalHarmonics ? 1u : 0u;
	Look.PointScaleFactor = PointScaleFactor;
	Look.OpacityScale = OpacityScale;
	Look.InverseToneCurveStrength = bInvertToneCurve ? InverseToneCurveStrength : 0.0f;
	Look.bInvertExpandGamut = bInvertExpandGamut ? 1u : 0u;
	Look.Intensity = FMath::Exp2(IntensityEV);
	Look.bBlendInSRGB = BlendSpace == EVGSStaticBlendSpace::SRGB ? 1u : 0u;
	Look.bLit = Lighting == EVGSStaticLighting::Shaded ? 1u : 0u;
	Look.SelfIllumination = FMath::Clamp(SelfIlluminationAmount, 0.0f, 1.0f);
	Look.Roughness = FMath::Clamp(Roughness, 0.0f, 1.0f);
	Look.Specular = FMath::Clamp(Specular, 0.0f, 1.0f);
	Look.NormalSmoothing = FMath::Max(NormalSmoothing, 0.0f);
	Look.Grade = FVector3f(FMath::Max(Gamma, 0.05f), FMath::Max(Contrast, 0.0f), FMath::Max(Saturation, 0.0f));
	{
		// The grade works on sRGB values, so the tint (a linear colour, as the picker gives it)
		// is encoded the same way, as the VGS capture does.
		auto Encode = [](float L)
		{
			L = FMath::Max(L, 0.0f);
			return L <= 0.0031308f ? L * 12.92f : 1.055f * FMath::Pow(L, 1.0f / 2.4f) - 0.055f;
		};
		Look.Tint = FVector3f(Encode(Tint.R), Encode(Tint.G), Encode(Tint.B));
	}

	if (!bForce && SentState.Num() == sizeof(Look) && FMemory::Memcmp(SentState.GetData(), &Look, sizeof(Look)) == 0)
	{
		return;
	}
	SentState.SetNumUninitialized(sizeof(Look));
	FMemory::Memcpy(SentState.GetData(), &Look, sizeof(Look));
	ENQUEUE_RENDER_COMMAND(VGSStaticSetLook)([Target = Source, Look](FRHICommandList&)
	{
		Target->SetLook_RenderThread(Look);
	});
}

void UVGSStaticSplatComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	PushRenderState(false);
}

void UVGSStaticSplatComponent::OnSplatChanged(UVGSStaticSplat* Changed)
{
	if (Changed == Splat && Source.IsValid())
	{
		// Reimported, or its layout changed: the GPU copy is a new one.
		RefreshInfo();
		UpdateBounds();
		ENQUEUE_RENDER_COMMAND(VGSStaticSetGpu)([Target = Source, Gpu = Splat->GetGpu()](FRHICommandList&)
		{
			Target->SetGpu_RenderThread(Gpu);
		});
		MarkRenderStateDirty();
	}
}

void UVGSStaticSplatComponent::RegisterSource()
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
	Source = MakeShared<FVGSStaticSplatSource, ESPMode::ThreadSafe>();
	VGSStaticSplat::RegisterExternalProxySource(World,
		FOnCollectVGSStaticSplatProxies::FDelegate::CreateSP(Source.ToSharedRef(), &FVGSStaticSplatSource::Collect_RenderThread));
	ENQUEUE_RENDER_COMMAND(VGSStaticSetGpu)([Target = Source, Gpu = Splat ? Splat->GetGpu() : nullptr](FRHICommandList&)
	{
		Target->SetGpu_RenderThread(Gpu);
	});
	ChangedHandle = UVGSStaticSplat::OnChanged.AddUObject(this, &UVGSStaticSplatComponent::OnSplatChanged);
	PushRenderState(true);
}

void UVGSStaticSplatComponent::ReleaseSource()
{
	UVGSStaticSplat::OnChanged.Remove(ChangedHandle);
	ChangedHandle.Reset();
	if (!Source.IsValid())
	{
		return;
	}
	// The renderer's binding expires with the source; letting go of it on the render thread
	// keeps the renderer from calling into it half destroyed.
	ENQUEUE_RENDER_COMMAND(VGSStaticReleaseSource)([Released = MoveTemp(Source)](FRHICommandList&) mutable
	{
		Released.Reset();
	});
	Source.Reset();
	SentState.Reset();
}

void UVGSStaticSplatComponent::OnRegister()
{
	Super::OnRegister();
	RefreshInfo();
	RegisterSource();
}

void UVGSStaticSplatComponent::OnUnregister()
{
	ReleaseSource();
	Super::OnUnregister();
}

void UVGSStaticSplatComponent::BeginDestroy()
{
	ReleaseSource();
	Super::BeginDestroy();
}

void UVGSStaticSplatComponent::OnUpdateTransform(EUpdateTransformFlags UpdateTransformFlags, ETeleportType Teleport)
{
	Super::OnUpdateTransform(UpdateTransformFlags, Teleport);
	PushRenderState(false);
}

void UVGSStaticSplatComponent::OnVisibilityChanged()
{
	Super::OnVisibilityChanged();
	PushRenderState(false);
}

void UVGSStaticSplatComponent::OnHiddenInGameChanged()
{
	Super::OnHiddenInGameChanged();
	PushRenderState(false);
}

FBoxSphereBounds UVGSStaticSplatComponent::CalcBounds(const FTransform& LocalToWorld) const
{
	const TSharedPtr<const FVGSStaticSplatData, ESPMode::ThreadSafe> Data = Splat ? Splat->GetData() : nullptr;
	if (Data.IsValid() && Data->Bounds.IsValid)
	{
		const FBox Box(FVector(Data->Bounds.Min), FVector(Data->Bounds.Max));
		return FBoxSphereBounds(Box.TransformBy(FileToComponent() * LocalToWorld.ToMatrixWithScale()));
	}
	return FBoxSphereBounds(LocalToWorld.GetLocation(), FVector(50.0), 50.0);
}

#if WITH_EDITOR
void UVGSStaticSplatComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	if (PropertyChangedEvent.GetMemberPropertyName() == GET_MEMBER_NAME_CHECKED(UVGSStaticSplatComponent, Splat))
	{
		RefreshInfo();
		if (Source.IsValid())
		{
			ENQUEUE_RENDER_COMMAND(VGSStaticSetGpu)([Target = Source, Gpu = Splat ? Splat->GetGpu() : nullptr](FRHICommandList&)
			{
				Target->SetGpu_RenderThread(Gpu);
			});
		}
	}
	UpdateBounds();
	MarkRenderStateDirty();
	PushRenderState(false);
}
#endif
