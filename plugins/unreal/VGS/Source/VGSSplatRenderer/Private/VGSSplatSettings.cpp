// VGS: not Epic's. See VGSSplatSettings.h.
#include "VGSSplatSettings.h"

#include "HAL/IConsoleManager.h"
#if WITH_EDITOR
#include "EditorSupportDelegates.h"
#endif

#include UE_INLINE_GENERATED_CPP_BY_NAME(VGSSplatSettings)

void UVGSSplatSettings::PostInitProperties()
{
	Super::PostInitProperties();
	if (IsTemplate())
	{
		ApplyToConsoleVariables();
	}
}

#if WITH_EDITOR
FText UVGSSplatSettings::GetSectionText() const
{
	return FText::FromString(FCString::Strcmp(VGSSPLAT_PLUGIN_NAME, TEXT("VGS")) == 0 ? TEXT("VGS") : TEXT("SGS"));
}

FText UVGSSplatSettings::GetSectionDescription() const
{
	return FText::FromString(FCString::Strcmp(VGSSPLAT_PLUGIN_NAME, TEXT("VGS")) == 0
		? TEXT("VGS (Video Gaussian Static). Project-wide shadow controls.")
		: TEXT("SGS (Static Gaussian Splatting). Actors follow this Shadow Method by default. No Shadows also disables mesh proxies."));
}

void UVGSSplatSettings::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	ApplyToConsoleVariables();
	IConsoleManager::Get().CallAllConsoleVariableSinks();
	FEditorSupportDelegates::RedrawAllViewports.Broadcast();
}
#endif

void UVGSSplatSettings::ApplyToConsoleVariables() const
{
	IConsoleManager& Console = IConsoleManager::Get();
	auto SetInt = [&Console](const TCHAR* Name, int32 Value)
	{
		if (IConsoleVariable* CVar = Console.FindConsoleVariable(Name))
		{
			CVar->Set(Value, ECVF_SetByProjectSetting);
		}
	};
	auto SetFloat = [&Console](const TCHAR* Name, float Value)
	{
		if (IConsoleVariable* CVar = Console.FindConsoleVariable(Name))
		{
			CVar->Set(Value, ECVF_SetByProjectSetting);
		}
	};
	SetInt(TEXT("r.VGSSplat.Shadow.Method"), int32(ShadowMethod));
	SetInt(TEXT("r.VGSSplat.Shadow.Self"), bSelfShadow ? 1 : 0);
	SetFloat(TEXT("r.VGSSplat.Shadow.TexelSize"), ShadowTexelSize);
	SetFloat(TEXT("r.VGSSplat.Shadow.Softness"), ShadowSoftness);
	SetFloat(TEXT("r.VGSSplat.Shadow.Bias"), ShadowBias);
}
