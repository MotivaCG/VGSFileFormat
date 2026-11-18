// VGS: binds a depth-aware visibility function to the sun's render proxy. The light
// component and its saved material are never changed. Existing user functions are kept.
#include "VGSSplatSubsystem.h"
#include "VGSSplatShadowVertexFactory.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "LightSceneProxy.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "SceneView.h"
#include "UObject/ConstructorHelpers.h"
#include "UObject/UObjectIterator.h"

namespace
{
	const TCHAR* ShadowSlot() { return FCString::Strcmp(VGSSPLAT_PLUGIN_NAME, TEXT("VGS")) == 0 ? TEXT("A") : TEXT("B"); }
	FName ShadowParameter(const TCHAR* Base) { return FName(*(FString(Base) + ShadowSlot())); }
}

UVGSSplatSubsystem::UVGSSplatSubsystem()
{
	const FString Path = FString::Printf(TEXT("/%s/Materials/M_SplatShadowLightFunction.M_SplatShadowLightFunction"), VGSSPLAT_PLUGIN_NAME);
	ConstructorHelpers::FObjectFinderOptional<UMaterialInterface> Material(*Path);
	ShadowMaterial = Material.Get();
}

void UVGSSplatSubsystem::ReleaseShadowLightFunction()
{
	if (ShadowInstance) ShadowInstance->SetScalarParameterValue(ShadowParameter(TEXT("Enabled")), 0.0f);
	const TCHAR* Other = FCString::Strcmp(ShadowSlot(), TEXT("A")) == 0 ? TEXT("EnabledB") : TEXT("EnabledA");
	if (UDirectionalLightComponent* Light = ShadowLight.Get())
	{
		if (Light->SceneProxy && (!ShadowInstance || ShadowInstance->K2_GetScalarParameterValue(Other) < 0.5f))
			Light->SceneProxy->SetLightFunctionMaterial_GameThread(Light);
	}
	ShadowLight.Reset();
}

