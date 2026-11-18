#include "PGSSplatComponent.h"

#include "VGSSplatLayout.h"
#include "PGSSplat.h"
#include "PGSSplatGpu.h"
#include "PGSSplatSceneProxy.h"
#include "PGSSplatSource.h"
#include "PGSSplatViewExtension.h" // PGSSplat::RegisterExternalProxySource (PGSSplatRenderer)
#include "PGSSplatShadowVertexFactory.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Misc/App.h"
#include "RenderingThread.h"
#include "SceneInterface.h"

#if WITH_EDITOR
#include "UObject/UnrealType.h"
#endif

#include UE_INLINE_GENERATED_CPP_BY_NAME(PGSSplatComponent)

UPGSSplatComponent::UPGSSplatComponent()
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
	FPGSShadowSettings ShadowSettingsOf(const FMatrix& FileToComponent, const UPGSSplatComponent& Component)
	{
		FPGSShadowSettings Settings;
		Settings.FileToComponent = FMatrix44f(FileToComponent);
		Settings.PointScale = Component.PointScaleFactor;
		Settings.OpacityScale = Component.OpacityScale;
		Settings.Inset = Component.ShadowInset;
		return Settings;
	}
}

FPrimitiveSceneProxy* UPGSSplatComponent::CreateSceneProxy()
{
	const FPGSShadowSettings Settings = ShadowSettingsOf(FileToComponent(), *this);
	ProxyState.SetNumUninitialized(sizeof(Settings));
	FMemory::Memcpy(ProxyState.GetData(), &Settings, sizeof(Settings));
	// The collision mesh never casts shadows; native casters use the splats.
	TSharedPtr<FPGSSplatGpu, ESPMode::ThreadSafe> Gpu = Splat ? Splat->GetGpu() : nullptr;
	if (!Gpu.IsValid() || Gpu->IsEmpty())
	{
		return nullptr;
	}
	return new FPGSSplatSceneProxy(this, MoveTemp(Gpu), Settings);
}

void UPGSSplatComponent::SetSplat(UPGSSplat* NewSplat)
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
		ENQUEUE_RENDER_COMMAND(PGSSetGpu)([Target = Source, Gpu = Splat ? Splat->GetGpu() : nullptr](FRHICommandList&)
		{
			Target->SetGpu_RenderThread(Gpu);
		});
	}
	PushRenderState(true);
}

void UPGSSplatComponent::RefreshInfo()
{
	const TSharedPtr<const FPGSSplatData, ESPMode::ThreadSafe> Data = Splat ? Splat->GetData() : nullptr;
	Splats = Data.IsValid() ? int64(Data->NumSplats) : 0;
	SHDegree = Data.IsValid() ? FMath::Max(vgsunreal::shDegree(Data->SHCoefficients), 0) : 0;
	if (!Data.IsValid())
	{
		Normals = Splat ? TEXT("No splats in the asset") : FString();
	}
	else if (Splat->Normals == EPGSNormals::None)
	{
		Normals = TEXT("None: Shaded lights each splat by its shortest axis, which is much noisier");
	}
	else
	{
		Normals = StaticEnum<EPGSNormals>()->GetDisplayNameTextByValue(int64(Splat->Normals)).ToString();
		if (!Splat->NormalsNote.IsEmpty())
		{
			Normals += TEXT(": ") + Splat->NormalsNote;
		}
	}
}

FMatrix UPGSSplatComponent::FileToComponent() const
{
	return FileToComponent(UpAxis, UnitScale);
}

FMatrix UPGSSplatComponent::FileToComponent(EPGSUpAxis InUpAxis, float InUnitScale)
{
	float Rows[3][3] = {};
	const float S = InUnitScale;
	if (InUpAxis == EPGSUpAxis::YDown)
	{
		Rows[0][1] = S;  // file +X (right)   -> Unreal +Y (right)
		Rows[1][2] = -S; // file +Y (down)    -> Unreal -Z
		Rows[2][0] = S;  // file +Z (forward) -> Unreal +X (forward)
	}
	else
	{
		vgsunreal::captureToUnreal(InUpAxis == EPGSUpAxis::Z ? vgsunreal::UpAxis::Z : vgsunreal::UpAxis::Y, S, Rows);
	}
	return FMatrix(
		FPlane(Rows[0][0], Rows[0][1], Rows[0][2], 0.0),
		FPlane(Rows[1][0], Rows[1][1], Rows[1][2], 0.0),
		FPlane(Rows[2][0], Rows[2][1], Rows[2][2], 0.0),
		FPlane(0.0, 0.0, 0.0, 1.0));
}

