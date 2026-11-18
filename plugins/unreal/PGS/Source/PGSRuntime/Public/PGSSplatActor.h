#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "PGSSplatActor.generated.h"

class UBillboardComponent;
class UPGSSplatComponent;

/**
 * A static Gaussian splat character placed in a level: drag a PGS (Plain Gaussian Splatting) asset into the
 * viewport, or Place Actors > PGS (Plain Gaussian Splatting) and pick its asset.
 *
 * The splats are not a primitive, so they cannot be clicked in the viewport; the editor-only
 * sprite at the actor's origin is what selects it.
 */
UCLASS(meta = (DisplayName = "PGS (Plain Gaussian Splatting)"))
class PGSRUNTIME_API APGSSplatActor : public AActor
{
	GENERATED_BODY()

public:
	APGSSplatActor();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "PGS")
	TObjectPtr<UPGSSplatComponent> SplatComponent;

#if WITH_EDITORONLY_DATA
	UPROPERTY()
	TObjectPtr<UBillboardComponent> Sprite;
#endif
};
