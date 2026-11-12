// VGS: not Epic's. See VGSSplatSettings.h.
#include "VGSSplatSettings.h"

#include "HAL/IConsoleManager.h"

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
	return FText::FromString(VGSSPLAT_PLUGIN_NAME);
}

FText UVGSSplatSettings::GetSectionDescription() const
{
	return FText::FromString(TEXT("How the plugin's splats cast shadows, for the whole project."));
}

void UVGSSplatSettings::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	ApplyToConsoleVariables();
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
