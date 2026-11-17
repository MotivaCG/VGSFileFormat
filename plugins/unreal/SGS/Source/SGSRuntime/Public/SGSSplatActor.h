#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "SGSSplatActor.generated.h"

class UBillboardComponent;
class USGSSplatComponent;

/**
 * A static Gaussian splat character placed in a level: drag a SGS (Static Gaussian Splatting) asset into the
 * viewport, or Place Actors > SGS (Static Gaussian Splatting) and pick its asset.
 *
 * The splats are not a primitive, so they cannot be clicked in the viewport; the editor-only
 * sprite at the actor's origin is what selects it.
 */
UCLASS(meta = (DisplayName = "SGS (Static Gaussian Splatting)"))
class SGSRUNTIME_API ASGSSplatActor : public AActor
{
	GENERATED_BODY()

public:
	ASGSSplatActor();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SGS")
	TObjectPtr<USGSSplatComponent> SplatComponent;

#if WITH_EDITORONLY_DATA
	UPROPERTY()
	TObjectPtr<UBillboardComponent> Sprite;
#endif
};
