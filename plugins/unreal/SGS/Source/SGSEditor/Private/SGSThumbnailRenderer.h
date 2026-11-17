#pragma once

#include "ThumbnailRendering/DefaultSizedThumbnailRenderer.h"
#include "SGSThumbnailRenderer.generated.h"

class FSGSThumbnailScene;

UCLASS()
class USGSThumbnailRenderer : public UDefaultSizedThumbnailRenderer
{
 GENERATED_BODY()
public:
 virtual bool CanVisualizeAsset(UObject* Object) override;
 virtual void Draw(UObject* Object, int32 X, int32 Y, uint32 Width, uint32 Height,
  FRenderTarget* RenderTarget, FCanvas* Canvas, bool bAdditionalViewFamily) override;
 virtual void BeginDestroy() override;
private:
 FSGSThumbnailScene* Scene = nullptr;
};
