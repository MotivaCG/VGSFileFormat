#include "Modules/ModuleManager.h"
#include "PGSSplatComponent.h"
#include "PGSSplatShadowVertexFactory.h"
#include "HAL/IConsoleManager.h"
#include "UObject/UObjectIterator.h"

class FPGSRuntimeModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		Sink = IConsoleManager::Get().RegisterConsoleVariableSink_Handle(FConsoleCommandDelegate::CreateRaw(this, &FPGSRuntimeModule::RefreshShadows));
	}
	virtual void ShutdownModule() override
	{
		IConsoleManager::Get().UnregisterConsoleVariableSink_Handle(Sink);
	}
private:
	void RefreshShadows()
	{
		const int32 Method = int32(PGSSplat::GetShadowMethod());
		if (Method == LastMethod) return;
		LastMethod = Method;
		for (TObjectIterator<UPGSSplatComponent> It; It; ++It)
			if (!It->IsTemplate() && It->IsRegistered()) It->PushRenderState(false);
	}
	int32 LastMethod = -1;
	FConsoleVariableSinkHandle Sink;
};

IMPLEMENT_MODULE(FPGSRuntimeModule, PGSRuntime)
