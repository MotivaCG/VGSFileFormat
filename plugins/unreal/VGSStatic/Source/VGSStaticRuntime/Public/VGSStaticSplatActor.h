#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "VGSStaticSplatActor.generated.h"

class UBillboardComponent;
class UVGSStaticSplatComponent;

/**
 * A static Gaussian splat character placed in a level: drag a VGS Static Splat asset into the
 * viewport, or Place Actors > VGS Static Splat and pick its asset.
 *
 * The splats are not a primitive, so they cannot be clicked in the viewport; the editor-only
 * sprite at the actor's origin is what selects it.
 */
UCLASS(meta = (DisplayName = "VGS Static Splat"))
class VGSSTATICRUNTIME_API AVGSStaticSplatActor : public AActor
{
	GENERATED_BODY()

public:
	AVGSStaticSplatActor();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "VGS Static")
	TObjectPtr<UVGSStaticSplatComponent> SplatComponent;

#if WITH_EDITORONLY_DATA
	UPROPERTY()
	TObjectPtr<UBillboardComponent> Sprite;
#endif
};