VGSSplat::FDeepShadowFrame UVGSSplatSubsystem::UpdateShadowLightFunction(const FSceneViewFamily& Family)
{
	VGSSplat::FDeepShadowFrame Frame;
	const IConsoleVariable* Enabled = IConsoleManager::Get().FindConsoleVariable(TEXT("r.VGSSplat"));
	if ((Enabled && Enabled->GetInt() == 0) || !VGSSplat::UsesSplatShadowMap() || !ShadowMaterial || !Family.RenderTarget || !Family.EngineShowFlags.LightFunctions)
	{
		ReleaseShadowLightFunction();
		return Frame;
	}
	UDirectionalLightComponent* Main = nullptr;
	float Brightest = -1.0f;
	for (TObjectIterator<UDirectionalLightComponent> It; It; ++It)
	{
		UDirectionalLightComponent* Light = *It;
		if (Light->GetWorld() != GetWorld() || !Light->IsRegistered() || !Light->IsVisible() ||
			!Light->CastsDynamicShadow() || !Light->SceneProxy) continue;
		if (Light->bAtmosphereSunLight && Light->AtmosphereSunLightIndex == 0) { Main = Light; break; }
		const float Value = Light->Intensity * Light->GetLightColor().GetLuminance();
		if (Value > Brightest) { Main = Light; Brightest = Value; }
	}
	if (Main != ShadowLight.Get()) ReleaseShadowLightFunction();
	if (!Main || Main->LightFunctionMaterial) { ReleaseShadowLightFunction(); return Frame; }
	FBox Bounds(ForceInit);
	const FName ComponentClass(VGSSPLAT_PLUGIN_NAME == FString(TEXT("VGS")) ? TEXT("VGSCaptureComponent") : TEXT("PGSSplatComponent"));
	for (TObjectIterator<UPrimitiveComponent> It; It; ++It)
	{
		if (It->GetWorld() != GetWorld() || !It->IsRegistered() || !It->IsVisible() || !It->CastShadow) continue;
		for (const UClass* Class = It->GetClass(); Class; Class = Class->GetSuperClass())
			if (Class->GetFName() == ComponentClass) { Bounds += It->Bounds.GetBox(); break; }
	}
	if (!Bounds.IsValid) { ReleaseShadowLightFunction(); return Frame; }
	Frame.Z = Main->GetForwardVector().GetSafeNormal();
	Frame.Z.FindBestAxisVectors(Frame.X, Frame.Y);
	Frame.Origin = Bounds.GetCenter();
	Frame.Extent = FVector::ZeroVector;
	for (int32 Corner = 0; Corner < 8; ++Corner)
	{
		const FVector P((Corner & 1) ? Bounds.Max.X : Bounds.Min.X, (Corner & 2) ? Bounds.Max.Y : Bounds.Min.Y, (Corner & 4) ? Bounds.Max.Z : Bounds.Min.Z);
		const FVector D = P - Frame.Origin;
		Frame.Extent.X = FMath::Max(Frame.Extent.X, FMath::Abs(FVector::DotProduct(D, Frame.X)));
		Frame.Extent.Y = FMath::Max(Frame.Extent.Y, FMath::Abs(FVector::DotProduct(D, Frame.Y)));
		Frame.Extent.Z = FMath::Max(Frame.Extent.Z, FMath::Abs(FVector::DotProduct(D, Frame.Z)));
	}
	Frame.Extent = Frame.Extent * 1.1 + FVector(10.0);
	const auto CVar = [](const TCHAR* Name, float Default) { const IConsoleVariable* V = IConsoleManager::Get().FindConsoleVariable(Name); return V ? V->GetFloat() : Default; };
	const float Texel = FMath::Max(CVar(TEXT("r.VGSSplat.Shadow.TexelSize"), 0.5f), 0.05f);
	const int32 Limit = FMath::Clamp(int32(CVar(TEXT("r.VGSSplat.Shadow.MaxMapSize"), 1024)), 64, 1024);
	Frame.Resolution = FMath::Clamp(Align(FMath::CeilToInt(2.0 * FMath::Max(Frame.Extent.X, Frame.Extent.Y) / Texel), 64), 64, Limit);
	const auto Ensure = [this](TObjectPtr<UTextureRenderTarget2D>& Texture, FIntPoint Size, EPixelFormat Format)
	{
		if (!Texture || Texture->SizeX != Size.X || Texture->SizeY != Size.Y)
		{
			Texture = NewObject<UTextureRenderTarget2D>(this);
			Texture->bCanCreateUAV = true;
			Texture->ClearColor = FLinearColor::Black;
			Texture->InitCustomFormat(Size.X, Size.Y, Format, true);
			Texture->UpdateResourceImmediate(true);
		}
	};
	Ensure(ShadowAtlas, FIntPoint(Frame.Resolution * 8, Frame.Resolution * 9), PF_R16F);
	Ensure(ShadowReceivers, Family.RenderTarget->GetSizeXY(), PF_R32_FLOAT);
	if (!ShadowInstance)
	{
		// Both plugins share one visibility function, with independent grids in slots A/B.
		// Finding the MID by its world-local name avoids a link dependency between plugins.
		ShadowInstance = FindObject<UMaterialInstanceDynamic>(GetWorld(), TEXT("VGSShadowLightVisibility"));
		if (!ShadowInstance) ShadowInstance = UMaterialInstanceDynamic::Create(ShadowMaterial, GetWorld(), TEXT("VGSShadowLightVisibility"));
	}
	if (!ParameterSource)
	{
		ParameterSource = NewObject<UDirectionalLightComponent>(this);
		ParameterSource->LightFunctionMaterial = ShadowInstance;
	}
	ShadowInstance->SetTextureParameterValue(ShadowParameter(TEXT("ShadowAtlas")), ShadowAtlas);
	ShadowInstance->SetTextureParameterValue(ShadowParameter(TEXT("CaptureMask")), ShadowReceivers);
	const auto Vector = [this](const TCHAR* Name, const FVector& V) { ShadowInstance->SetVectorParameterValue(ShadowParameter(Name), FLinearColor(float(V.X), float(V.Y), float(V.Z), 0)); };
	Vector(TEXT("GridOrigin"), Frame.Origin);
	Vector(TEXT("GridX"), Frame.X); Vector(TEXT("GridY"), Frame.Y); Vector(TEXT("GridZ"), Frame.Z);
	Vector(TEXT("GridExtent"), Frame.Extent);
	ShadowInstance->SetScalarParameterValue(ShadowParameter(TEXT("GridResolution")), float(Frame.Resolution));
	ShadowInstance->SetScalarParameterValue(ShadowParameter(TEXT("DepthBias")), FMath::Max(CVar(TEXT("r.VGSSplat.Shadow.Bias"), 3), 0.0f));
	ShadowInstance->SetScalarParameterValue(ShadowParameter(TEXT("SelfShadow")), CVar(TEXT("r.VGSSplat.Shadow.Self"), 0));
	ShadowInstance->SetScalarParameterValue(ShadowParameter(TEXT("Penumbra")), FMath::Tan(FMath::DegreesToRadians(Main->LightSourceAngle * 0.5f)) * CVar(TEXT("r.VGSSplat.Shadow.Softness"), 1));
	ShadowInstance->SetScalarParameterValue(ShadowParameter(TEXT("Enabled")), 1.0f);
	Main->SceneProxy->SetLightFunctionMaterial_GameThread(ParameterSource);
	ShadowLight = Main;
	Frame.Atlas = ShadowAtlas->GameThread_GetRenderTargetResource();
	Frame.Receivers = ShadowReceivers->GameThread_GetRenderTargetResource();
	return Frame;
}
