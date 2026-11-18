#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/ObjectThumbnail.h"
#include "ObjectTools.h"
#include "PGSSplat.h"
#include "ImageUtils.h"
#include "ImageCore.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPGSThumbnailTest, "PGS.Editor.CharacterThumbnail",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPGSThumbnailTest::RunTest(const FString&)
{
 FString AssetPath;
 if (!FParse::Value(FCommandLine::Get(), TEXT("PGSThumbnailAsset="), AssetPath))
 {
  AddWarning(TEXT("Provide -PGSThumbnailAsset=/Game/... to render an imported PGS fixture."));
  return true;
 }
 UPGSSplat* Asset = LoadObject<UPGSSplat>(nullptr, *AssetPath);
 if (!TestNotNull(TEXT("Imported PGS asset"), Asset)) return false;
 FObjectThumbnail Thumbnail;
 ThumbnailTools::RenderThumbnail(Asset, 256, 256, ThumbnailTools::EThumbnailTextureFlushMode::AlwaysFlush, nullptr, &Thumbnail);
 const TArray<uint8>& Pixels = Thumbnail.GetUncompressedImageData();
 if (!TestEqual(TEXT("Full thumbnail readback"), Pixels.Num(), 256 * 256 * 4)) return false;
 int32 Foreground = 0;
 for (int32 I = 0; I < Pixels.Num(); I += 4)
  if (FMath::Max3(Pixels[I], Pixels[I + 1], Pixels[I + 2]) > 40) ++Foreground;
 TestTrue(TEXT("Capture occupies the thumbnail"), Foreground > 300);
 const FString Output = FPaths::ProjectSavedDir() / TEXT("Automation/PGSThumbnail.png");
 TestTrue(TEXT("Saved preview"), FImageUtils::SaveImageByExtension(*Output,
  FImageView(const_cast<uint8*>(Pixels.GetData()), 256, 256, ERawImageFormat::BGRA8)));
 return true;
}
#endif
