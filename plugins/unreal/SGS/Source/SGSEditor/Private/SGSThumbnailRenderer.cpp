#include "SGSThumbnailRenderer.h"
#include "SGSSplat.h"
#include "SGSSplatActor.h"
#include "SGSSplatComponent.h"
#include "Components/BillboardComponent.h"
#include "Engine/World.h"
#include "SceneView.h"
#include "ThumbnailHelpers.h"
#include "RenderingThread.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(SGSThumbnailRenderer)

class FSGSThumbnailScene : public FThumbnailPreviewScene
{
public:
 FSGSThumbnailScene()
  : FThumbnailPreviewScene(FConstructionValues().SetCreateSkySphere(false).SetCreateFloorPlane(false))
 {
  Actor = GetWorld()->SpawnActor<ASGSSplatActor>();
  Actor->SplatComponent->Lighting = ESGSLighting::SelfIllumination;
  Actor->SplatComponent->SetCastShadow(false);
  Actor->SplatComponent->bProxyCollision = false;
  if (auto* Sprite = Actor->FindComponentByClass<UBillboardComponent>()) Sprite->SetVisibility(false);
 }
 void SetAsset(USGSSplat* Asset) { Actor->SplatComponent->SetSplat(Asset); }
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
 ASGSSplatActor* Actor = nullptr; // owned and referenced by the preview world
};

bool USGSThumbnailRenderer::CanVisualizeAsset(UObject* Object)
{
 const auto* Asset = Cast<USGSSplat>(Object);
 return Asset && Asset->GetData().IsValid() && Asset->GetData()->NumSplats > 0;
}

void USGSThumbnailRenderer::Draw(UObject* Object, int32 X, int32 Y, uint32 Width, uint32 Height,
 FRenderTarget* RenderTarget, FCanvas* Canvas, bool bAdditionalViewFamily)
{
 auto* Asset = Cast<USGSSplat>(Object);
 if (!Asset || !CanVisualizeAsset(Asset)) return;
 if (!Scene) Scene = new FSGSThumbnailScene();
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

void USGSThumbnailRenderer::BeginDestroy()
{
 if (Scene) { FlushRenderingCommands(); delete Scene; Scene = nullptr; }
 Super::BeginDestroy();
}
