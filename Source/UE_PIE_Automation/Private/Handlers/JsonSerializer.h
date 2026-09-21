#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonValue.h"

class FProperty;

class FMCPJsonSerializer
{
public:
	static TSharedPtr<FJsonObject> SerializeObject(UObject* Object);

private:
	static TSharedPtr<FJsonValue> SerializeValue(const void* Value, FProperty* Property);
};
