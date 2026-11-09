#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "VGSCaptureActor.generated.h"

class UBillboardComponent;
class UVGSCaptureComponent;

/**
 * A VGS capture placed in a level: Place Actors > VGS Capture, then pick its file.
 *
 * The splats are not a primitive, so they cannot be clicked in the viewport; the editor-only
 * sprite at the actor's origin is what selects it.
 */
UCLASS(meta = (DisplayName = "VGS Capture"))
class VGSRUNTIME_API AVGSCaptureActor : public AActor
{
	GENERATED_BODY()

public:
	AVGSCaptureActor();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "VGS")
	TObjectPtr<UVGSCaptureComponent> Capture;

#if WITH_EDITORONLY_DATA
	UPROPERTY()
	TObjectPtr<UBillboardComponent> Sprite;
#endif
};
