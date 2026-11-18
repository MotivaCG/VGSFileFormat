#include "PGSThumbnailRenderer.h"
#include "PGSSplat.h"
#include "PGSSplatActor.h"
#include "PGSSplatComponent.h"
#include "Components/BillboardComponent.h"
#include "Engine/World.h"
#include "SceneView.h"
#include "ThumbnailHelpers.h"
#include "RenderingThread.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(PGSThumbnailRenderer)

class FPGSThumbnailScene : public FThumbnailPreviewScene
{
public:
 FPGSThumbnailScene()
  : FThumbnailPreviewScene(FConstructionValues().SetCreateSkySphere(false).SetCreateFloorPlane(false))
 {
  Actor = GetWorld()->SpawnActor<APGSSplatActor>();
  Actor->SplatComponent->Lighting = EPGSLighting::SelfIllumination;
  Actor->SplatComponent->SetCastShadow(false);
  Actor->SplatComponent->bProxyCollision = false;
  if (auto* Sprite = Actor->FindComponentByClass<UBillboardComponent>()) Sprite->SetVisibility(false);
 }
 void SetAsset(UPGSSplat* Asset) { Actor->SplatComponent->SetSplat(Asset); }
protected:
 virtual void GetViewMatrixParameters(float FOV, FVector& Origin, float& Pitch, float& Yaw, float& Zoom) const override
 {
  const FBoxSphereBounds Bounds = Actor->SplatComponent->Bounds;
  Origin = -Bounds.Origin;
  Pitch = 0.0f;
  Yaw = 90.0f;
  Zoom = 1.12f * Bounds.SphereRadius / FMath::Sin(FMath::DegreesToRadians(FOV * 0.5f));
 }
private:
 APGSSplatActor* Actor = nullptr; // owned and referenced by the preview world
};

bool UPGSThumbnailRenderer::CanVisualizeAsset(UObject* Object)
{
 const auto* Asset = Cast<UPGSSplat>(Object);
 return Asset && Asset->GetData().IsValid() && Asset->GetData()->NumSplats > 0;
}

void UPGSThumbnailRenderer::Draw(UObject* Object, int32 X, int32 Y, uint32 Width, uint32 Height,
 FRenderTarget* RenderTarget, FCanvas* Canvas, bool bAdditionalViewFamily)
{
 auto* Asset = Cast<UPGSSplat>(Object);
 if (!Asset || !CanVisualizeAsset(Asset)) return;
 if (!Scene) Scene = new FPGSThumbnailScene();
 Scene->SetAsset(Asset);
 FSceneViewFamilyContext Family(FSceneViewFamily::ConstructionValues(RenderTarget, Scene->GetScene(), FEngineShowFlags(ESFIM_Game))
  .SetTime(GetTime()).SetAdditionalViewFamily(bAdditionalViewFamily));
 Family.EngineShowFlags.DisableAdvancedFeatures();
 Family.EngineShowFlags.MotionBlur = false;
 Family.EngineShowFlags.BillboardSprites = false;
 Family.EngineShowFlags.EyeAdaptation = false;
 RenderViewFamily(Canvas, &Family, Scene->CreateView(&Family, X, Y, Width, Height));
 Scene->SetAsset(nullptr);
}

void UPGSThumbnailRenderer::BeginDestroy()
{
 if (Scene) { FlushRenderingCommands(); delete Scene; Scene = nullptr; }
 Super::BeginDestroy();
}
