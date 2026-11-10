// VGS: a copy of Epic's GaussianSplat renderer (UE 5.9.0, CL 60351404), renamed by plugins/unreal/tools/copy_epic_splat_renderer.py.
// What VGS changed is marked "VGS CHANGE"; see plugins/unreal/EPIC_RENDERER_COPY.md.
// Copyright Epic Games, Inc. All Rights Reserved.

#include "VGSSplatSubsystem.h"

#include "VGSSplatViewExtension.h"
//#include "VGSSplatsModule.h"
//#include "VGSSplatsActor.h"

#include "Engine/World.h"
#include "EngineUtils.h"


void UVGSSplatSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	SceneViewExtension = FSceneViewExtensions::NewExtension<FVGSSplatViewExtension>(GetWorld());
}

void UVGSSplatSubsystem::Deinitialize()
{
	// Due to a flaw in SVE APIs, it may be kept alive unless we explicitely deactivate it.
	//SceneViewExtension->bIsActive = false;
	SceneViewExtension.Reset();
}

bool UVGSSplatSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	if (Super::ShouldCreateSubsystem(Outer))
	{
		return true;
	}

	if (const UWorld* World = Cast<UWorld>(Outer))
	{
		return World->WorldType == EWorldType::EditorPreview;
	}

	return false;
}
