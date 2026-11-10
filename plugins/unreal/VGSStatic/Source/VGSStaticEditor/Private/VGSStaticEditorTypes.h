#pragma once

#include "ActorFactories/ActorFactory.h"
#include "AssetDefinitionDefault.h"
#include "CoreMinimal.h"
#include "VGSStaticSplat.h"

#include "VGSStaticEditorTypes.generated.h"

/** The asset in the Content Browser. */
UCLASS()
class UAssetDefinition_VGSStaticSplat : public UAssetDefinitionDefault
{
	GENERATED_BODY()

public:
	virtual FText GetAssetDisplayName() const override { return NSLOCTEXT("VGSStatic", "AssetName", "VGS Static Splat"); }
	virtual FLinearColor GetAssetColor() const override { return FLinearColor(FColor(64, 160, 200)); }
	virtual TSoftClassPtr<UObject> GetAssetClass() const override { return UVGSStaticSplat::StaticClass(); }
	virtual TConstArrayView<FAssetCategoryPath> GetAssetCategories() const override
	{
		static const auto Categories = { EAssetCategoryPaths::Basic };
		return Categories;
	}
	virtual bool CanImport() const override { return true; }
};

/** Dragging the asset into a level places a VGS Static Splat actor showing it. */
UCLASS()
class UVGSStaticSplatActorFactory : public UActorFactory
{
	GENERATED_BODY()

public:
	UVGSStaticSplatActorFactory();

	virtual bool CanCreateActorFrom(const FAssetData& AssetData, FText& OutErrorMsg) override;
	virtual void PostSpawnActor(UObject* Asset, AActor* NewActor) override;
	virtual UObject* GetAssetFromActorInstance(AActor* ActorInstance) override;
};
