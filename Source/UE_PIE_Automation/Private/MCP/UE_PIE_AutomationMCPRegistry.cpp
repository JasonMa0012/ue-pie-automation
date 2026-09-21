#include "UE_PIE_AutomationMCPRegistry.h"

#include "UE_PIE_AutomationMCPTool.h"
#include "Handlers/GameplayHandlers.h"
#include "IModelContextProtocolModule.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Interfaces/IPluginManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	using FHandler = FUE_PIE_AutomationMCPTool::FHandler;

	struct FHandlerEntry
	{
		const TCHAR* Name;
		FHandler Handler;
	};

	static const FHandlerEntry Handlers[] =
	{
		{TEXT("inject_input"), &FGameplayHandlers::InjectInput},
		{TEXT("inject_input_start"), &FGameplayHandlers::InjectInputStart},
		{TEXT("inject_input_update"), &FGameplayHandlers::InjectInputUpdate},
		{TEXT("inject_input_stop"), &FGameplayHandlers::InjectInputStop},
		{TEXT("inject_input_tape"), &FGameplayHandlers::InjectInputTape},
		{TEXT("record_arm"), &FGameplayHandlers::PieRecordArm},
		{TEXT("record_disarm"), &FGameplayHandlers::PieRecordDisarm},
		{TEXT("record_stop"), &FGameplayHandlers::PieRecordStop},
		{TEXT("record_status"), &FGameplayHandlers::PieRecordStatus},
		{TEXT("record_list"), &FGameplayHandlers::PieRecordList},
		{TEXT("record_read"), &FGameplayHandlers::PieRecordRead},
		{TEXT("record_delete"), &FGameplayHandlers::PieRecordDelete},
		{TEXT("mark"), &FGameplayHandlers::PieMark},
		{TEXT("replay_arm"), &FGameplayHandlers::PieReplayArm},
		{TEXT("replay_run"), &FGameplayHandlers::PieReplayRun},
		{TEXT("replay_disarm"), &FGameplayHandlers::PieReplayDisarm},
		{TEXT("replay_stop"), &FGameplayHandlers::PieReplayStop},
		{TEXT("replay_status"), &FGameplayHandlers::PieReplayStatus},
		{TEXT("replay_analyze"), &FGameplayHandlers::PieReplayAnalyze},
		{TEXT("replay_state"), &FGameplayHandlers::PieReplayState},
		{TEXT("reference_save"), &FGameplayHandlers::PieReferenceSave},
		{TEXT("frame_diff"), &FGameplayHandlers::PieFrameDiff},
		{TEXT("record_diff"), &FGameplayHandlers::PieRecordDiff},
		{TEXT("snapshot"), &FGameplayHandlers::PieSnapshot},
		{TEXT("profile_create"), &FGameplayHandlers::PieProfileCreate},
		{TEXT("profile_read"), &FGameplayHandlers::PieProfileRead},
		{TEXT("profile_update"), &FGameplayHandlers::PieProfileUpdate},
		{TEXT("profile_delete"), &FGameplayHandlers::PieProfileDelete},
		{TEXT("profile_list"), &FGameplayHandlers::PieProfileList},
		{TEXT("observe_arm"), &FGameplayHandlers::PieObserveArm},
		{TEXT("observe_disarm"), &FGameplayHandlers::PieObserveDisarm},
		{TEXT("observe_stop"), &FGameplayHandlers::PieObserveStop},
		{TEXT("observe_status"), &FGameplayHandlers::PieObserveStatus},
		{TEXT("observe_list"), &FGameplayHandlers::PieObserveList},
		{TEXT("observe_read"), &FGameplayHandlers::PieObserveRead},
		{TEXT("anim_state"), &FGameplayHandlers::GetPieAnimState},
		{TEXT("anim_properties"), &FGameplayHandlers::GetPieAnimProperties},
		{TEXT("subsystem_state"), &FGameplayHandlers::GetPieSubsystemState},
		{TEXT("session_errors"), &FGameplayHandlers::PieSessionErrors},
		{TEXT("session_log"), &FGameplayHandlers::PieSessionLog},
		{TEXT("capture"), &FGameplayHandlers::PieCapture},
		{TEXT("trace_start"), &FGameplayHandlers::PieTraceStart},
		{TEXT("trace_stop"), &FGameplayHandlers::PieTraceStop},
		{TEXT("perf_summary"), &FGameplayHandlers::PiePerfSummary},
		{TEXT("test_scaffold"), &FGameplayHandlers::PieTestScaffold},
		{TEXT("test_run"), &FGameplayHandlers::PieTestRun},
		{TEXT("test_list"), &FGameplayHandlers::PieTestList},
		{TEXT("assert_eval"), &FGameplayHandlers::PieAssertEval},
		{TEXT("actor_spawn"), &FGameplayHandlers::PieActorSpawn},
		{TEXT("actor_destroy"), &FGameplayHandlers::PieActorDestroy},
		{TEXT("actor_set"), &FGameplayHandlers::PieActorSet},
		{TEXT("actor_call"), &FGameplayHandlers::PieActorCall},
		{TEXT("scenario_scaffold"), &FGameplayHandlers::PieScenarioScaffold},
		{TEXT("scenario_validate"), &FGameplayHandlers::PieScenarioValidate},
	};

	FHandler FindHandler(const FString& Name)
	{
		for (const FHandlerEntry& Entry : Handlers)
		{
			if (Name.Equals(Entry.Name, ESearchCase::IgnoreCase)) return Entry.Handler;
		}
		return nullptr;
	}

	FString SchemaPath()
	{
		if (const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("UE_PIE_Automation")))
		{
			return Plugin->GetBaseDir() / TEXT("Resources/UE_PIE_AutomationTools.json");
		}
		return FPaths::ProjectPluginsDir() / TEXT("UE_PIE_Automation/Resources/UE_PIE_AutomationTools.json");
	}
}

