#include "VGSStaticSplatActor.h"

#include "VGSStaticSplatComponent.h"

#include "Components/BillboardComponent.h"
#include "Engine/Texture2D.h"
#include "UObject/ConstructorHelpers.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(VGSStaticSplatActor)

AVGSStaticSplatActor::AVGSStaticSplatActor()
{
	SplatComponent = CreateDefaultSubobject<UVGSStaticSplatComponent>(TEXT("Splat"));
	RootComponent = SplatComponent;

#if WITH_EDITORONLY_DATA
	Sprite = CreateEditorOnlyDefaultSubobject<UBillboardComponent>(TEXT("Sprite"));
	if (Sprite)
	{
		// The VGS plugin's icon, copied into this plugin when it is assembled. It was saved by
		// Unreal 5.9: an older engine refuses it, and the actor falls back to the engine's sprite.
		static ConstructorHelpers::FObjectFinderOptional<UTexture2D> Icon(TEXT("/VGSStatic/Icons/S_VGSCapture"));
		static ConstructorHelpers::FObjectFinderOptional<UTexture2D> EngineIcon(TEXT("/Engine/EditorResources/S_Actor"));
		Sprite->Sprite = Icon.Get() ? Icon.Get() : EngineIcon.Get();
		Sprite->SetupAttachment(SplatComponent);
		Sprite->bIsScreenSizeScaled = true;
	}
#endif
}
