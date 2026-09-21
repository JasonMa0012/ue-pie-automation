#include "UE_PIE_AutomationModule.h"
#include "Modules/ModuleManager.h"
#include "Handlers/GameplayHandlers.h"
#include "MCP/UE_PIE_AutomationMCPRegistry.h"
#include "PIE/PIEInputInjector.h"
#include "PIE/PIEInputRecorder.h"
#include "PIE/PIEInputReplayer.h"
#include "PIE/PIEObserver.h"
#include "PIE/PIESessionLog.h"
#include "UI/SMCPPIEPanel.h"
#include "Editor.h"
#include "Misc/CoreDelegates.h"
#include "Containers/Ticker.h"

DEFINE_LOG_CATEGORY(LogUE_PIE_Automation);
IMPLEMENT_MODULE(FUE_PIE_AutomationModule, UE_PIE_Automation)

namespace
{
	TUniquePtr<FUE_PIE_AutomationMCPRegistry> NativeMCPRegistry;
}

void FUE_PIE_AutomationModule::StartupModule()
{
	UE_PIE_Automation::FPIEInputInjector::Init();
	UE_PIE_Automation::FPIEInputRecorder::Get().Init();
	UE_PIE_Automation::FPIEInputReplayer::Get().Init();
	UE_PIE_Automation::FPIEObserver::Get().Init();
	UE_PIE_Automation::FPIESessionLog::Get().Init();
	SMCPPIEPanel::RegisterTab();
	SMCPPIEPanel::RegisterToolbarButton();
	NativeMCPRegistry = MakeUnique<FUE_PIE_AutomationMCPRegistry>();
	NativeMCPRegistry->Register();

	FEditorDelegates::EndPIE.AddLambda([](bool)
	{
		UE_PIE_Automation::FPIEInputInjector::OnPIEEnded();
	});

	// CPU throttle suppression while recording/replaying/observing
	FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([](float) -> bool
		{
			if (!GEditor) return true;
			bool bHasWorld = false;
			for (const FWorldContext& Context : GEngine->GetWorldContexts())
			{
				if (Context.World()) { bHasWorld = true; break; }
			}
			if (!bHasWorld) return true;

			UEditorEngine::FShouldDisableCPUThrottling Suppress;
			Suppress.BindLambda([]() -> bool
			{
				return UE_PIE_Automation::FPIEInputRecorder::Get().IsActive()
				    || UE_PIE_Automation::FPIEInputReplayer::Get().IsActive()
				    || UE_PIE_Automation::FPIEObserver::Get().IsActive();
			});
			GEditor->ShouldDisableCPUThrottlingDelegates.Add(Suppress);
			return false;
		})
	);

	UE_LOG(LogUE_PIE_Automation, Log, TEXT("[UE PIE Automation] Native MCP registration requested"));
}

void FUE_PIE_AutomationModule::ShutdownModule()
{
	SMCPPIEPanel::UnregisterToolbarButton();
	SMCPPIEPanel::UnregisterTab();

	if (NativeMCPRegistry)
	{
		NativeMCPRegistry->Unregister();
		NativeMCPRegistry.Reset();
	}

	UE_PIE_Automation::FPIESessionLog::Get().Shutdown();
	UE_PIE_Automation::FPIEObserver::Get().Shutdown();
	UE_PIE_Automation::FPIEInputReplayer::Get().Shutdown();
	UE_PIE_Automation::FPIEInputRecorder::Get().Shutdown();
	UE_PIE_Automation::FPIEInputInjector::Shutdown();
}