FTransform UPGSSplatComponent::ProxyToComponent() const
{
	// Both are mirrors (a right-handed file into Unreal's left-handed axes): what is left is a
	// turn and a uniform scale, which a transform holds.
	return FTransform(FileToComponent(EPGSUpAxis::Y, 100.0f).Inverse() * FileToComponent());
}

void UPGSSplatComponent::UpdateProxyComponent()
{
	UStaticMesh* Mesh = Splat ? Splat->ShadowProxy.Get() : nullptr;
	const bool bCollision = Mesh && bProxyCollision;
	UWorld* World = GetWorld();
	if (!bCollision || !IsRegistered() || !World || IsTemplate())
	{
		if (ProxyComponent)
		{
			ProxyComponent->DestroyComponent();
			ProxyComponent = nullptr;
		}
		return;
	}
	if (!ProxyComponent)
	{
		AActor* Owner = GetOwner();
		ProxyComponent = NewObject<UStaticMeshComponent>(Owner ? static_cast<UObject*>(Owner) : static_cast<UObject*>(this), NAME_None,
			RF_Transient | RF_TextExportTransient | RF_DuplicateTransient);
		ProxyComponent->SetupAttachment(this);
		ProxyComponent->SetMobility(EComponentMobility::Movable);
		// Collision only: never draw the approximate mesh into depth, lighting or shadow passes.
		ProxyComponent->SetRenderInMainPass(false);
		ProxyComponent->SetRenderInDepthPass(false);
        ProxyComponent->SetVisibility(false);
        ProxyComponent->SetVisibleInRayTracing(false);
        ProxyComponent->bAffectDistanceFieldLighting = false;
        ProxyComponent->bAffectDynamicIndirectLighting = false;
		ProxyComponent->bCastDynamicShadow = false;
		ProxyComponent->bCastStaticShadow = false;
		ProxyComponent->bSelectable = false;
		ProxyComponent->SetCanEverAffectNavigation(false);
		ProxyComponent->SetGenerateOverlapEvents(false);
		ProxyComponent->SetStaticMesh(Mesh);
		ProxyComponent->SetRelativeTransform(ProxyToComponent());
		ProxyComponent->SetCastShadow(false);
		ProxyComponent->SetCollisionProfileName(bCollision ? UCollisionProfile::BlockAll_ProfileName : UCollisionProfile::NoCollision_ProfileName);
		if (Owner)
		{
			ProxyComponent->RegisterComponent();
		}
		else
		{
			ProxyComponent->RegisterComponentWithWorld(World);
		}
	}
	ProxyComponent->SetStaticMesh(Mesh);
	const FTransform Relative = ProxyToComponent();
	if (!ProxyComponent->GetRelativeTransform().Equals(Relative))
	{
		ProxyComponent->SetRelativeTransform(Relative);
	}
	const FName Profile = bCollision ? UCollisionProfile::BlockAll_ProfileName : UCollisionProfile::NoCollision_ProfileName;
	if (ProxyComponent->GetCollisionProfileName() != Profile)
	{
		ProxyComponent->SetCollisionProfileName(Profile);
	}

}

void UPGSSplatComponent::PushRenderState(bool bForce)
{
	// The shadow casters' own copy of what shapes them: a new proxy when it changed.
	const FPGSShadowSettings Shadows = ShadowSettingsOf(FileToComponent(), *this);
	if (ProxyState.Num() == sizeof(Shadows) && FMemory::Memcmp(ProxyState.GetData(), &Shadows, sizeof(Shadows)) != 0)
	{
		MarkRenderStateDirty();
	}
	UpdateProxyComponent();

	if (!Source.IsValid())
	{
		return;
	}

	FPGSLook Look;
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
	Look.bBlendInSRGB = BlendSpace == EPGSBlendSpace::SRGB ? 1u : 0u;
	Look.bLit = Lighting == EPGSLighting::Shaded ? 1u : 0u;
	Look.SelfIllumination = FMath::Clamp(SelfIlluminationAmount, 0.0f, 1.0f);
	Look.Roughness = FMath::Clamp(Roughness, 0.0f, 1.0f);
	Look.Specular = FMath::Clamp(Specular, 0.0f, 1.0f);
	Look.NormalSmoothing = FMath::Max(NormalSmoothing, 0.0f);
	Look.bCastShadow = CastShadow && bCastDynamicShadow
		&& PGSSplat::GetShadowMethod() != PGSSplat::EShadowMethod::None ? 1u : 0u;
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
	ENQUEUE_RENDER_COMMAND(PGSSetLook)([Target = Source, Look](FRHICommandList&)
	{
		Target->SetLook_RenderThread(Look);
	});
}

void UPGSSplatComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	PushRenderState(false);
}

