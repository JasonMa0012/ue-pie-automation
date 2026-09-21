#include "JsonSerializer.h"

#include "Dom/JsonObject.h"
#include "UObject/UnrealType.h"

namespace
{
	TSharedPtr<FJsonValue> SerializeProperty(const void* Value, FProperty* Property)
	{
		if (!Value || !Property) return MakeShared<FJsonValueNull>();

		if (const FStrProperty* P = CastField<FStrProperty>(Property)) return MakeShared<FJsonValueString>(P->GetPropertyValue(Value));
		if (const FNameProperty* P = CastField<FNameProperty>(Property)) return MakeShared<FJsonValueString>(P->GetPropertyValue(Value).ToString());
		if (const FTextProperty* P = CastField<FTextProperty>(Property)) return MakeShared<FJsonValueString>(P->GetPropertyValue(Value).ToString());
		if (const FBoolProperty* P = CastField<FBoolProperty>(Property)) return MakeShared<FJsonValueBoolean>(P->GetPropertyValue(Value));
		if (const FNumericProperty* P = CastField<FNumericProperty>(Property))
		{
			if (P->IsInteger()) return MakeShared<FJsonValueNumber>(static_cast<double>(P->GetSignedIntPropertyValue(Value)));
			return MakeShared<FJsonValueNumber>(P->GetFloatingPointPropertyValue(Value));
		}
		if (const FObjectPropertyBase* P = CastField<FObjectPropertyBase>(Property))
		{
			if (UObject* Object = P->GetObjectPropertyValue(Value)) return MakeShared<FJsonValueString>(Object->GetPathName());
			return MakeShared<FJsonValueNull>();
		}
		if (const FStructProperty* P = CastField<FStructProperty>(Property))
		{
			TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
			for (TFieldIterator<FProperty> It(P->Struct); It; ++It)
			{
				FProperty* Field = *It;
				if (Field) Object->SetField(Field->GetName(), SerializeProperty(Field->ContainerPtrToValuePtr<void>(Value), Field));
			}
			return MakeShared<FJsonValueObject>(Object);
		}
		if (const FArrayProperty* P = CastField<FArrayProperty>(Property))
		{
			TArray<TSharedPtr<FJsonValue>> Values;
			FScriptArrayHelper Helper(P, Value);
			for (int32 Index = 0; Index < Helper.Num(); ++Index) Values.Add(SerializeProperty(Helper.GetRawPtr(Index), P->Inner));
			return MakeShared<FJsonValueArray>(Values);
		}

		FString Exported;
		Property->ExportText_Direct(Exported, Value, Value, nullptr, PPF_None);
		return MakeShared<FJsonValueString>(Exported);
	}
}

TSharedPtr<FJsonObject> FMCPJsonSerializer::SerializeObject(UObject* Object)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	if (!Object) return Result;

	Result->SetStringField(TEXT("name"), Object->GetName());
	Result->SetStringField(TEXT("class"), Object->GetClass()->GetName());
	Result->SetStringField(TEXT("path"), Object->GetPathName());
	for (TFieldIterator<FProperty> It(Object->GetClass()); It; ++It)
	{
		FProperty* Property = *It;
		if (Property && Property->HasAnyPropertyFlags(CPF_BlueprintVisible))
		{
			Result->SetField(Property->GetName(), SerializeProperty(Property->ContainerPtrToValuePtr<void>(Object), Property));
		}
	}
	return Result;
}

TSharedPtr<FJsonValue> FMCPJsonSerializer::SerializeValue(const void* Value, FProperty* Property)
{
	return SerializeProperty(Value, Property);
}
