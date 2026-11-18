#pragma once

#include "ActorFactories/ActorFactory.h"
#include "AssetDefinitionDefault.h"
#include "CoreMinimal.h"
#include "PGSSplat.h"

#include "PGSEditorTypes.generated.h"

/** The asset in the Content Browser. */
UCLASS()
class UAssetDefinition_PGSSplat : public UAssetDefinitionDefault
{
	GENERATED_BODY()

public:
	virtual FText GetAssetDisplayName() const override { return NSLOCTEXT("PGS", "AssetName", "PGS (Plain Gaussian Splatting)"); }
	virtual FLinearColor GetAssetColor() const override { return FLinearColor(FColor(64, 160, 200)); }
	virtual TSoftClassPtr<UObject> GetAssetClass() const override { return UPGSSplat::StaticClass(); }
	virtual TConstArrayView<FAssetCategoryPath> GetAssetCategories() const override
	{
		static const auto Categories = { EAssetCategoryPaths::Basic };
		return Categories;
	}
	virtual bool CanImport() const override { return true; }
};

/** Dragging the asset into a level places a PGS (Plain Gaussian Splatting) actor showing it. */
UCLASS()
class UPGSSplatActorFactory : public UActorFactory
{
	GENERATED_BODY()

public:
	UPGSSplatActorFactory();

	virtual bool CanCreateActorFrom(const FAssetData& AssetData, FText& OutErrorMsg) override;
	virtual void PostSpawnActor(UObject* Asset, AActor* NewActor) override;
	virtual UObject* GetAssetFromActorInstance(AActor* ActorInstance) override;
};
