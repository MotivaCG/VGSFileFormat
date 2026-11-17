// VGS: a copy of Epic's GaussianSplat renderer (UE 5.9.0, CL 60351404), renamed by plugins/unreal/tools/copy_epic_splat_renderer.py.
// What VGS changed is marked "VGS CHANGE"; see plugins/unreal/EPIC_RENDERER_COPY.md.
// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Subsystems/WorldSubsystem.h"
#include "Engine/EngineBaseTypes.h"
#include "VGSSplatLightShadow.h"

#if WITH_EDITOR
#include "EditorUndoClient.h"
#endif

#include "VGSSplatSubsystem.generated.h"

class FVGSSplatViewExtension;
class UMaterialInterface;
class UMaterialInstanceDynamic;
class UTextureRenderTarget2D;
class UDirectionalLightComponent;

UCLASS(MinimalAPI)
class UVGSSplatSubsystem : public UWorldSubsystem
{
public:
	GENERATED_BODY()
	UVGSSplatSubsystem();

public:

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;

	TSharedPtr<FVGSSplatViewExtension> SceneViewExtension;
	VGSSplat::FDeepShadowFrame UpdateShadowLightFunction(const FSceneViewFamily& Family);
private:
	UPROPERTY() TObjectPtr<UMaterialInterface> ShadowMaterial;
	UPROPERTY() TObjectPtr<UMaterialInstanceDynamic> ShadowInstance;
	UPROPERTY() TObjectPtr<UTextureRenderTarget2D> ShadowAtlas;
	UPROPERTY() TObjectPtr<UTextureRenderTarget2D> ShadowReceivers;
	UPROPERTY() TObjectPtr<UDirectionalLightComponent> ParameterSource;
	TWeakObjectPtr<UDirectionalLightComponent> ShadowLight;
	void ReleaseShadowLightFunction();
};
