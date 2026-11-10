#include "VGSStaticSplatFactory.h"

#include "VGSStaticImportOptions.h"
#include "VGSStaticNormals.h"
#include "VGSStaticPly.h"
#include "VGSStaticSplat.h"

#include "Editor.h"
#include "EditorFramework/AssetImportData.h"
#include "AssetImportTask.h"
#include "Subsystems/ImportSubsystem.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "IDetailsView.h"
#include "Interfaces/IMainFrameModule.h"
#include "Misc/FeedbackContext.h"
#include "Misc/FileHelper.h"
#include "Misc/ScopedSlowTask.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(VGSStaticSplatFactory)

#define LOCTEXT_NAMESPACE "VGSStaticSplatFactory"

DEFINE_LOG_CATEGORY_STATIC(LogVGSStatic, Log, All);

namespace
{
	/** Epic's PLY importer (GaussianSplatEditor), when its plugin is enabled: Unreal 5.9 on. */
	UClass* EpicFactoryClass()
	{
		return FindObject<UClass>(nullptr, TEXT("/Script/GaussianSplatEditor.GaussianSplatFactory"));
	}

	/** The import dialog: the options, and Import / Epic's importer / Cancel. Returns 0, 1 or 2. */
	int32 AskOptions(UVGSStaticImportOptions* Options, const FString& Filename, bool bOfferEpic)
	{
		FPropertyEditorModule& PropertyEditor = FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");
		FDetailsViewArgs Args;
		Args.bAllowSearch = false;
		Args.NameAreaSettings = FDetailsViewArgs::HideNameArea;
		const TSharedRef<IDetailsView> Details = PropertyEditor.CreateDetailView(Args);
		Details->SetObject(Options);

		int32 Result = 2;
		const TSharedRef<SWindow> Window = SNew(SWindow)
			.Title(LOCTEXT("Title", "Import Gaussian Splat PLY"))
			.SizingRule(ESizingRule::UserSized)
			.ClientSize(FVector2D(520, 400))
			.SupportsMinimize(false)
			.SupportsMaximize(false);
		const TWeakPtr<SWindow> WeakWindow = Window;
		auto Close = [&Result, WeakWindow](int32 Answer)
		{
			Result = Answer;
			if (const TSharedPtr<SWindow> W = WeakWindow.Pin())
			{
				W->RequestDestroyWindow();
			}
			return FReply::Handled();
		};

		TSharedRef<SHorizontalBox> Buttons = SNew(SHorizontalBox);
		Buttons->AddSlot().FillWidth(1.0f);
		Buttons->AddSlot().AutoWidth().Padding(4.0f)
		[
			SNew(SButton)
			.Text(LOCTEXT("Import", "Import"))
			.ToolTipText(LOCTEXT("ImportTip", "A VGS Static Splat: exposure, grade and lighting by the scene, with normals."))
			.OnClicked_Lambda([Close]() { return Close(0); })
		];
		if (bOfferEpic)
		{
			Buttons->AddSlot().AutoWidth().Padding(4.0f)
			[
				SNew(SButton)
				.Text(LOCTEXT("Epic", "Import as Epic's Gaussian Splat"))
				.ToolTipText(LOCTEXT("EpicTip", "Epic's own asset and importer (GaussianSplat plugin), without these options."))
				.OnClicked_Lambda([Close]() { return Close(1); })
			];
		}
		Buttons->AddSlot().AutoWidth().Padding(4.0f)
		[
			SNew(SButton)
			.Text(LOCTEXT("Cancel", "Cancel"))
			.OnClicked_Lambda([Close]() { return Close(2); })
		];

		Window->SetContent(
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(8.0f)
			[
				SNew(STextBlock).Text(FText::FromString(FPaths::GetCleanFilename(Filename)))
			]
			+ SVerticalBox::Slot().FillHeight(1.0f).Padding(4.0f)
			[
				Details
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(4.0f)
			[
				Buttons
			]);

		TSharedPtr<SWindow> Parent;
		if (FModuleManager::Get().IsModuleLoaded("MainFrame"))
		{
			Parent = FModuleManager::LoadModuleChecked<IMainFrameModule>("MainFrame").GetParentWindow();
		}
		FSlateApplication::Get().AddModalWindow(Window, Parent, false);
		return Result;
	}

	/** Reads the start of a file: enough for a PLY header. */
	FString ReadHead(const FString& Filename)
	{
		TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*Filename));
		if (!Reader)
		{
			return FString();
		}
		const int64 Size = FMath::Min<int64>(Reader->TotalSize(), 64 * 1024);
		TArray<uint8> Bytes;
		Bytes.SetNumUninitialized(int32(Size));
		Reader->Serialize(Bytes.GetData(), Size);
		return FString(int32(Size), reinterpret_cast<const ANSICHAR*>(Bytes.GetData()));
	}
}

