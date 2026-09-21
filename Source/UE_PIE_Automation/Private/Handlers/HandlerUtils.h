#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "UObject/UObjectIterator.h"

template <typename T>
T* LoadAssetByPath(const FString& AssetPath)
{
	return LoadObject<T>(nullptr, *AssetPath);
}

inline TArray<FString> JsonArrayToStringList(const TArray<TSharedPtr<FJsonValue>>* Values)
{
	TArray<FString> Result;
	if (!Values) return Result;
	for (const TSharedPtr<FJsonValue>& Value : *Values)
	{
		FString Text;
		if (Value.IsValid() && Value->TryGetString(Text)) Result.Add(Text);
	}
	return Result;
}

// Small local replacement for the former ue-mcp bridge helpers. Keeping this
// header local makes the plugin self-contained while preserving the handler
// contracts and response fields.

#define MCP_CHECK_GAME_THREAD() checkf(IsInGameThread(), TEXT("UE PIE Automation handler must run on the game thread"))

inline TSharedPtr<FJsonValue> MCPError(const FString& Message)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), false);
	Result->SetStringField(TEXT("error"), Message);
	return MakeShared<FJsonValueObject>(Result);
}

inline TSharedPtr<FJsonValue> MCPResult(const TSharedPtr<FJsonObject>& Result)
{
	return MakeShared<FJsonValueObject>(Result);
}

inline TSharedPtr<FJsonObject> MCPSuccess()
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	return Result;
}

inline void MCPSetCreated(const TSharedPtr<FJsonObject>& Result)
{
	Result->SetBoolField(TEXT("existed"), false);
	Result->SetBoolField(TEXT("created"), true);
}

inline void MCPSetUpdated(const TSharedPtr<FJsonObject>& Result)
{
	Result->SetBoolField(TEXT("updated"), true);
}

inline void MCPSetRollback(const TSharedPtr<FJsonObject>& Result, const FString& Method, const TSharedPtr<FJsonObject>& Payload)
{
	TSharedPtr<FJsonObject> Rollback = MakeShared<FJsonObject>();
	Rollback->SetStringField(TEXT("method"), Method);
	Rollback->SetObjectField(TEXT("payload"), Payload);
	Result->SetObjectField(TEXT("rollback"), Rollback);
}

inline void MCPSetDeleteAssetRollback(const TSharedPtr<FJsonObject>& Result, const FString& AssetPath)
{
	TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("assetPath"), AssetPath);
	MCPSetRollback(Result, TEXT("delete_asset"), Payload);
}

inline FString OptionalString(const TSharedPtr<FJsonObject>& Params, const TCHAR* Key, const FString& DefaultValue = TEXT(""))
{
	FString Value;
	return Params.IsValid() && Params->TryGetStringField(Key, Value) ? Value : DefaultValue;
}

inline double OptionalNumber(const TSharedPtr<FJsonObject>& Params, const TCHAR* Key, double DefaultValue = 0.0)
{
	double Value = DefaultValue;
	return Params.IsValid() && Params->TryGetNumberField(Key, Value) ? Value : DefaultValue;
}

inline int32 OptionalInt(const TSharedPtr<FJsonObject>& Params, const TCHAR* Key, int32 DefaultValue = 0)
{
	int32 Value = DefaultValue;
	return Params.IsValid() && Params->TryGetNumberField(Key, Value) ? Value : DefaultValue;
}

inline bool OptionalBool(const TSharedPtr<FJsonObject>& Params, const TCHAR* Key, bool DefaultValue = false)
{
	bool Value = DefaultValue;
	return Params.IsValid() && Params->TryGetBoolField(Key, Value) ? Value : DefaultValue;
}

inline TSharedPtr<FJsonValue> RequireString(const TSharedPtr<FJsonObject>& Params, const TCHAR* Key, FString& OutValue)
{
	if (Params.IsValid() && Params->TryGetStringField(Key, OutValue) && !OutValue.IsEmpty())
	{
		return nullptr;
	}
	return MCPError(FString::Printf(TEXT("Missing required parameter '%s'"), Key));
}

inline TSharedPtr<FJsonValue> RequireStringAlt(const TSharedPtr<FJsonObject>& Params, const TCHAR* Key1, const TCHAR* Key2, FString& OutValue)
{
	if (Params.IsValid())
	{
		if (Params->TryGetStringField(Key1, OutValue) && !OutValue.IsEmpty()) return nullptr;
		if (Params->TryGetStringField(Key2, OutValue) && !OutValue.IsEmpty()) return nullptr;
	}
	return MCPError(FString::Printf(TEXT("Missing required parameter '%s' (or '%s')"), Key1, Key2));
}

inline UWorld* GetPIEWorld()
{
	if (!GEngine) return nullptr;
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		if ((Context.WorldType == EWorldType::PIE || Context.WorldType == EWorldType::Game) && Context.World())
		{
			return Context.World();
		}
	}
	return nullptr;
}

inline AActor* FindActorByLabelOrName(UWorld* World, const FString& Token)
{
	if (!World) return nullptr;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (Actor && (Actor->GetActorLabel() == Token || Actor->GetName() == Token)) return Actor;
	}
	return nullptr;
}

inline UClass* FindClassByShortName(const FString& ClassName)
{
	if (ClassName.IsEmpty()) return nullptr;
	if (UClass* Exact = FindObject<UClass>(nullptr, *ClassName)) return Exact;
	for (TObjectIterator<UClass> It; It; ++It)
	{
		if (It->GetName() == ClassName || It->GetPathName() == ClassName) return *It;
	}
	return nullptr;
}
