#include "PGSSplatActor.h"

#include "PGSSplatComponent.h"

#include "Components/BillboardComponent.h"
#include "Engine/Texture2D.h"
#include "UObject/ConstructorHelpers.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(PGSSplatActor)

APGSSplatActor::APGSSplatActor()
{
	SplatComponent = CreateDefaultSubobject<UPGSSplatComponent>(TEXT("Splat"));
	RootComponent = SplatComponent;

#if WITH_EDITORONLY_DATA
	Sprite = CreateEditorOnlyDefaultSubobject<UBillboardComponent>(TEXT("Sprite"));
	if (Sprite)
	{
		// The PGS icon, shipped with this plugin. It was saved by
		// Unreal 5.9: an older engine refuses it, and the actor falls back to the engine's sprite.
		static ConstructorHelpers::FObjectFinderOptional<UTexture2D> Icon(TEXT("/PGS/Icons/S_PGSCapture"));
		static ConstructorHelpers::FObjectFinderOptional<UTexture2D> EngineIcon(TEXT("/Engine/EditorResources/S_Actor"));
		Sprite->Sprite = Icon.Get() ? Icon.Get() : EngineIcon.Get();
		Sprite->SetupAttachment(SplatComponent);
		Sprite->bIsScreenSizeScaled = true;
		Sprite->SetRelativeScale3D(FVector(0.125f)); // 512 px icon -> 16 cm billboard in UE
		Sprite->SetRelativeLocation(FVector(0.0, 0.0, 40.0));
		Sprite->bUseInEditorScaling = false;
	}
#endif
}