UVGSStaticSplatFactory::UVGSStaticSplatFactory()
{
	SupportedClass = UVGSStaticSplat::StaticClass();
	Formats.Add(TEXT("ply;Gaussian splat PLY (VGS Static)"));
	bCreateNew = false;
	bEditorImport = true;
	bText = false;
	// Before Epic's (DefaultImportPriority + 1), which the dialog can still hand the file to.
	ImportPriority = DefaultImportPriority + 2;
}

bool UVGSStaticSplatFactory::FactoryCanImport(const FString& Filename)
{
	// Only Gaussian splat PLYs: meshes and point clouds are left to other importers.
	const FString Head = ReadHead(Filename);
	const int32 End = Head.Find(TEXT("end_header"));
	return Head.StartsWith(TEXT("ply")) && End > 0 && Head.Left(End).Contains(TEXT(" f_dc_0")) && Head.Left(End).Contains(TEXT(" rot_0"));
}

void UVGSStaticSplatFactory::CleanUp()
{
	Super::CleanUp();
	bAnswered = false;
	Answer = 0;
}

UObject* UVGSStaticSplatFactory::FactoryCreateFile(UClass* InClass, UObject* InParent, FName InName, EObjectFlags Flags, const FString& Filename,
	const TCHAR* Parms, FFeedbackContext* Warn, bool& bOutOperationCanceled)
{
	// A script's own options (AssetImportTask.options), or the dialog's, remembered per user.
	UVGSStaticImportOptions* TaskOptions = AssetImportTask ? Cast<UVGSStaticImportOptions>(AssetImportTask->Options) : nullptr;
	UVGSStaticImportOptions* Options = TaskOptions ? TaskOptions : GetMutableDefault<UVGSStaticImportOptions>();
	UClass* Epic = EpicFactoryClass();
	if (!bAnswered)
	{
		Answer = (IsAutomatedImport() || FApp::IsUnattended()) ? 0 : AskOptions(Options, Filename, Epic != nullptr);
		bAnswered = true;
		if (!TaskOptions)
		{
			Options->SaveConfig();
		}
	}
	if (Answer == 2)
	{
		bOutOperationCanceled = true;
		return nullptr;
	}
	if (Answer == 1 && Epic)
	{
		// Epic's importer, as if it had been the only one: its own options and asset.
		UFactory* EpicFactory = NewObject<UFactory>(GetTransientPackage(), Epic);
		EpicFactory->AddToRoot();
		UObject* Result = nullptr;
		if (EpicFactory->ConfigureProperties())
		{
			Result = EpicFactory->FactoryCreateFile(EpicFactory->ResolveSupportedClass(), InParent, InName, Flags, Filename, Parms, Warn, bOutOperationCanceled);
		}
		else
		{
			bOutOperationCanceled = true;
		}
		EpicFactory->RemoveFromRoot();
		return Result;
	}

	GEditor->GetEditorSubsystem<UImportSubsystem>()->BroadcastAssetPreImport(this, InClass, InParent, InName, TEXT("ply"));
	UVGSStaticSplat* Asset = NewObject<UVGSStaticSplat>(InParent, InClass, InName, Flags);
	if (!Build(Asset, Filename, *Options, Warn))
	{
		Asset->MarkAsGarbage();
		GEditor->GetEditorSubsystem<UImportSubsystem>()->BroadcastAssetPostImport(this, nullptr);
		return nullptr;
	}
	GEditor->GetEditorSubsystem<UImportSubsystem>()->BroadcastAssetPostImport(this, Asset);
	return Asset;
}

