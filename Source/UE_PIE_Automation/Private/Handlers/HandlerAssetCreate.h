#pragma once

#include "CoreMinimal.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "Handlers/HandlerUtils.h"

template <typename TAsset>
struct FMCPAssetCreate
{
	TAsset* Asset = nullptr;
	TSharedPtr<FJsonValue> EarlyReturn;
};

inline TSharedPtr<FJsonValue> MCPCheckAssetExists(const FString& PackagePath, const FString& Name, const FString& OnConflict, const FString& FriendlyType = TEXT("Asset"))
{
	const FString ObjectPath = PackagePath + TEXT("/") + Name + TEXT(".") + Name;
	if (UObject* Existing = LoadObject<UObject>(nullptr, *ObjectPath))
	{
		if (OnConflict.Equals(TEXT("error"), ESearchCase::IgnoreCase))
		{
			return MCPError(FString::Printf(TEXT("%s '%s' already exists"), *FriendlyType, *ObjectPath));
		}
		TSharedPtr<FJsonObject> Result = MCPSuccess();
		Result->SetBoolField(TEXT("existed"), true);
		Result->SetBoolField(TEXT("created"), false);
		Result->SetStringField(TEXT("path"), Existing->GetPathName());
		Result->SetStringField(TEXT("name"), Name);
		Result->SetStringField(TEXT("packagePath"), PackagePath);
		return MCPResult(Result);
	}
	return nullptr;
}

template <typename TAsset>
inline FMCPAssetCreate<TAsset> MCPCreateAssetIdempotentNewObject(const FString& Name, const FString& PackagePath, const FString& OnConflict, const FString& AssetTypeLabel)
{
	FMCPAssetCreate<TAsset> Out;
	if (TSharedPtr<FJsonValue> Existing = MCPCheckAssetExists(PackagePath, Name, OnConflict, AssetTypeLabel))
	{
		Out.EarlyReturn = Existing;
		return Out;
	}

	UPackage* Package = CreatePackage(*(PackagePath + TEXT("/") + Name));
	if (!Package)
	{
		Out.EarlyReturn = MCPError(FString::Printf(TEXT("Failed to create package for %s '%s'"), *AssetTypeLabel, *Name));
		return Out;
	}
	Out.Asset = NewObject<TAsset>(Package, TAsset::StaticClass(), *Name, RF_Public | RF_Standalone);
	if (!Out.Asset)
	{
		Out.EarlyReturn = MCPError(FString::Printf(TEXT("Failed to construct %s '%s'"), *AssetTypeLabel, *Name));
		return Out;
	}
	FAssetRegistryModule::AssetCreated(Out.Asset);
	Out.Asset->MarkPackageDirty();
	Package->SetDirtyFlag(true);
	return Out;
}

inline bool SaveAssetPackage(UObject* Asset)
{
	return Asset && UEditorAssetLibrary::SaveLoadedAsset(Asset);
}
