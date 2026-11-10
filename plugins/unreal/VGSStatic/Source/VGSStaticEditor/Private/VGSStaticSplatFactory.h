#pragma once

#include "CoreMinimal.h"
#include "EditorReimportHandler.h"
#include "Factories/Factory.h"

#include "VGSStaticSplatFactory.generated.h"

class UVGSStaticImportOptions;
class UVGSStaticSplat;

/**
 * Imports a Gaussian splat PLY as a VGS Static Splat asset. Before Epic's importer (whose
 * asset has no exposure, grade or lighting controls), whose turn the dialog can still give
 * it, where Epic's GaussianSplat plugin is enabled.
 */
UCLASS(BlueprintType, hidecategories = Object)
class UVGSStaticSplatFactory : public UFactory, public FReimportHandler
{
	GENERATED_BODY()

public:
	UVGSStaticSplatFactory();

	//~ UFactory
	virtual bool FactoryCanImport(const FString& Filename) override;
	virtual UObject* FactoryCreateFile(UClass* InClass, UObject* InParent, FName InName, EObjectFlags Flags, const FString& Filename,
		const TCHAR* Parms, FFeedbackContext* Warn, bool& bOutOperationCanceled) override;
	virtual void CleanUp() override;

	//~ FReimportHandler
	virtual bool CanReimport(UObject* Obj, TArray<FString>& OutFilenames) override;
	virtual void SetReimportPaths(UObject* Obj, const TArray<FString>& NewReimportPaths) override;
	virtual EReimportResult::Type Reimport(UObject* Obj) override;
	virtual int32 GetPriority() const override { return ImportPriority; }

private:
	/** Reads, estimates and builds into Asset; false (with the reason logged to Warn) on failure. */
	static bool Build(UVGSStaticSplat* Asset, const FString& Filename, const UVGSStaticImportOptions& Options, FFeedbackContext* Warn);

	/** One answer of the dialog for every file of a multi-file import. */
	bool bAnswered = false;
	int32 Answer = 0; // 0 import, 1 Epic's importer, 2 cancel
};