bool UVGSStaticSplatFactory::Build(UVGSStaticSplat* Asset, const FString& Filename, const UVGSStaticImportOptions& Options, FFeedbackContext* Warn)
{
	FScopedSlowTask Task(4.0f, FText::Format(LOCTEXT("Importing", "Importing {0}"), FText::FromString(FPaths::GetCleanFilename(Filename))));
	Task.MakeDialog();

	const double Start = FPlatformTime::Seconds();
	Task.EnterProgressFrame(1.0f, LOCTEXT("Reading", "Reading the splats"));
	TArray64<uint8> File;
	if (!FFileHelper::LoadFileToArray(File, *Filename))
	{
		Warn->Logf(ELogVerbosity::Error, TEXT("Cannot read %s"), *Filename);
		return false;
	}
	vgsstatic::Splats Splats;
	const std::string Error = vgsstatic::readPly(File.GetData(), size_t(File.Num()), Splats);
	File.Empty();
	if (!Error.empty())
	{
		Warn->Logf(ELogVerbosity::Error, TEXT("%s: %s"), *Filename, UTF8_TO_TCHAR(Error.c_str()));
		return false;
	}

	const double Read = FPlatformTime::Seconds();
	// Normals: the file's (SMNForge's and the VGS editor's, from the capture's own cameras), or
	// worked out with a rig's cameras (the one the file names, or the one asked for), or as
	// PGSR takes them, from each splat's shortest axis, for a PLY from anywhere else.
	Task.EnterProgressFrame(2.0f, LOCTEXT("Normals", "Working out normals"));
	std::vector<float> Estimated;
	const float* Normals = Splats.normals.empty() ? nullptr : Splats.normals.data();
	EVGSStaticNormals Source = Normals ? EVGSStaticNormals::File : EVGSStaticNormals::None;
	vgsstatic::NormalRig FileRig = vgsstatic::NormalRig::SmnV1;
	const bool bFileRig = vgsstatic::rigFromComment(Splats, FileRig);
	const auto RigName = [](vgsstatic::NormalRig Rig) { return Rig == vgsstatic::NormalRig::SmnV1 ? TEXT("SMNV1") : TEXT("THE4DSCANNER"); };
	FString Note;
	if (Normals)
	{
		const FString From = UTF8_TO_TCHAR(Splats.comment("normals").c_str());
		Note = FString::Printf(TEXT("%llu of %llu splats have one%s%s%s"), uint64(Splats.splatsWithNormals), uint64(Splats.count),
			From.IsEmpty() ? TEXT("") : TEXT(", by "), *From, bFileRig ? *FString::Printf(TEXT(", %s rig"), RigName(FileRig)) : TEXT(""));
	}
	if (Options.bEstimateNormals && (!Normals || Options.bReplaceFileNormals))
	{
		bool bUseRig = false;
		vgsstatic::NormalRig Rig = FileRig;
		FString Why;
		switch (Options.Method)
		{
		case EVGSStaticNormalMethod::Auto: bUseRig = bFileRig; Why = bFileRig ? TEXT("named by the file") : TEXT(""); break;
		case EVGSStaticNormalMethod::SmnV1: bUseRig = true; Rig = vgsstatic::NormalRig::SmnV1; Why = TEXT("as asked"); break;
		case EVGSStaticNormalMethod::The4DScanner: bUseRig = true; Rig = vgsstatic::NormalRig::The4DScanner; Why = TEXT("as asked"); break;
		default: break;
		}
		FString Fallback;
		if (bUseRig)
		{
			const vgsstatic::RigNormalsResult Result = vgsstatic::rigNormals(Splats, Rig, Estimated);
			if (Result.seenShare() >= 0.5)
			{
				Source = EVGSStaticNormals::Rig;
				Note = FString::Printf(TEXT("%s rig (%s), whose cameras see %.1f %% of the opaque splats"), RigName(Rig), *Why, 100.0 * Result.seenShare());
			}
			else
			{
				// Not in that rig's frame: its cameras would decide the sides by guessing.
				bUseRig = false;
				Fallback = FString::Printf(TEXT(" (the %s rig's cameras saw only %.0f %% of the splats: not its frame)"), RigName(Rig), 100.0 * Result.seenShare());
			}
		}
		if (!bUseRig)
		{
			vgsstatic::AxisNormalsSettings Settings;
			Settings.neighbours = FMath::Clamp(Options.Neighbours, 4, 64);
			Settings.radiusShare = FMath::Clamp(Options.RadiusPercent, 0.05f, 10.0f) / 100.0f;
			const vgsstatic::AxisNormalsResult Result = vgsstatic::axisNormals(Splats, Estimated, Settings);
			Source = EVGSStaticNormals::Axes;
			Note = FString::Printf(TEXT("%.0f %% of the splats flat, %.0f %% took their neighbours' direction (%d nearest, within %.2g %% of the size), side facing the camera%s"),
				100.0 * double(Result.flat) / double(FMath::Max<size_t>(Result.splats, 1)), 100.0 * double(Result.replaced) / double(FMath::Max<size_t>(Result.splats, 1)),
				Settings.neighbours, double(Settings.radiusShare * 100.0f), *Fallback);
		}
		Normals = Estimated.data();
	}

	const double NormalsDone = FPlatformTime::Seconds();
	Task.EnterProgressFrame(1.0f, LOCTEXT("Building", "Ordering into clusters"));
	vgsstatic::Built Built;
	vgsstatic::build(Splats, Normals, Built);

	TSharedPtr<FVGSStaticSplatData, ESPMode::ThreadSafe> Data = MakeShared<FVGSStaticSplatData, ESPMode::ThreadSafe>();
	Data->NumSplats = Built.count;
	Data->SHCoefficients = Built.shCoefficients;
	Data->Stride = Built.stride;
	Data->Records.Append(Built.records.data(), int32(Built.records.size()));
	Data->Normals.Append(Built.normals.data(), int32(Built.normals.size()));
	Data->Clusters.Append(reinterpret_cast<const uint8*>(Built.clusters.data()), int32(Built.clusters.size() * sizeof(vgsstatic::Cluster)));
	Data->Bounds = FBox3f(FVector3f(Built.boundsMin[0], Built.boundsMin[1], Built.boundsMin[2]), FVector3f(Built.boundsMax[0], Built.boundsMax[1], Built.boundsMax[2]));

	Asset->Modify();
	Asset->NumSplats = int32(Built.count);
	Asset->SHDegree = FMath::Max(vgsunreal::shDegree(Built.shCoefficients), 0);
	Asset->NumClusters = int32(Built.clusters.size());
	Asset->Bounds = Data->Bounds;
	Asset->DroppedSplats = int32(Splats.dropped);
	Asset->GpuLayout = Options.GpuLayout;
	Asset->Normals = Built.normals.empty() ? EVGSStaticNormals::None : Source;
	Asset->NormalsNote = Built.normals.empty() ? FString() : Note;
	Asset->bEstimateNormals = Options.bEstimateNormals;
	Asset->bReplaceFileNormals = Options.bReplaceFileNormals;
	Asset->NormalMethod = Options.Method;
	Asset->NormalNeighbours = Options.Neighbours;
	Asset->NormalRadiusPercent = Options.RadiusPercent;
	Asset->AssetImportData->Update(Filename);
	Asset->SetData(Data);
	Asset->MarkPackageDirty();

	const double End = FPlatformTime::Seconds();
	UE_LOG(LogVGSStatic, Display, TEXT("%s: %d splats (%d dropped), SH degree %d, %d clusters; normals %s%s%s. %.2f s: reading %.2f, normals %.2f, the rest %.2f"),
		*Filename, Asset->NumSplats, Asset->DroppedSplats, Asset->SHDegree, Asset->NumClusters,
		*StaticEnum<EVGSStaticNormals>()->GetDisplayNameTextByValue(int64(Asset->Normals)).ToString(),
		Asset->NormalsNote.IsEmpty() ? TEXT("") : TEXT(", "), *Asset->NormalsNote, End - Start, Read - Start, NormalsDone - Read, End - NormalsDone);
	return true;
}

