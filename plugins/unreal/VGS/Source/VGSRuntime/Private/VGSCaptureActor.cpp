#include "VGSCaptureActor.h"

#include "VGSCaptureComponent.h"

#include "Components/BillboardComponent.h"
#include "Engine/Texture2D.h"
#include "UObject/ConstructorHelpers.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(VGSCaptureActor)

AVGSCaptureActor::AVGSCaptureActor()
{
	Capture = CreateDefaultSubobject<UVGSCaptureComponent>(TEXT("Capture"));
	RootComponent = Capture;

#if WITH_EDITORONLY_DATA
	Sprite = CreateEditorOnlyDefaultSubobject<UBillboardComponent>(TEXT("Sprite"));
	if (Sprite)
	{
		static ConstructorHelpers::FObjectFinderOptional<UTexture2D> Icon(TEXT("/VGS/Icons/S_VGSCapture"));
		Sprite->Sprite = Icon.Get();
		Sprite->SetupAttachment(Capture);
		Sprite->bIsScreenSizeScaled = true;
	}
#endif
}
