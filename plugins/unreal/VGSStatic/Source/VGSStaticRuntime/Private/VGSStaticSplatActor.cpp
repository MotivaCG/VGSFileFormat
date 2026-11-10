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
		// The engine's own sprite: an asset of the plugin's would be saved by one engine version
		// and refused by older ones.
		static ConstructorHelpers::FObjectFinderOptional<UTexture2D> Icon(TEXT("/Engine/EditorResources/S_Actor"));
		Sprite->Sprite = Icon.Get();
		Sprite->SetupAttachment(SplatComponent);
		Sprite->bIsScreenSizeScaled = true;
	}
#endif
}