bool FUE_PIE_AutomationMCPRegistry::Register()
{
	IModelContextProtocolModule* Module = FModuleManager::LoadModulePtr<IModelContextProtocolModule>(TEXT("ModelContextProtocol"));
	if (!Module)
	{
		UE_LOG(LogTemp, Warning, TEXT("[UE PIE Automation] Native ModelContextProtocol module is unavailable"));
		return false;
	}

	RefreshHandle = Module->OnRefreshTools().AddRaw(this, &FUE_PIE_AutomationMCPRegistry::RegisterTools);
	RegisterTools();
	return Tools.Num() > 0;
}

void FUE_PIE_AutomationMCPRegistry::Unregister()
{
	IModelContextProtocolModule* Module = FModuleManager::GetModulePtr<IModelContextProtocolModule>(TEXT("ModelContextProtocol"));
	if (Module && RefreshHandle.IsValid()) Module->OnRefreshTools().Remove(RefreshHandle);
	RefreshHandle.Reset();
	if (Module)
	{
		for (const TSharedRef<IModelContextProtocolTool>& Tool : Tools) Module->RemoveTool(Tool);
	}
	Tools.Reset();
}

void FUE_PIE_AutomationMCPRegistry::RegisterTools()
{
	IModelContextProtocolModule* Module = FModuleManager::GetModulePtr<IModelContextProtocolModule>(TEXT("ModelContextProtocol"));
	if (!Module) return;

	for (const TSharedRef<IModelContextProtocolTool>& Tool : Tools) Module->RemoveTool(Tool);
	Tools.Reset();

	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *SchemaPath()))
	{
		UE_LOG(LogTemp, Error, TEXT("[UE PIE Automation] Cannot load native MCP schema: %s"), *SchemaPath());
		return;
	}

	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		UE_LOG(LogTemp, Error, TEXT("[UE PIE Automation] Invalid native MCP schema: %s"), *SchemaPath());
		return;
	}

	const TArray<TSharedPtr<FJsonValue>>* ToolValues = nullptr;
	if (!Root->TryGetArrayField(TEXT("tools"), ToolValues) || !ToolValues) return;

	for (const TSharedPtr<FJsonValue>& Value : *ToolValues)
	{
		const TSharedPtr<FJsonObject> Definition = Value.IsValid() ? Value->AsObject() : nullptr;
		if (!Definition.IsValid()) continue;

		FString Name;
		FString Description;
		Definition->TryGetStringField(TEXT("name"), Name);
		Definition->TryGetStringField(TEXT("description"), Description);
		if (!Name.StartsWith(TEXT("PIEStudio."))) continue;

		const FString HandlerName = Name.RightChop(FCString::Strlen(TEXT("PIEStudio.")));
		const FHandler Handler = FindHandler(HandlerName);
		if (!Handler)
		{
			UE_LOG(LogTemp, Warning, TEXT("[UE PIE Automation] No Handler for native MCP tool %s"), *Name);
			continue;
		}

		TSharedPtr<FJsonObject> InputSchema;
		const TSharedPtr<FJsonObject>* Schema = nullptr;
		if (Definition->TryGetObjectField(TEXT("inputSchema"), Schema) && Schema) InputSchema = *Schema;
		if (!InputSchema.IsValid())
		{
			InputSchema = MakeShared<FJsonObject>();
			InputSchema->SetStringField(TEXT("type"), TEXT("object"));
		}

		TSharedRef<FUE_PIE_AutomationMCPTool> Tool = MakeShared<FUE_PIE_AutomationMCPTool>(Name, Description, InputSchema, Handler);
		if (Module->AddTool(Tool)) Tools.Add(Tool);
	}

	UE_LOG(LogTemp, Log, TEXT("[UE PIE Automation] Registered %d native MCP tools"), Tools.Num());
}
