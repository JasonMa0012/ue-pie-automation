#include "UE_PIE_AutomationMCPTool.h"

#include "Async/Async.h"
#include "Serialization/JsonSerializer.h"
#include "ModelContextProtocolToolResults.h"

FUE_PIE_AutomationMCPTool::FUE_PIE_AutomationMCPTool(FString InName, FString InDescription, TSharedPtr<FJsonObject> InSchema, FHandler InHandler)
	: Name(MoveTemp(InName))
	, Description(MoveTemp(InDescription))
	, InputSchema(MoveTemp(InSchema))
	, Handler(InHandler)
{
}

void FUE_PIE_AutomationMCPTool::RunAsync(const FModelContextProtocolToolRequestId& /*RequestId*/, const TSharedPtr<FJsonObject>& Params, const FResultCallback& OnComplete)
{
	const FHandler LocalHandler = Handler;
	AsyncTask(ENamedThreads::GameThread, [LocalHandler, Params, OnComplete]()
	{
		if (!LocalHandler)
		{
			OnComplete(UE::ModelContextProtocol::MakeErrorResult(TEXT("UE PIE Automation tool is not bound")));
			return;
		}

		const TSharedPtr<FJsonValue> Value = LocalHandler(Params);
		if (!Value.IsValid() || Value->Type != EJson::Object)
		{
			OnComplete(UE::ModelContextProtocol::MakeErrorResult(TEXT("UE PIE Automation handler returned a non-object result")));
			return;
		}

		FModelContextProtocolToolResult Result = UE::ModelContextProtocol::MakeStructuredContentResult(Value);
		const TSharedPtr<FJsonObject> Object = Value->AsObject();
		bool bSuccess = true;
		if (Object.IsValid()) Object->TryGetBoolField(TEXT("success"), bSuccess);
		if (!bSuccess) Result.JsonObject->SetBoolField(TEXT("isError"), true);
		OnComplete(Result);
	});
}
