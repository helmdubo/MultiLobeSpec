#pragma once

#include "CoreMinimal.h"
#include "HAL/IConsoleManager.h"

// Process-wide compatibility for the native UE 5.8.2 D3D12 BindlessAll RT path.
// This is independent of the FogMS actor/effect switch: native RT remains affected.
class FFogMSRHICompatibility
{
public:
	void Startup();
	void Shutdown();

private:
	void Apply();
	FDelegateHandle PostEngineInitHandle;
	FConsoleVariableSinkHandle ConsoleSinkHandle;
	bool bReported = false;
	bool bPrimed = false;
};
