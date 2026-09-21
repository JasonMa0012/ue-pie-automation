#pragma once

#include "CoreMinimal.h"
#include "IModelContextProtocolTool.h"

class FUEPIEAutomationMCPRegistry
{
public:
	bool Register();
	void Unregister();

private:
	void RegisterTools();

	TArray<TSharedRef<IModelContextProtocolTool>> Tools;
	FDelegateHandle RefreshHandle;
};
