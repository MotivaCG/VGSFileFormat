#include "VGSStaticEditorTypes.h"

#include "VGSStaticSplatActor.h"
#include "VGSStaticSplatComponent.h"

#include "Modules/ModuleManager.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(VGSStaticEditorTypes)

#define LOCTEXT_NAMESPACE "VGSStaticEditor"

UVGSStaticSplatActorFactory::UVGSStaticSplatActorFactory()
{
	DisplayName = LOCTEXT("ActorFactory", "VGS Static Splat");
	NewActorClass = AVGSStaticSplatActor::StaticClass();
	bUseSurfaceOrientation = true;
}

bool UVGSStaticSplatActorFactory::CanCreateActorFrom(const FAssetData& AssetData, FText& OutErrorMsg)
{
	if (!AssetData.IsValid() || !AssetData.IsInstanceOf(UVGSStaticSplat::StaticClass()))
	{
		OutErrorMsg = LOCTEXT("NoAsset", "No VGS Static Splat asset selected");
		return false;
	}
	return true;
}

void UVGSStaticSplatActorFactory::PostSpawnActor(UObject* Asset, AActor* NewActor)
{
	Super::PostSpawnActor(Asset, NewActor);
	AVGSStaticSplatActor* Actor = CastChecked<AVGSStaticSplatActor>(NewActor);
	if (Actor->SplatComponent)
	{
		Actor->SplatComponent->SetSplat(CastChecked<UVGSStaticSplat>(Asset));
	}
}

UObject* UVGSStaticSplatActorFactory::GetAssetFromActorInstance(AActor* ActorInstance)
{
	const AVGSStaticSplatActor* Actor = Cast<AVGSStaticSplatActor>(ActorInstance);
	return Actor && Actor->SplatComponent ? Actor->SplatComponent->Splat.Get() : nullptr;
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FDefaultModuleImpl, VGSStaticEditor)
