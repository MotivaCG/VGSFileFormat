#include "SGSEditorTypes.h"
#include "SGSThumbnailRenderer.h"
#include "ThumbnailRendering/ThumbnailManager.h"

#include "SGSSplatActor.h"
#include "SGSSplatComponent.h"

#include "Modules/ModuleManager.h"
#include "Brushes/SlateImageBrush.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "Styling/SlateStyle.h"
#include "Styling/SlateStyleRegistry.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(SGSEditorTypes)

#define LOCTEXT_NAMESPACE "SGSEditor"

USGSSplatActorFactory::USGSSplatActorFactory()
{
	DisplayName = LOCTEXT("ActorFactory", "SGS (Static Gaussian Splatting)");
	NewActorClass = ASGSSplatActor::StaticClass();
	bUseSurfaceOrientation = true;
}

bool USGSSplatActorFactory::CanCreateActorFrom(const FAssetData& AssetData, FText& OutErrorMsg)
{
	if (!AssetData.IsValid() || !AssetData.IsInstanceOf(USGSSplat::StaticClass()))
	{
		OutErrorMsg = LOCTEXT("NoAsset", "No SGS (Static Gaussian Splatting) asset selected");
		return false;
	}
	return true;
}

void USGSSplatActorFactory::PostSpawnActor(UObject* Asset, AActor* NewActor)
{
	Super::PostSpawnActor(Asset, NewActor);
	ASGSSplatActor* Actor = CastChecked<ASGSSplatActor>(NewActor);
	if (Actor->SplatComponent)
	{
		Actor->SplatComponent->SetSplat(CastChecked<USGSSplat>(Asset));
	}
}

UObject* USGSSplatActorFactory::GetAssetFromActorInstance(AActor* ActorInstance)
{
	const ASGSSplatActor* Actor = Cast<ASGSSplatActor>(ActorInstance);
	return Actor && Actor->SplatComponent ? Actor->SplatComponent->Splat.Get() : nullptr;
}

#undef LOCTEXT_NAMESPACE

class FSGSEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		UThumbnailManager::Get().RegisterCustomRenderer(USGSSplat::StaticClass(), USGSThumbnailRenderer::StaticClass());
		const auto Plugin = IPluginManager::Get().FindPlugin(TEXT("SGS"));
		if (!Plugin) return;
		Icons = MakeShared<FSlateStyleSet>(TEXT("SGSActorIcons"));
		const FString Icon = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Resources/IconSGS.png"));
		Icons->Set("ClassIcon.SGSSplatActor", new FSlateImageBrush(Icon, FVector2D(16, 16)));
		Icons->Set("ClassThumbnail.SGSSplatActor", new FSlateImageBrush(Icon, FVector2D(64, 64)));
		Icons->Set("ClassIcon.SGSSplatComponent", new FSlateImageBrush(Icon, FVector2D(16, 16)));
		Icons->Set("ClassIcon.SGSSplat", new FSlateImageBrush(Icon, FVector2D(16, 16)));
		Icons->Set("ClassThumbnail.SGSSplat", new FSlateImageBrush(Icon, FVector2D(64, 64)));
		FSlateStyleRegistry::RegisterSlateStyle(*Icons);
	}
	virtual void ShutdownModule() override
	{
		if (UObjectInitialized()) UThumbnailManager::Get().UnregisterCustomRenderer(USGSSplat::StaticClass());
		if (Icons) { FSlateStyleRegistry::UnRegisterSlateStyle(*Icons); Icons.Reset(); }
	}
private:
	TSharedPtr<FSlateStyleSet> Icons;
};

IMPLEMENT_MODULE(FSGSEditorModule, SGSEditor)
