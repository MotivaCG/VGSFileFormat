#pragma once

#include "ThumbnailRendering/DefaultSizedThumbnailRenderer.h"
#include "PGSThumbnailRenderer.generated.h"

class FPGSThumbnailScene;

UCLASS()
class UPGSThumbnailRenderer : public UDefaultSizedThumbnailRenderer
{
 GENERATED_BODY()
public:
 virtual bool CanVisualizeAsset(UObject* Object) override;
 virtual void Draw(UObject* Object, int32 X, int32 Y, uint32 Width, uint32 Height,
  FRenderTarget* RenderTarget, FCanvas* Canvas, bool bAdditionalViewFamily) override;
 virtual void BeginDestroy() override;
private:
 FPGSThumbnailScene* Scene = nullptr;
};
