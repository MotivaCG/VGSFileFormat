#pragma once

#include "ActorFactories/ActorFactory.h"
#include "AssetDefinitionDefault.h"
#include "CoreMinimal.h"
#include "SGSSplat.h"

#include "SGSEditorTypes.generated.h"

/** The asset in the Content Browser. */
UCLASS()
class UAssetDefinition_SGSSplat : public UAssetDefinitionDefault
{
	GENERATED_BODY()

public:
	virtual FText GetAssetDisplayName() const override { return NSLOCTEXT("SGS", "AssetName", "SGS (Static Gaussian Splatting)"); }
	virtual FLinearColor GetAssetColor() const override { return FLinearColor(FColor(64, 160, 200)); }
	virtual TSoftClassPtr<UObject> GetAssetClass() const override { return USGSSplat::StaticClass(); }
	virtual TConstArrayView<FAssetCategoryPath> GetAssetCategories() const override
	{
		static const auto Categories = { EAssetCategoryPaths::Basic };
		return Categories;
	}
	virtual bool CanImport() const override { return true; }
};

/** Dragging the asset into a level places a SGS (Static Gaussian Splatting) actor showing it. */
UCLASS()
class USGSSplatActorFactory : public UActorFactory
{
	GENERATED_BODY()

public:
	USGSSplatActorFactory();

	virtual bool CanCreateActorFrom(const FAssetData& AssetData, FText& OutErrorMsg) override;
	virtual void PostSpawnActor(UObject* Asset, AActor* NewActor) override;
	virtual UObject* GetAssetFromActorInstance(AActor* ActorInstance) override;
};