void UPGSSplatComponent::OnSplatChanged(UPGSSplat* Changed)
{
	if (Changed == Splat && Source.IsValid())
	{
		// Reimported, or its layout changed: the GPU copy is a new one.
		RefreshInfo();
		UpdateBounds();
		ENQUEUE_RENDER_COMMAND(PGSSetGpu)([Target = Source, Gpu = Splat->GetGpu()](FRHICommandList&)
		{
			Target->SetGpu_RenderThread(Gpu);
		});
		MarkRenderStateDirty();
	}
}

void UPGSSplatComponent::RegisterSource()
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
	Source = MakeShared<FPGSSplatSource, ESPMode::ThreadSafe>();
	PGSSplat::RegisterExternalProxySource(World,
		FOnCollectPGSSplatProxies::FDelegate::CreateSP(Source.ToSharedRef(), &FPGSSplatSource::Collect_RenderThread));
	ENQUEUE_RENDER_COMMAND(PGSSetGpu)([Target = Source, Gpu = Splat ? Splat->GetGpu() : nullptr](FRHICommandList&)
	{
		Target->SetGpu_RenderThread(Gpu);
	});
	ChangedHandle = UPGSSplat::OnChanged.AddUObject(this, &UPGSSplatComponent::OnSplatChanged);
	PushRenderState(true);
}

void UPGSSplatComponent::ReleaseSource()
{
	UPGSSplat::OnChanged.Remove(ChangedHandle);
	ChangedHandle.Reset();
	if (!Source.IsValid())
	{
		return;
	}
	// The renderer's binding expires with the source; letting go of it on the render thread
	// keeps the renderer from calling into it half destroyed.
	ENQUEUE_RENDER_COMMAND(PGSReleaseSource)([Released = MoveTemp(Source)](FRHICommandList&) mutable
	{
		Released.Reset();
	});
	Source.Reset();
	SentState.Reset();
}

void UPGSSplatComponent::OnRegister()
{
	Super::OnRegister();
	RefreshInfo();
	RegisterSource();
	UpdateProxyComponent();
}

void UPGSSplatComponent::OnUnregister()
{
	ReleaseSource();
	if (ProxyComponent)
	{
		ProxyComponent->DestroyComponent();
		ProxyComponent = nullptr;
	}
	Super::OnUnregister();
}

void UPGSSplatComponent::BeginDestroy()
{
	ReleaseSource();
	Super::BeginDestroy();
}

void UPGSSplatComponent::OnUpdateTransform(EUpdateTransformFlags UpdateTransformFlags, ETeleportType Teleport)
{
	Super::OnUpdateTransform(UpdateTransformFlags, Teleport);
	PushRenderState(false);
}

void UPGSSplatComponent::OnVisibilityChanged()
{
	Super::OnVisibilityChanged();
	PushRenderState(false);
}

void UPGSSplatComponent::OnHiddenInGameChanged()
{
	Super::OnHiddenInGameChanged();
	PushRenderState(false);
}

FBoxSphereBounds UPGSSplatComponent::CalcBounds(const FTransform& LocalToWorld) const
{
	const TSharedPtr<const FPGSSplatData, ESPMode::ThreadSafe> Data = Splat ? Splat->GetData() : nullptr;
	if (Data.IsValid() && Data->Bounds.IsValid)
	{
		const FBox Box(FVector(Data->Bounds.Min), FVector(Data->Bounds.Max));
		return FBoxSphereBounds(Box.TransformBy(FileToComponent() * LocalToWorld.ToMatrixWithScale()));
	}
	return FBoxSphereBounds(LocalToWorld.GetLocation(), FVector(50.0), 50.0);
}

#if WITH_EDITOR
bool UPGSSplatComponent::CanEditChange(const FProperty* InProperty) const
{
	if (!Super::CanEditChange(InProperty))
	{
		return false;
	}
	const FName Name = InProperty ? InProperty->GetFName() : NAME_None;
	if (Name == GET_MEMBER_NAME_CHECKED(UPGSSplatComponent, bProxyCollision))
	{
		return Splat && Splat->ShadowProxy;
	}
	if (Name == GET_MEMBER_NAME_CHECKED(UPGSSplatComponent, ShadowInset))
	{
		return CastShadow && bCastDynamicShadow
			&& PGSSplat::GetShadowMethod() != PGSSplat::EShadowMethod::None;
	}
	return true;
}

void UPGSSplatComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	if (PropertyChangedEvent.GetMemberPropertyName() == GET_MEMBER_NAME_CHECKED(UPGSSplatComponent, Splat))
	{
		RefreshInfo();
		if (Source.IsValid())
		{
			ENQUEUE_RENDER_COMMAND(PGSSetGpu)([Target = Source, Gpu = Splat ? Splat->GetGpu() : nullptr](FRHICommandList&)
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
