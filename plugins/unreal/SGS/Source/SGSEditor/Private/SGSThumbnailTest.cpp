#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/ObjectThumbnail.h"
#include "ObjectTools.h"
#include "SGSSplat.h"
#include "ImageUtils.h"
#include "ImageCore.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSGSThumbnailTest, "SGS.Editor.CharacterThumbnail",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSGSThumbnailTest::RunTest(const FString&)
{
 FString AssetPath;
 if (!FParse::Value(FCommandLine::Get(), TEXT("SGSThumbnailAsset="), AssetPath))
 {
  AddWarning(TEXT("Provide -SGSThumbnailAsset=/Game/... to render an imported SGS fixture."));
  return true;
 }
 USGSSplat* Asset = LoadObject<USGSSplat>(nullptr, *AssetPath);
 if (!TestNotNull(TEXT("Imported SGS asset"), Asset)) return false;
 FObjectThumbnail Thumbnail;
 ThumbnailTools::RenderThumbnail(Asset, 256, 256, ThumbnailTools::EThumbnailTextureFlushMode::AlwaysFlush, nullptr, &Thumbnail);
 const TArray<uint8>& Pixels = Thumbnail.GetUncompressedImageData();
 if (!TestEqual(TEXT("Full thumbnail readback"), Pixels.Num(), 256 * 256 * 4)) return false;
 int32 Foreground = 0;
 for (int32 I = 0; I < Pixels.Num(); I += 4)
  if (FMath::Max3(Pixels[I], Pixels[I + 1], Pixels[I + 2]) > 40) ++Foreground;
 TestTrue(TEXT("Capture occupies the thumbnail"), Foreground > 300);
 const FString Output = FPaths::ProjectSavedDir() / TEXT("Automation/SGSThumbnail.png");
 TestTrue(TEXT("Saved preview"), FImageUtils::SaveImageByExtension(*Output,
  FImageView(const_cast<uint8*>(Pixels.GetData()), 256, 256, ERawImageFormat::BGRA8)));
 return true;
}
#endif
