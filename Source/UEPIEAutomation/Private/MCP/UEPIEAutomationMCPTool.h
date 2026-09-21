#pragma once

#include "CoreMinimal.h"
#include "IModelContextProtocolTool.h"

class FGameplayHandlers;

class FUEPIEAutomationMCPTool final : public IModelContextProtocolTool
{
public:
	using FHandler = TSharedPtr<FJsonValue> (*)(const TSharedPtr<FJsonObject>& Params);

	FUEPIEAutomationMCPTool(FString InName, FString InDescription, TSharedPtr<FJsonObject> InSchema, FHandler InHandler);

	virtual FString GetName() const override { return Name; }
	virtual FString GetDescription() const override { return Description; }
	virtual TSharedPtr<FJsonObject> GetInputJsonSchema() const override { return InputSchema; }
	virtual void RunAsync(const FModelContextProtocolToolRequestId& RequestId, const TSharedPtr<FJsonObject>& Params, const FResultCallback& OnComplete) override;

private:
	FString Name;
	FString Description;
	TSharedPtr<FJsonObject> InputSchema;
	FHandler Handler = nullptr;
};
