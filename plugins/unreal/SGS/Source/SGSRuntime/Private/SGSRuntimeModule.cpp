#include "Modules/ModuleManager.h"
#include "SGSSplatComponent.h"
#include "SGSSplatShadowVertexFactory.h"
#include "HAL/IConsoleManager.h"
#include "UObject/UObjectIterator.h"

class FSGSRuntimeModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		Sink = IConsoleManager::Get().RegisterConsoleVariableSink_Handle(FConsoleCommandDelegate::CreateRaw(this, &FSGSRuntimeModule::RefreshShadows));
	}
	virtual void ShutdownModule() override
	{
		IConsoleManager::Get().UnregisterConsoleVariableSink_Handle(Sink);
	}
private:
	void RefreshShadows()
	{
		const int32 Method = int32(SGSSplat::GetShadowMethod());
		if (Method == LastMethod) return;
		LastMethod = Method;
		for (TObjectIterator<USGSSplatComponent> It; It; ++It)
			if (!It->IsTemplate() && It->IsRegistered()) It->PushRenderState(false);
	}
	int32 LastMethod = -1;
	FConsoleVariableSinkHandle Sink;
};

IMPLEMENT_MODULE(FSGSRuntimeModule, SGSRuntime)
