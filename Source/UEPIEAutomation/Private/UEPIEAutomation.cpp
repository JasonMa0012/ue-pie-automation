#include "UEPIEAutomationModule.h"
#include "Modules/ModuleManager.h"
#include "Handlers/GameplayHandlers.h"
#include "MCP/UEPIEAutomationMCPRegistry.h"
#include "PIE/PIEInputInjector.h"
#include "PIE/PIEInputRecorder.h"
#include "PIE/PIEInputReplayer.h"
#include "PIE/PIEObserver.h"
#include "PIE/PIESessionLog.h"
#include "UI/SMCPPIEPanel.h"
#include "Editor.h"
#include "Misc/CoreDelegates.h"
#include "Containers/Ticker.h"

DEFINE_LOG_CATEGORY(LogUEPIEAutomation);
IMPLEMENT_MODULE(FUEPIEAutomationModule, UEPIEAutomation)

namespace
{
	TUniquePtr<FUEPIEAutomationMCPRegistry> NativeMCPRegistry;
}

void FUEPIEAutomationModule::StartupModule()
{
	UEPIEAutomation::FPIEInputInjector::Init();
	UEPIEAutomation::FPIEInputRecorder::Get().Init();
	UEPIEAutomation::FPIEInputReplayer::Get().Init();
	UEPIEAutomation::FPIEObserver::Get().Init();
	UEPIEAutomation::FPIESessionLog::Get().Init();
	SMCPPIEPanel::RegisterTab();
	SMCPPIEPanel::RegisterToolbarButton();
	NativeMCPRegistry = MakeUnique<FUEPIEAutomationMCPRegistry>();
	NativeMCPRegistry->Register();

	FEditorDelegates::EndPIE.AddLambda([](bool)
	{
		UEPIEAutomation::FPIEInputInjector::OnPIEEnded();
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
				return UEPIEAutomation::FPIEInputRecorder::Get().IsActive()
				    || UEPIEAutomation::FPIEInputReplayer::Get().IsActive()
				    || UEPIEAutomation::FPIEObserver::Get().IsActive();
			});
			GEditor->ShouldDisableCPUThrottlingDelegates.Add(Suppress);
			return false;
		})
	);

	UE_LOG(LogUEPIEAutomation, Log, TEXT("[UE PIE Automation] Native MCP registration requested"));
}

void FUEPIEAutomationModule::ShutdownModule()
{
	SMCPPIEPanel::UnregisterToolbarButton();
	SMCPPIEPanel::UnregisterTab();

	if (NativeMCPRegistry)
	{
		NativeMCPRegistry->Unregister();
		NativeMCPRegistry.Reset();
	}

	UEPIEAutomation::FPIESessionLog::Get().Shutdown();
	UEPIEAutomation::FPIEObserver::Get().Shutdown();
	UEPIEAutomation::FPIEInputReplayer::Get().Shutdown();
	UEPIEAutomation::FPIEInputRecorder::Get().Shutdown();
	UEPIEAutomation::FPIEInputInjector::Shutdown();
}
