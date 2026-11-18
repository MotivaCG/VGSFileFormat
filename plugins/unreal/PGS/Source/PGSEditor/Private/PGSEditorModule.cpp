#include "PGSEditorTypes.h"
#include "PGSThumbnailRenderer.h"
#include "ThumbnailRendering/ThumbnailManager.h"

#include "PGSSplatActor.h"
#include "PGSSplatComponent.h"

#include "Modules/ModuleManager.h"
#include "Brushes/SlateImageBrush.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "Styling/SlateStyle.h"
#include "Styling/SlateStyleRegistry.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(PGSEditorTypes)

#define LOCTEXT_NAMESPACE "PGSEditor"

UPGSSplatActorFactory::UPGSSplatActorFactory()
{
	DisplayName = LOCTEXT("ActorFactory", "PGS (Plain Gaussian Splatting)");
	NewActorClass = APGSSplatActor::StaticClass();
	bUseSurfaceOrientation = true;
}

bool UPGSSplatActorFactory::CanCreateActorFrom(const FAssetData& AssetData, FText& OutErrorMsg)
{
	if (!AssetData.IsValid() || !AssetData.IsInstanceOf(UPGSSplat::StaticClass()))
	{
		OutErrorMsg = LOCTEXT("NoAsset", "No PGS (Plain Gaussian Splatting) asset selected");
		return false;
	}
	return true;
}

void UPGSSplatActorFactory::PostSpawnActor(UObject* Asset, AActor* NewActor)
{
	Super::PostSpawnActor(Asset, NewActor);
	APGSSplatActor* Actor = CastChecked<APGSSplatActor>(NewActor);
	if (Actor->SplatComponent)
	{
		Actor->SplatComponent->SetSplat(CastChecked<UPGSSplat>(Asset));
	}
}

UObject* UPGSSplatActorFactory::GetAssetFromActorInstance(AActor* ActorInstance)
{
	const APGSSplatActor* Actor = Cast<APGSSplatActor>(ActorInstance);
	return Actor && Actor->SplatComponent ? Actor->SplatComponent->Splat.Get() : nullptr;
}

#undef LOCTEXT_NAMESPACE

class FPGSEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		UThumbnailManager::Get().RegisterCustomRenderer(UPGSSplat::StaticClass(), UPGSThumbnailRenderer::StaticClass());
		const auto Plugin = IPluginManager::Get().FindPlugin(TEXT("PGS"));
		if (!Plugin) return;
		Icons = MakeShared<FSlateStyleSet>(TEXT("PGSActorIcons"));
		const FString Icon = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Resources/IconPGS.png"));
		Icons->Set("ClassIcon.PGSSplatActor", new FSlateImageBrush(Icon, FVector2D(16, 16)));
		Icons->Set("ClassThumbnail.PGSSplatActor", new FSlateImageBrush(Icon, FVector2D(64, 64)));
		Icons->Set("ClassIcon.PGSSplatComponent", new FSlateImageBrush(Icon, FVector2D(16, 16)));
		Icons->Set("ClassIcon.PGSSplat", new FSlateImageBrush(Icon, FVector2D(16, 16)));
		Icons->Set("ClassThumbnail.PGSSplat", new FSlateImageBrush(Icon, FVector2D(64, 64)));
		FSlateStyleRegistry::RegisterSlateStyle(*Icons);
	}
	virtual void ShutdownModule() override
	{
		if (UObjectInitialized()) UThumbnailManager::Get().UnregisterCustomRenderer(UPGSSplat::StaticClass());
		if (Icons) { FSlateStyleRegistry::UnRegisterSlateStyle(*Icons); Icons.Reset(); }
	}
private:
	TSharedPtr<FSlateStyleSet> Icons;
};

IMPLEMENT_MODULE(FPGSEditorModule, PGSEditor)