bool UVGSStaticSplatFactory::CanReimport(UObject* Obj, TArray<FString>& OutFilenames)
{
	const UVGSStaticSplat* Asset = Cast<UVGSStaticSplat>(Obj);
	if (Asset && Asset->AssetImportData)
	{
		Asset->AssetImportData->ExtractFilenames(OutFilenames);
		return true;
	}
	return false;
}

void UVGSStaticSplatFactory::SetReimportPaths(UObject* Obj, const TArray<FString>& NewReimportPaths)
{
	UVGSStaticSplat* Asset = Cast<UVGSStaticSplat>(Obj);
	if (Asset && Asset->AssetImportData && NewReimportPaths.Num() == 1)
	{
		Asset->AssetImportData->UpdateFilenameOnly(NewReimportPaths[0]);
	}
}

EReimportResult::Type UVGSStaticSplatFactory::Reimport(UObject* Obj)
{
	UVGSStaticSplat* Asset = Cast<UVGSStaticSplat>(Obj);
	if (!Asset || !Asset->AssetImportData)
	{
		return EReimportResult::Failed;
	}
	const FString Filename = Asset->AssetImportData->GetFirstFilename();
	if (Filename.IsEmpty() || !FPaths::FileExists(Filename))
	{
		return EReimportResult::Failed;
	}
	// As it was imported, with what the asset's Import Settings say now.
	UVGSStaticImportOptions* Options = NewObject<UVGSStaticImportOptions>();
	Options->bEstimateNormals = Asset->bEstimateNormals;
	Options->bReplaceFileNormals = Asset->bReplaceFileNormals;
	Options->Method = Asset->NormalMethod;
	Options->Neighbours = Asset->NormalNeighbours;
	Options->RadiusPercent = Asset->NormalRadiusPercent;
	Options->GpuLayout = Asset->GpuLayout;
	return Build(Asset, Filename, *Options, GWarn) ? EReimportResult::Succeeded : EReimportResult::Failed;
}

#undef LOCTEXT_NAMESPACE
