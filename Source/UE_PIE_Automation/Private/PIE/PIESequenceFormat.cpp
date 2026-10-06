#include "PIESequenceFormat.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/DateTime.h"
#include "Misc/Guid.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

namespace UE_PIE_Automation
{
	namespace
	{
		FString WriteJsonToString(const TSharedRef<FJsonObject>& Obj)
		{
			FString Out;
			TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
			FJsonSerializer::Serialize(Obj, Writer);
			return Out;
		}

		// Single-line emitter for the .jsonl path. The default writer is
		// pretty-printed, which breaks the "one object per line" invariant
		// JSONL consumers expect.
		FString WriteJsonToStringCondensed(const TSharedRef<FJsonObject>& Obj)
		{
			FString Out;
			TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
				TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
			FJsonSerializer::Serialize(Obj, Writer);
			return Out;
		}

		bool ReadStringFromFile(const FString& Path, FString& Out, FString& OutError)
		{
			if (!FFileHelper::LoadFileToString(Out, *Path))
			{
				OutError = FString::Printf(TEXT("Failed to read '%s'"), *Path);
				return false;
			}
			return true;
		}

		bool ParseJsonObject(const FString& Json, TSharedPtr<FJsonObject>& Out, FString& OutError)
		{
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
			if (!FJsonSerializer::Deserialize(Reader, Out) || !Out.IsValid())
			{
				OutError = TEXT("JSON parse failed");
				return false;
			}
			return true;
		}

		FString CSVEscape(const FString& In)
		{
			if (In.Contains(TEXT(",")) || In.Contains(TEXT("\"")) || In.Contains(TEXT("\n")))
			{
				FString S = In.Replace(TEXT("\""), TEXT("\"\""));
				return FString::Printf(TEXT("\"%s\""), *S);
			}
			return In;
		}
	}

	TArray<FString> SplitCSVLine(const FString& Line)
	{
		TArray<FString> Out;
		FString Cur;
		bool bInQuotes = false;
		for (int32 i = 0; i < Line.Len(); ++i)
		{
			const TCHAR C = Line[i];
			if (bInQuotes)
			{
				if (C == TEXT('"'))
				{
					if (i + 1 < Line.Len() && Line[i + 1] == TEXT('"')) { Cur.AppendChar('"'); ++i; }
					else { bInQuotes = false; }
				}
				else { Cur.AppendChar(C); }
			}
			else if (C == TEXT('"')) { bInQuotes = true; }
			else if (C == TEXT(',')) { Out.Add(Cur); Cur.Reset(); }
			else { Cur.AppendChar(C); }
		}
		Out.Add(Cur);
		return Out;
	}

	FString ActionValueTypeToString(EActionValueType Type)
	{
		switch (Type)
		{
		case EActionValueType::Boolean: return TEXT("Boolean");
		case EActionValueType::Axis1D:  return TEXT("Axis1D");
		case EActionValueType::Axis2D:  return TEXT("Axis2D");
		case EActionValueType::Axis3D:  return TEXT("Axis3D");
		}
		return TEXT("Boolean");
	}

	bool ParseActionValueType(const FString& Str, EActionValueType& Out)
	{
		if (Str.Equals(TEXT("Boolean"), ESearchCase::IgnoreCase) || Str.Equals(TEXT("Digital"), ESearchCase::IgnoreCase)) { Out = EActionValueType::Boolean; return true; }
		if (Str.Equals(TEXT("Axis1D"),  ESearchCase::IgnoreCase) || Str.Equals(TEXT("Float"),   ESearchCase::IgnoreCase)) { Out = EActionValueType::Axis1D;  return true; }
		if (Str.Equals(TEXT("Axis2D"),  ESearchCase::IgnoreCase) || Str.Equals(TEXT("Vector2D"),ESearchCase::IgnoreCase)) { Out = EActionValueType::Axis2D;  return true; }
		if (Str.Equals(TEXT("Axis3D"),  ESearchCase::IgnoreCase) || Str.Equals(TEXT("Vector"),  ESearchCase::IgnoreCase)) { Out = EActionValueType::Axis3D;  return true; }
		return false;
	}

	FString StepTypeToString(EStepType Type)
	{
		switch (Type)
		{
		case EStepType::Capture:   return TEXT("capture");
		case EStepType::Console:   return TEXT("console");
		case EStepType::Mark:      return TEXT("mark");
		}
		return TEXT("mark");
	}

	bool ParseStepType(const FString& Str, EStepType& Out)
	{
		if (Str == TEXT("capture"))    { Out = EStepType::Capture;   return true; }
		if (Str == TEXT("console"))    { Out = EStepType::Console;   return true; }
		if (Str == TEXT("mark"))       { Out = EStepType::Mark;      return true; }
		return false;
	}

	FString InputEventTypeToString(EPIEInputEventType Type)
	{
		switch (Type)
		{
		case EPIEInputEventType::KeyDown: return TEXT("key_down");
		case EPIEInputEventType::KeyUp: return TEXT("key_up");
		case EPIEInputEventType::MouseMove: return TEXT("mouse_move");
		case EPIEInputEventType::MouseButtonDown: return TEXT("mouse_button_down");
		case EPIEInputEventType::MouseButtonUp: return TEXT("mouse_button_up");
		case EPIEInputEventType::MouseDoubleClick: return TEXT("mouse_double_click");
		case EPIEInputEventType::MouseWheel: return TEXT("mouse_wheel");
		}
		return FString();
	}

	bool ParseInputEventType(const FString& Str, EPIEInputEventType& Out)
	{
		if (Str == TEXT("key_down")) { Out = EPIEInputEventType::KeyDown; return true; }
		if (Str == TEXT("key_up")) { Out = EPIEInputEventType::KeyUp; return true; }
		if (Str == TEXT("mouse_move")) { Out = EPIEInputEventType::MouseMove; return true; }
		if (Str == TEXT("mouse_button_down")) { Out = EPIEInputEventType::MouseButtonDown; return true; }
		if (Str == TEXT("mouse_button_up")) { Out = EPIEInputEventType::MouseButtonUp; return true; }
		if (Str == TEXT("mouse_double_click")) { Out = EPIEInputEventType::MouseDoubleClick; return true; }
		if (Str == TEXT("mouse_wheel")) { Out = EPIEInputEventType::MouseWheel; return true; }
		return false;
	}

	TSharedRef<FJsonObject> InputEventToJson(const FPIEInputEvent& Event)
	{
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("type"), InputEventTypeToString(Event.Type));
		O->SetNumberField(TEXT("time_seconds"), Event.TimeSeconds);
		O->SetNumberField(TEXT("order"), Event.Order);
		O->SetNumberField(TEXT("user_index"), Event.UserIndex);
		O->SetNumberField(TEXT("pointer_index"), Event.PointerIndex);
		O->SetBoolField(TEXT("shift"), Event.bShift);
		O->SetBoolField(TEXT("control"), Event.bControl);
		O->SetBoolField(TEXT("alt"), Event.bAlt);
		O->SetBoolField(TEXT("command"), Event.bCommand);
		O->SetBoolField(TEXT("caps_locked"), Event.bCapsLocked);

		if (Event.Type == EPIEInputEventType::KeyDown || Event.Type == EPIEInputEventType::KeyUp)
		{
			O->SetStringField(TEXT("key"), Event.Key);
			O->SetNumberField(TEXT("key_code"), Event.KeyCode);
			O->SetBoolField(TEXT("repeat"), Event.bIsRepeat);
		}
		else
		{
			TArray<TSharedPtr<FJsonValue>> Pos;
			Pos.Add(MakeShared<FJsonValueNumber>(Event.Position.X));
			Pos.Add(MakeShared<FJsonValueNumber>(Event.Position.Y));
			O->SetArrayField(TEXT("position"), Pos);
			TArray<TSharedPtr<FJsonValue>> Delta;
			Delta.Add(MakeShared<FJsonValueNumber>(Event.Delta.X));
			Delta.Add(MakeShared<FJsonValueNumber>(Event.Delta.Y));
			O->SetArrayField(TEXT("delta"), Delta);
			O->SetStringField(TEXT("key"), Event.Key);
			TArray<TSharedPtr<FJsonValue>> Buttons;
			for (const FString& Button : Event.PressedButtons)
			{
				Buttons.Add(MakeShared<FJsonValueString>(Button));
			}
			O->SetArrayField(TEXT("pressed_buttons"), Buttons);
			O->SetNumberField(TEXT("wheel_delta"), Event.WheelDelta);
		}
		return O;
	}

	bool ReadInputEvent(const TSharedPtr<FJsonValue>& Value, FPIEInputEvent& Out, FString& OutError)
	{
		const TSharedPtr<FJsonObject> O = Value.IsValid() ? Value->AsObject() : nullptr;
		FString Type;
		if (!O.IsValid() || !O->TryGetStringField(TEXT("type"), Type) || !ParseInputEventType(Type, Out.Type))
		{
			OutError = TEXT("input event has an invalid type");
			return false;
		}
		if (!O->TryGetNumberField(TEXT("time_seconds"), Out.TimeSeconds) || !FMath::IsFinite(Out.TimeSeconds) || Out.TimeSeconds < 0.0
			|| !O->TryGetNumberField(TEXT("order"), Out.Order) || Out.Order < 0)
		{
			OutError = TEXT("input event is missing a valid time or order");
			return false;
		}
		double UserIndexValue = 0.0, PointerIndexValue = 0.0;
		if (!O->TryGetNumberField(TEXT("user_index"), UserIndexValue) || !FMath::IsFinite(UserIndexValue) || UserIndexValue < 0.0 || UserIndexValue > MAX_uint32
			|| !O->TryGetNumberField(TEXT("pointer_index"), PointerIndexValue) || !FMath::IsFinite(PointerIndexValue) || PointerIndexValue < 0.0 || PointerIndexValue > MAX_uint32
			|| !O->TryGetBoolField(TEXT("shift"), Out.bShift)
			|| !O->TryGetBoolField(TEXT("control"), Out.bControl)
			|| !O->TryGetBoolField(TEXT("alt"), Out.bAlt)
			|| !O->TryGetBoolField(TEXT("command"), Out.bCommand)
			|| !O->TryGetBoolField(TEXT("caps_locked"), Out.bCapsLocked))
		{
			OutError = TEXT("input event is missing valid user, pointer, or modifier data");
			return false;
		}
		Out.UserIndex = static_cast<uint32>(UserIndexValue);
		Out.PointerIndex = static_cast<uint32>(PointerIndexValue);
		double Number = 0.0;

		if (Out.Type == EPIEInputEventType::KeyDown || Out.Type == EPIEInputEventType::KeyUp)
		{
			if (!O->TryGetStringField(TEXT("key"), Out.Key) || Out.Key.IsEmpty()
				|| !O->TryGetNumberField(TEXT("key_code"), Number) || !FMath::IsFinite(Number)
				|| Number < 0.0 || Number > MAX_uint32
				|| !O->TryGetBoolField(TEXT("repeat"), Out.bIsRepeat))
			{
				OutError = TEXT("keyboard input event is missing key data");
				return false;
			}
			Out.KeyCode = static_cast<uint32>(FMath::Max(0.0, Number));
			return true;
		}

		const TArray<TSharedPtr<FJsonValue>>* Pos = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Delta = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Buttons = nullptr;
		if (!O->TryGetArrayField(TEXT("position"), Pos) || !Pos || Pos->Num() != 2
			|| !O->TryGetArrayField(TEXT("delta"), Delta) || !Delta || Delta->Num() != 2
			|| !O->TryGetArrayField(TEXT("pressed_buttons"), Buttons) || !Buttons)
		{
			OutError = TEXT("pointer input event is missing position, delta, or pressed buttons");
			return false;
		}
		double PositionX = 0.0, PositionY = 0.0, DeltaX = 0.0, DeltaY = 0.0;
		if (!(*Pos)[0].IsValid() || !(*Pos)[0]->TryGetNumber(PositionX)
			|| !(*Pos)[1].IsValid() || !(*Pos)[1]->TryGetNumber(PositionY)
			|| !(*Delta)[0].IsValid() || !(*Delta)[0]->TryGetNumber(DeltaX)
			|| !(*Delta)[1].IsValid() || !(*Delta)[1]->TryGetNumber(DeltaY)
			|| !FMath::IsFinite(PositionX) || !FMath::IsFinite(PositionY)
			|| !FMath::IsFinite(DeltaX) || !FMath::IsFinite(DeltaY))
		{
			OutError = TEXT("pointer input event contains invalid coordinates");
			return false;
		}
		Out.Position = FVector2D(PositionX, PositionY);
		Out.Delta = FVector2D(DeltaX, DeltaY);
		for (const TSharedPtr<FJsonValue>& Button : *Buttons)
		{
			FString Name;
			if (!Button.IsValid() || !Button->TryGetString(Name) || Name.IsEmpty())
			{
				OutError = TEXT("pointer input event contains an invalid button name");
				return false;
			}
			Out.PressedButtons.Add(Name);
		}
		if (O->HasField(TEXT("key")) && !O->TryGetStringField(TEXT("key"), Out.Key))
		{
			OutError = TEXT("pointer input event contains an invalid key name");
			return false;
		}
		if ((Out.Type == EPIEInputEventType::MouseButtonDown || Out.Type == EPIEInputEventType::MouseButtonUp
			|| Out.Type == EPIEInputEventType::MouseDoubleClick) && Out.Key.IsEmpty())
		{
			OutError = TEXT("mouse button event is missing its button name");
			return false;
		}
		double WheelDelta = 0.0;
		if (O->HasField(TEXT("wheel_delta")))
		{
			if (!O->TryGetNumberField(TEXT("wheel_delta"), WheelDelta) || !FMath::IsFinite(WheelDelta))
			{
				OutError = TEXT("pointer input event has an invalid wheel delta");
				return false;
			}
			Out.WheelDelta = static_cast<float>(WheelDelta);
		}
		else if (Out.Type == EPIEInputEventType::MouseWheel)
		{
			OutError = TEXT("mouse wheel event is missing its delta");
			return false;
		}
		return true;
	}

	// ── Manifest ─────────────────────────────────────────────────────────

	TSharedRef<FJsonObject> ManifestToJson(const FManifest& M)
	{
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetNumberField(TEXT("version"), M.Version);
		O->SetStringField(TEXT("id"), M.Id);
		O->SetStringField(TEXT("started_at"), M.StartedAt);
		O->SetStringField(TEXT("ended_at"), M.EndedAt);
		O->SetNumberField(TEXT("duration_seconds"), M.DurationSeconds);
		O->SetNumberField(TEXT("total_frames"), M.TotalFrames);
		O->SetNumberField(TEXT("sample_hz"), M.SampleHz);
		O->SetNumberField(TEXT("pin_max_fps"), M.PinMaxFPS);
		O->SetNumberField(TEXT("rng_seed"), static_cast<double>(M.RngSeed));
		O->SetStringField(TEXT("pie_world"), M.PIEWorld);
		O->SetStringField(TEXT("pawn_class"), M.PawnClass);
		O->SetNumberField(TEXT("client_id"), M.ClientId);
		O->SetNumberField(TEXT("axis_threshold"), M.AxisThreshold);

		TArray<TSharedPtr<FJsonValue>> ActionsArr;
		for (const FActionSpec& A : M.Actions)
		{
			TSharedRef<FJsonObject> AO = MakeShared<FJsonObject>();
			AO->SetStringField(TEXT("name"), A.Name);
			AO->SetStringField(TEXT("path"), A.Path);
			AO->SetStringField(TEXT("value_type"), ActionValueTypeToString(A.ValueType));
			ActionsArr.Add(MakeShared<FJsonValueObject>(AO));
		}
		O->SetArrayField(TEXT("actions"), ActionsArr);

		TArray<TSharedPtr<FJsonValue>> TrackedArr;
		for (const FTrackedValueSpec& T : M.TrackedValues)
		{
			TSharedRef<FJsonObject> TO = MakeShared<FJsonObject>();
			TO->SetStringField(TEXT("path"), T.Path);
			TO->SetStringField(TEXT("type"), T.Type);
			TrackedArr.Add(MakeShared<FJsonValueObject>(TO));
		}
		O->SetArrayField(TEXT("tracked_values"), TrackedArr);

		TArray<TSharedPtr<FJsonValue>> MarkersArr;
		for (const FMarker& Mk : M.Markers)
		{
			TSharedRef<FJsonObject> MO = MakeShared<FJsonObject>();
			MO->SetNumberField(TEXT("frame"), static_cast<double>(Mk.Frame));
			MO->SetNumberField(TEXT("time"), Mk.Time);
			MO->SetStringField(TEXT("label"), Mk.Label);
			if (Mk.Metadata.IsValid())
			{
				MO->SetObjectField(TEXT("metadata"), Mk.Metadata);
			}
			MarkersArr.Add(MakeShared<FJsonValueObject>(MO));
		}
		O->SetArrayField(TEXT("markers"), MarkersArr);

		TSharedRef<FJsonObject> Files = MakeShared<FJsonObject>();
		Files->SetStringField(TEXT("csv"), M.CSVFile);
		Files->SetStringField(TEXT("sequence"), M.SequenceFile);
		if (!M.DriftFile.IsEmpty())
		{
			Files->SetStringField(TEXT("drift"), M.DriftFile);
		}
		if (!M.TrackedActorsFile.IsEmpty())
		{
			Files->SetStringField(TEXT("tracked_actors"), M.TrackedActorsFile);
		}
		O->SetObjectField(TEXT("files"), Files);

		TArray<TSharedPtr<FJsonValue>> ActorIdsArr;
		for (const FString& Id : M.TrackedActorIds)
		{
			ActorIdsArr.Add(MakeShared<FJsonValueString>(Id));
		}
		O->SetArrayField(TEXT("tracked_actors"), ActorIdsArr);

		return O;
	}

	bool ManifestFromJson(const TSharedRef<FJsonObject>& Obj, FManifest& Out, FString& OutError)
	{
		Out = FManifest();
		int32 V = 0;
		if (!Obj->TryGetNumberField(TEXT("version"), V))
		{
			OutError = TEXT("manifest: missing 'version'; create a new recording");
			return false;
		}
		Out.Version = V;
		if (Out.Version != kFormatVersion)
		{
			OutError = FString::Printf(TEXT("manifest: unsupported version %d (expected %d); create a new recording"), Out.Version, kFormatVersion);
			return false;
		}
		if (!Obj->TryGetStringField(TEXT("id"), Out.Id))
		{
			OutError = TEXT("manifest: missing 'id'");
			return false;
		}
		Obj->TryGetStringField(TEXT("started_at"), Out.StartedAt);
		Obj->TryGetStringField(TEXT("ended_at"), Out.EndedAt);
		Obj->TryGetNumberField(TEXT("duration_seconds"), Out.DurationSeconds);
		int32 TotalFrames = 0;
		Obj->TryGetNumberField(TEXT("total_frames"), TotalFrames);
		Out.TotalFrames = TotalFrames;
		int32 SampleHz = 60;
		Obj->TryGetNumberField(TEXT("sample_hz"), SampleHz);
		Out.SampleHz = SampleHz;
		int32 PinMaxFPS = 60;
		Obj->TryGetNumberField(TEXT("pin_max_fps"), PinMaxFPS);
		Out.PinMaxFPS = PinMaxFPS;
		double Seed = 0.0;
		Obj->TryGetNumberField(TEXT("rng_seed"), Seed);
		Out.RngSeed = static_cast<int64>(Seed);
		Obj->TryGetStringField(TEXT("pie_world"), Out.PIEWorld);
		Obj->TryGetStringField(TEXT("pawn_class"), Out.PawnClass);
		int32 ClientId = 0;
		Obj->TryGetNumberField(TEXT("client_id"), ClientId);
		Out.ClientId = ClientId;
		double Threshold = 0.15;
		Obj->TryGetNumberField(TEXT("axis_threshold"), Threshold);
		Out.AxisThreshold = static_cast<float>(Threshold);

		const TArray<TSharedPtr<FJsonValue>>* Actions = nullptr;
		if (Obj->TryGetArrayField(TEXT("actions"), Actions) && Actions)
		{
			for (const TSharedPtr<FJsonValue>& V2 : *Actions)
			{
				const TSharedPtr<FJsonObject>& AO = V2->AsObject();
				if (!AO.IsValid()) continue;
				FActionSpec A;
				AO->TryGetStringField(TEXT("name"), A.Name);
				AO->TryGetStringField(TEXT("path"), A.Path);
				FString VT;
				AO->TryGetStringField(TEXT("value_type"), VT);
				ParseActionValueType(VT, A.ValueType);
				Out.Actions.Add(A);
			}
		}

		const TArray<TSharedPtr<FJsonValue>>* Tracked = nullptr;
		if (Obj->TryGetArrayField(TEXT("tracked_values"), Tracked) && Tracked)
		{
			for (const TSharedPtr<FJsonValue>& V2 : *Tracked)
			{
				const TSharedPtr<FJsonObject>& TO = V2->AsObject();
				if (!TO.IsValid()) continue;
				FTrackedValueSpec T;
				TO->TryGetStringField(TEXT("path"), T.Path);
				TO->TryGetStringField(TEXT("type"), T.Type);
				Out.TrackedValues.Add(T);
			}
		}

		const TArray<TSharedPtr<FJsonValue>>* Markers = nullptr;
		if (Obj->TryGetArrayField(TEXT("markers"), Markers) && Markers)
		{
			for (const TSharedPtr<FJsonValue>& V2 : *Markers)
			{
				const TSharedPtr<FJsonObject>& MO = V2->AsObject();
				if (!MO.IsValid()) continue;
				FMarker Mk;
				double F = 0; MO->TryGetNumberField(TEXT("frame"), F); Mk.Frame = static_cast<uint64>(F);
				MO->TryGetNumberField(TEXT("time"), Mk.Time);
				MO->TryGetStringField(TEXT("label"), Mk.Label);
				const TSharedPtr<FJsonObject>* MetaObj = nullptr;
				if (MO->TryGetObjectField(TEXT("metadata"), MetaObj) && MetaObj)
				{
					Mk.Metadata = *MetaObj;
				}
				Out.Markers.Add(Mk);
			}
		}

		const TSharedPtr<FJsonObject>* Files = nullptr;
		if (Obj->TryGetObjectField(TEXT("files"), Files) && Files)
		{
			(*Files)->TryGetStringField(TEXT("csv"), Out.CSVFile);
			(*Files)->TryGetStringField(TEXT("sequence"), Out.SequenceFile);
			(*Files)->TryGetStringField(TEXT("drift"), Out.DriftFile);
			(*Files)->TryGetStringField(TEXT("tracked_actors"), Out.TrackedActorsFile);
		}

		const TArray<TSharedPtr<FJsonValue>>* ActorIds = nullptr;
		if (Obj->TryGetArrayField(TEXT("tracked_actors"), ActorIds) && ActorIds)
		{
			for (const TSharedPtr<FJsonValue>& V2 : *ActorIds)
			{
				FString S;
				if (V2.IsValid() && V2->TryGetString(S)) Out.TrackedActorIds.Add(S);
			}
		}

		return true;
	}

	// ── Sequence ─────────────────────────────────────────────────────────

	TSharedRef<FJsonObject> SequenceToJson(const FSequence& S)
	{
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetNumberField(TEXT("version"), S.Version);
		if (!S.SourceRecordingId.IsEmpty())
		{
			O->SetStringField(TEXT("source_recording_id"), S.SourceRecordingId);
		}
		O->SetNumberField(TEXT("settle_ms"), S.SettleMs);
		O->SetNumberField(TEXT("sample_hz"), S.SampleHz);
		O->SetNumberField(TEXT("rng_seed"), static_cast<double>(S.RngSeed));
		O->SetStringField(TEXT("input_source"), TEXT("slate"));
		TArray<TSharedPtr<FJsonValue>> ViewportSize;
		ViewportSize.Add(MakeShared<FJsonValueNumber>(S.ViewportSize.X));
		ViewportSize.Add(MakeShared<FJsonValueNumber>(S.ViewportSize.Y));
		O->SetArrayField(TEXT("viewport_size"), ViewportSize);
		TArray<TSharedPtr<FJsonValue>> InputEvents;
		InputEvents.Reserve(S.InputEvents.Num());
		for (const FPIEInputEvent& Event : S.InputEvents)
		{
			InputEvents.Add(MakeShared<FJsonValueObject>(InputEventToJson(Event)));
		}
		O->SetArrayField(TEXT("input_events"), InputEvents);

		TArray<TSharedPtr<FJsonValue>> StepsArr;
		for (const FStep& Step : S.Steps)
		{
			TSharedRef<FJsonObject> SO = MakeShared<FJsonObject>();
			SO->SetStringField(TEXT("type"), StepTypeToString(Step.Type));
			SO->SetNumberField(TEXT("delay_ms"), Step.DelayMs);
			switch (Step.Type)
			{
			case EStepType::Capture:
				SO->SetStringField(TEXT("name"), Step.CaptureName);
				break;
			case EStepType::Console:
				SO->SetStringField(TEXT("command"), Step.Command);
				break;
			case EStepType::Mark:
				SO->SetStringField(TEXT("label"), Step.Label);
				break;
			}
			StepsArr.Add(MakeShared<FJsonValueObject>(SO));
		}
		O->SetArrayField(TEXT("steps"), StepsArr);
		return O;
	}

	bool SequenceFromJson(const TSharedRef<FJsonObject>& Obj, FSequence& Out, FString& OutError)
	{
		Out = FSequence();
		int32 V = 0;
		if (!Obj->TryGetNumberField(TEXT("version"), V))
		{
			OutError = TEXT("sequence: missing 'version'; create a new recording");
			return false;
		}
		Out.Version = V;
		if (Out.Version != kFormatVersion)
		{
			OutError = FString::Printf(TEXT("sequence: unsupported version %d (expected %d); create a new recording"), Out.Version, kFormatVersion);
			return false;
		}
		Obj->TryGetStringField(TEXT("source_recording_id"), Out.SourceRecordingId);
		int32 Settle = 500;
		Obj->TryGetNumberField(TEXT("settle_ms"), Settle);
		Out.SettleMs = Settle;
		int32 Hz = 60;
		Obj->TryGetNumberField(TEXT("sample_hz"), Hz);
		Out.SampleHz = Hz;
		double Seed = 0.0;
		Obj->TryGetNumberField(TEXT("rng_seed"), Seed);
		Out.RngSeed = static_cast<int64>(Seed);
		FString InputSource;
		const TArray<TSharedPtr<FJsonValue>>* ViewportSize = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* InputEvents = nullptr;
		if (!Obj->TryGetStringField(TEXT("input_source"), InputSource) || InputSource != TEXT("slate")
			|| !Obj->TryGetArrayField(TEXT("viewport_size"), ViewportSize) || !ViewportSize || ViewportSize->Num() != 2
			|| !Obj->TryGetArrayField(TEXT("input_events"), InputEvents) || !InputEvents)
		{
			OutError = TEXT("sequence: missing raw Slate input data; create a new recording");
			return false;
		}
		double ViewportWidth = 0.0, ViewportHeight = 0.0;
		if (!(*ViewportSize)[0].IsValid() || !(*ViewportSize)[0]->TryGetNumber(ViewportWidth)
			|| !(*ViewportSize)[1].IsValid() || !(*ViewportSize)[1]->TryGetNumber(ViewportHeight)
			|| !FMath::IsFinite(ViewportWidth) || !FMath::IsFinite(ViewportHeight)
			|| ViewportWidth <= 0.0 || ViewportHeight <= 0.0)
		{
			OutError = TEXT("sequence: invalid viewport_size; create a new recording");
			return false;
		}
		Out.ViewportSize = FVector2D(ViewportWidth, ViewportHeight);
		double PreviousTime = -1.0;
		int32 PreviousOrder = -1;
		for (const TSharedPtr<FJsonValue>& Value : *InputEvents)
		{
			FPIEInputEvent Event;
			if (!ReadInputEvent(Value, Event, OutError)) return false;
			if (Event.TimeSeconds < PreviousTime || (Event.TimeSeconds == PreviousTime && Event.Order <= PreviousOrder))
			{
				OutError = TEXT("sequence: input_events are not in stable time/order sequence");
				return false;
			}
			if (Event.TimeSeconds != PreviousTime) PreviousOrder = -1;
			PreviousTime = Event.TimeSeconds;
			PreviousOrder = Event.Order;
			Out.InputEvents.Add(MoveTemp(Event));
		}

		const TArray<TSharedPtr<FJsonValue>>* Steps = nullptr;
		if (!Obj->TryGetArrayField(TEXT("steps"), Steps) || !Steps)
		{
			OutError = TEXT("sequence: missing 'steps'");
			return false;
		}
		for (const TSharedPtr<FJsonValue>& V2 : *Steps)
		{
			const TSharedPtr<FJsonObject>& SO = V2->AsObject();
			if (!SO.IsValid())
			{
				OutError = TEXT("sequence: invalid step entry");
				return false;
			}
			FString TypeStr;
			if (!SO->TryGetStringField(TEXT("type"), TypeStr))
			{
				OutError = TEXT("sequence: step is missing 'type'");
				return false;
			}
			FStep Step;
			if (!ParseStepType(TypeStr, Step.Type))
			{
				OutError = FString::Printf(TEXT("sequence: unsupported step type '%s'; create a new recording"), *TypeStr);
				return false;
			}
			int32 Delay = 0;
			SO->TryGetNumberField(TEXT("delay_ms"), Delay);
			Step.DelayMs = Delay;
			SO->TryGetStringField(TEXT("name"), Step.CaptureName);
			SO->TryGetStringField(TEXT("command"), Step.Command);
			SO->TryGetStringField(TEXT("label"), Step.Label);

			Out.Steps.Add(Step);
		}
		return true;
	}

	// ── Drift report ─────────────────────────────────────────────────────

	TSharedRef<FJsonObject> DriftToJson(const FDriftReport& D)
	{
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetNumberField(TEXT("version"), D.Version);
		O->SetStringField(TEXT("source_recording_id"), D.SourceRecordingId);
		O->SetStringField(TEXT("replay_started_at"), D.ReplayStartedAt);
		O->SetNumberField(TEXT("frames_compared"), D.FramesCompared);
		O->SetNumberField(TEXT("frames_missing_in_replay"), D.FramesMissingInReplay);
		O->SetNumberField(TEXT("max_position_drift_cm"), D.MaxPositionDriftCm);
		O->SetNumberField(TEXT("max_position_drift_frame"), static_cast<double>(D.MaxPositionDriftFrame));
		O->SetNumberField(TEXT("max_velocity_drift_cms"), D.MaxVelocityDriftCms);
		O->SetNumberField(TEXT("max_rotation_drift_deg"), D.MaxRotationDriftDeg);
		O->SetNumberField(TEXT("montage_section_mismatches"), D.MontageSectionMismatches);

		TSharedRef<FJsonObject> TVD = MakeShared<FJsonObject>();
		for (const TPair<FString, float>& KV : D.TrackedValueMaxDeltas)
		{
			TVD->SetNumberField(KV.Key, KV.Value);
		}
		O->SetObjectField(TEXT("tracked_value_max_deltas"), TVD);

		TSharedRef<FJsonObject> AD = MakeShared<FJsonObject>();
		for (const TPair<FString, FActorDrift>& KV : D.ActorDrift)
		{
			TSharedRef<FJsonObject> A = MakeShared<FJsonObject>();
			A->SetNumberField(TEXT("max_position_cm"), KV.Value.MaxPositionCm);
			A->SetNumberField(TEXT("max_rotation_deg"), KV.Value.MaxRotationDeg);
			A->SetNumberField(TEXT("max_velocity_cms"), KV.Value.MaxVelocityCms);
			A->SetNumberField(TEXT("frames_unresolved_in_source"), KV.Value.FramesUnresolvedInSource);
			A->SetNumberField(TEXT("frames_unresolved_in_replay"), KV.Value.FramesUnresolvedInReplay);
			AD->SetObjectField(KV.Key, A);
		}
		O->SetObjectField(TEXT("actor_drift"), AD);

		TArray<TSharedPtr<FJsonValue>> Frames;
		for (const FDriftFrameEntry& F : D.FramesOverThreshold)
		{
			TSharedRef<FJsonObject> FO = MakeShared<FJsonObject>();
			FO->SetNumberField(TEXT("frame"), static_cast<double>(F.Frame));
			FO->SetNumberField(TEXT("position_delta_cm"), F.PositionDeltaCm);
			FO->SetNumberField(TEXT("velocity_delta_cms"), F.VelocityDeltaCms);
			FO->SetNumberField(TEXT("rotation_delta_deg"), F.RotationDeltaDeg);
			Frames.Add(MakeShared<FJsonValueObject>(FO));
		}
		O->SetArrayField(TEXT("frames_over_threshold"), Frames);

		// Synthesised lead (item 1c): first divergence + ranked channels + errors.
		if (D.Summary.bValid)
		{
			TSharedRef<FJsonObject> S = MakeShared<FJsonObject>();
			const FDriftDivergence& Fd = D.Summary.First;
			if (Fd.bFound)
			{
				TSharedRef<FJsonObject> FD = MakeShared<FJsonObject>();
				FD->SetNumberField(TEXT("frame"), static_cast<double>(Fd.Frame));
				FD->SetNumberField(TEXT("time"), Fd.Time);
				FD->SetStringField(TEXT("channel"), Fd.Channel);
				FD->SetNumberField(TEXT("delta"), Fd.Delta);
				FD->SetNumberField(TEXT("threshold"), Fd.Threshold);
				FD->SetNumberField(TEXT("source_value"), Fd.SourceValue);
				FD->SetNumberField(TEXT("replay_value"), Fd.ReplayValue);
				S->SetObjectField(TEXT("first_divergence"), FD);
			}
			else
			{
				S->SetBoolField(TEXT("diverged"), false);
			}

			TArray<TSharedPtr<FJsonValue>> Top;
			for (const TPair<FString, float>& KV : D.Summary.TopChannels)
			{
				TSharedRef<FJsonObject> T = MakeShared<FJsonObject>();
				T->SetStringField(TEXT("channel"), KV.Key);
				T->SetNumberField(TEXT("max_delta"), KV.Value);
				Top.Add(MakeShared<FJsonValueObject>(T));
			}
			S->SetArrayField(TEXT("top_channels"), Top);

			S->SetNumberField(TEXT("errors_during_run"), D.Summary.ErrorsDuringRun);
			S->SetNumberField(TEXT("warnings_during_run"), D.Summary.WarningsDuringRun);
			if (!D.Summary.TopError.IsEmpty())
			{
				S->SetStringField(TEXT("top_error"), D.Summary.TopError);
				S->SetNumberField(TEXT("top_error_frame"), static_cast<double>(D.Summary.TopErrorFrame));
			}
			if (!D.Summary.SessionDir.IsEmpty())
			{
				S->SetStringField(TEXT("session_dir"), D.Summary.SessionDir);
			}
			O->SetObjectField(TEXT("summary"), S);
		}

		return O;
	}

	bool DriftFromJson(const TSharedRef<FJsonObject>& Obj, FDriftReport& Out, FString& OutError)
	{
		Out = FDriftReport();
		int32 V = 0;
		if (!Obj->TryGetNumberField(TEXT("version"), V))
		{
			OutError = TEXT("drift: missing 'version'");
			return false;
		}
		Out.Version = V;
		Obj->TryGetStringField(TEXT("source_recording_id"), Out.SourceRecordingId);
		Obj->TryGetStringField(TEXT("replay_started_at"), Out.ReplayStartedAt);
		int32 I = 0;
		Obj->TryGetNumberField(TEXT("frames_compared"), I); Out.FramesCompared = I;
		I = 0; Obj->TryGetNumberField(TEXT("frames_missing_in_replay"), I); Out.FramesMissingInReplay = I;
		double F = 0;
		Obj->TryGetNumberField(TEXT("max_position_drift_cm"), F); Out.MaxPositionDriftCm = static_cast<float>(F);
		F = 0; Obj->TryGetNumberField(TEXT("max_position_drift_frame"), F); Out.MaxPositionDriftFrame = static_cast<uint64>(F);
		F = 0; Obj->TryGetNumberField(TEXT("max_velocity_drift_cms"), F); Out.MaxVelocityDriftCms = static_cast<float>(F);
		F = 0; Obj->TryGetNumberField(TEXT("max_rotation_drift_deg"), F); Out.MaxRotationDriftDeg = static_cast<float>(F);
		I = 0; Obj->TryGetNumberField(TEXT("montage_section_mismatches"), I); Out.MontageSectionMismatches = I;

		const TSharedPtr<FJsonObject>* TVD = nullptr;
		if (Obj->TryGetObjectField(TEXT("tracked_value_max_deltas"), TVD) && TVD)
		{
			for (const auto& KV : (*TVD)->Values)
			{
				double D2 = 0;
				if (KV.Value.IsValid() && KV.Value->TryGetNumber(D2))
				{
					Out.TrackedValueMaxDeltas.Add(FString(*KV.Key), static_cast<float>(D2));
				}
			}
		}

		const TSharedPtr<FJsonObject>* AD = nullptr;
		if (Obj->TryGetObjectField(TEXT("actor_drift"), AD) && AD)
		{
			for (const auto& KV : (*AD)->Values)
			{
				const TSharedPtr<FJsonObject>* AO = nullptr;
				if (!KV.Value.IsValid() || !KV.Value->TryGetObject(AO) || !AO) continue;
				FActorDrift AcDr;
				double T = 0;
				(*AO)->TryGetNumberField(TEXT("max_position_cm"), T); AcDr.MaxPositionCm = static_cast<float>(T);
				T = 0; (*AO)->TryGetNumberField(TEXT("max_rotation_deg"), T); AcDr.MaxRotationDeg = static_cast<float>(T);
				T = 0; (*AO)->TryGetNumberField(TEXT("max_velocity_cms"), T); AcDr.MaxVelocityCms = static_cast<float>(T);
				int32 Unresolved = 0;
				(*AO)->TryGetNumberField(TEXT("frames_unresolved_in_source"), Unresolved); AcDr.FramesUnresolvedInSource = Unresolved;
				Unresolved = 0; (*AO)->TryGetNumberField(TEXT("frames_unresolved_in_replay"), Unresolved); AcDr.FramesUnresolvedInReplay = Unresolved;
				Out.ActorDrift.Add(FString(*KV.Key), AcDr);
			}
		}

		const TArray<TSharedPtr<FJsonValue>>* Frames = nullptr;
		if (Obj->TryGetArrayField(TEXT("frames_over_threshold"), Frames) && Frames)
		{
			for (const TSharedPtr<FJsonValue>& VV : *Frames)
			{
				const TSharedPtr<FJsonObject>& FO = VV->AsObject();
				if (!FO.IsValid()) continue;
				FDriftFrameEntry E;
				double DD = 0;
				FO->TryGetNumberField(TEXT("frame"), DD); E.Frame = static_cast<uint64>(DD);
				DD = 0; FO->TryGetNumberField(TEXT("position_delta_cm"), DD); E.PositionDeltaCm = static_cast<float>(DD);
				DD = 0; FO->TryGetNumberField(TEXT("velocity_delta_cms"), DD); E.VelocityDeltaCms = static_cast<float>(DD);
				DD = 0; FO->TryGetNumberField(TEXT("rotation_delta_deg"), DD); E.RotationDeltaDeg = static_cast<float>(DD);
				Out.FramesOverThreshold.Add(E);
			}
		}

		const TSharedPtr<FJsonObject>* SumObj = nullptr;
		if (Obj->TryGetObjectField(TEXT("summary"), SumObj) && SumObj)
		{
			Out.Summary.bValid = true;
			const TSharedPtr<FJsonObject>* FD = nullptr;
			if ((*SumObj)->TryGetObjectField(TEXT("first_divergence"), FD) && FD)
			{
				FDriftDivergence& Fd = Out.Summary.First;
				Fd.bFound = true;
				double DD = 0;
				(*FD)->TryGetNumberField(TEXT("frame"), DD); Fd.Frame = static_cast<uint64>(DD);
				(*FD)->TryGetNumberField(TEXT("time"), Fd.Time);
				(*FD)->TryGetStringField(TEXT("channel"), Fd.Channel);
				(*FD)->TryGetNumberField(TEXT("delta"), Fd.Delta);
				(*FD)->TryGetNumberField(TEXT("threshold"), Fd.Threshold);
				(*FD)->TryGetNumberField(TEXT("source_value"), Fd.SourceValue);
				(*FD)->TryGetNumberField(TEXT("replay_value"), Fd.ReplayValue);
			}
			const TArray<TSharedPtr<FJsonValue>>* Top = nullptr;
			if ((*SumObj)->TryGetArrayField(TEXT("top_channels"), Top) && Top)
			{
				for (const TSharedPtr<FJsonValue>& VV : *Top)
				{
					const TSharedPtr<FJsonObject>& TO = VV->AsObject();
					if (!TO.IsValid()) continue;
					FString Ch; double MD = 0;
					TO->TryGetStringField(TEXT("channel"), Ch);
					TO->TryGetNumberField(TEXT("max_delta"), MD);
					Out.Summary.TopChannels.Add(TPair<FString, float>(Ch, static_cast<float>(MD)));
				}
			}
			int32 EC = 0; (*SumObj)->TryGetNumberField(TEXT("errors_during_run"), EC); Out.Summary.ErrorsDuringRun = EC;
			int32 WC = 0; (*SumObj)->TryGetNumberField(TEXT("warnings_during_run"), WC); Out.Summary.WarningsDuringRun = WC;
			(*SumObj)->TryGetStringField(TEXT("top_error"), Out.Summary.TopError);
			double TEF = 0; (*SumObj)->TryGetNumberField(TEXT("top_error_frame"), TEF); Out.Summary.TopErrorFrame = static_cast<uint64>(TEF);
			(*SumObj)->TryGetStringField(TEXT("session_dir"), Out.Summary.SessionDir);
		}
		return true;
	}

	// ── File IO ──────────────────────────────────────────────────────────

	bool SaveManifest(const FString& Path, const FManifest& M, FString& OutError)
	{
		const FString S = WriteJsonToString(ManifestToJson(M));
		if (!FFileHelper::SaveStringToFile(S, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			OutError = FString::Printf(TEXT("write failed: %s"), *Path);
			return false;
		}
		return true;
	}

	bool LoadManifest(const FString& Path, FManifest& Out, FString& OutError)
	{
		FString Raw;
		if (!ReadStringFromFile(Path, Raw, OutError)) return false;
		TSharedPtr<FJsonObject> Obj;
		if (!ParseJsonObject(Raw, Obj, OutError)) return false;
		return ManifestFromJson(Obj.ToSharedRef(), Out, OutError);
	}

	bool SaveSequence(const FString& Path, const FSequence& S, FString& OutError)
	{
		const FString Json = WriteJsonToString(SequenceToJson(S));
		if (!FFileHelper::SaveStringToFile(Json, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			OutError = FString::Printf(TEXT("write failed: %s"), *Path);
			return false;
		}
		return true;
	}

	bool LoadSequence(const FString& Path, FSequence& Out, FString& OutError)
	{
		FString Raw;
		if (!ReadStringFromFile(Path, Raw, OutError)) return false;
		TSharedPtr<FJsonObject> Obj;
		if (!ParseJsonObject(Raw, Obj, OutError)) return false;
		return SequenceFromJson(Obj.ToSharedRef(), Out, OutError);
	}

	bool SaveDrift(const FString& Path, const FDriftReport& D, FString& OutError)
	{
		const FString Json = WriteJsonToString(DriftToJson(D));
		if (!FFileHelper::SaveStringToFile(Json, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			OutError = FString::Printf(TEXT("write failed: %s"), *Path);
			return false;
		}
		return true;
	}

	bool LoadDrift(const FString& Path, FDriftReport& Out, FString& OutError)
	{
		FString Raw;
		if (!ReadStringFromFile(Path, Raw, OutError)) return false;
		TSharedPtr<FJsonObject> Obj;
		if (!ParseJsonObject(Raw, Obj, OutError)) return false;
		return DriftFromJson(Obj.ToSharedRef(), Out, OutError);
	}

	// ── CSV ──────────────────────────────────────────────────────────────

	FString BuildCSVHeader(const FCSVHeader& H)
	{
		FString Out;
		Out += FString::Printf(TEXT("# ue-mcp recording v%d | id=%s | sample_hz=%d | seed=%lld\n"),
			kFormatVersion, *H.RecordingId, H.SampleHz, static_cast<long long>(H.RngSeed));
		Out += TEXT("frame,time,dt,pos_x,pos_y,pos_z,rot_yaw,rot_pitch,rot_roll,vel_x,vel_y,vel_z,speed2d,montage,mouse_x,mouse_y,mouse_valid");
		for (const FActionSpec& A : H.Actions)
		{
			switch (A.ValueType)
			{
			case EActionValueType::Boolean:
			case EActionValueType::Axis1D:
				Out += FString::Printf(TEXT(",%s"), *A.Name);
				break;
			case EActionValueType::Axis2D:
				Out += FString::Printf(TEXT(",%s_x,%s_y"), *A.Name, *A.Name);
				break;
			case EActionValueType::Axis3D:
				Out += FString::Printf(TEXT(",%s_x,%s_y,%s_z"), *A.Name, *A.Name, *A.Name);
				break;
			}
		}
		for (const FTrackedValueSpec& T : H.TrackedValues)
		{
			Out += FString::Printf(TEXT(",t:%s"), *T.Path);
		}
		// Perf columns (item 4a) sit before the free-form event column so a naive
		// comma split still lands them correctly even if an event field is quoted.
		Out += TEXT(",game_ms,render_ms,gpu_ms,mem_mb");
		Out += TEXT(",event\n");
		return Out;
	}

	void AppendCSVRow(FString& Body, const FCSVRow& Row, const FCSVHeader& H)
	{
		Body += FString::Printf(TEXT("%llu,%.4f,%.4f,%.1f,%.1f,%.1f,%.2f,%.2f,%.2f,%.1f,%.1f,%.1f,%.1f,%s"),
			static_cast<unsigned long long>(Row.Frame),
			Row.Time, Row.Dt,
			Row.PawnLocation.X, Row.PawnLocation.Y, Row.PawnLocation.Z,
			Row.PawnRotation.Yaw, Row.PawnRotation.Pitch, Row.PawnRotation.Roll,
			Row.PawnVelocity.X, Row.PawnVelocity.Y, Row.PawnVelocity.Z,
			Row.Speed2D,
			Row.MontageSection.IsEmpty() ? TEXT("") : *CSVEscape(Row.MontageSection));
		Body += FString::Printf(TEXT(",%.1f,%.1f,%d"),
			Row.MousePosition.X,
			Row.MousePosition.Y,
			Row.bMousePositionValid ? 1 : 0);

		for (const FActionSpec& A : H.Actions)
		{
			const FVector* V = Row.ActionValues.Find(A.Name);
			FVector Val = V ? *V : FVector::ZeroVector;
			switch (A.ValueType)
			{
			case EActionValueType::Boolean:
				Body += FString::Printf(TEXT(",%.0f"), Val.X);
				break;
			case EActionValueType::Axis1D:
				Body += FString::Printf(TEXT(",%.3f"), Val.X);
				break;
			case EActionValueType::Axis2D:
				Body += FString::Printf(TEXT(",%.3f,%.3f"), Val.X, Val.Y);
				break;
			case EActionValueType::Axis3D:
				Body += FString::Printf(TEXT(",%.3f,%.3f,%.3f"), Val.X, Val.Y, Val.Z);
				break;
			}
		}

		for (const FTrackedValueSpec& T : H.TrackedValues)
		{
			const double* DV = Row.TrackedValues.Find(T.Path);
			Body += FString::Printf(TEXT(",%.6f"), DV ? *DV : 0.0);
		}

		Body += FString::Printf(TEXT(",%.3f,%.3f,%.3f,%.1f"),
			Row.GameMs, Row.RenderMs, Row.GpuMs, Row.MemMB);

		FString Events = FString::Join(Row.EdgeEvents, TEXT("|"));
		Body += FString::Printf(TEXT(",%s\n"), *CSVEscape(Events));
	}

	bool SaveCSV(const FString& Path, const FString& HeaderAndBody, FString& OutError)
	{
		if (!FFileHelper::SaveStringToFile(HeaderAndBody, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			OutError = FString::Printf(TEXT("CSV write failed: %s"), *Path);
			return false;
		}
		return true;
	}

	// ── tracked.jsonl ────────────────────────────────────────────────────

	bool SaveTrackedActorsJSONL(const FString& Path, const TArray<FTrackedActorRow>& Rows, FString& OutError)
	{
		FString Buf;
		Buf.Reserve(Rows.Num() * 96);
		for (const FTrackedActorRow& Row : Rows)
		{
			TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
			O->SetNumberField(TEXT("frame"), static_cast<double>(Row.Frame));
			O->SetNumberField(TEXT("time"), Row.Time);
			TSharedRef<FJsonObject> Actors = MakeShared<FJsonObject>();
			for (const TPair<FString, FActorState>& KV : Row.Actors)
			{
				TSharedRef<FJsonObject> A = MakeShared<FJsonObject>();
				A->SetBoolField(TEXT("resolved"), KV.Value.bResolved);
				if (KV.Value.bResolved)
				{
					TArray<TSharedPtr<FJsonValue>> Pos;
					Pos.Add(MakeShared<FJsonValueNumber>(KV.Value.Location.X));
					Pos.Add(MakeShared<FJsonValueNumber>(KV.Value.Location.Y));
					Pos.Add(MakeShared<FJsonValueNumber>(KV.Value.Location.Z));
					A->SetArrayField(TEXT("pos"), Pos);
					TArray<TSharedPtr<FJsonValue>> Rot;
					Rot.Add(MakeShared<FJsonValueNumber>(KV.Value.Rotation.Yaw));
					Rot.Add(MakeShared<FJsonValueNumber>(KV.Value.Rotation.Pitch));
					Rot.Add(MakeShared<FJsonValueNumber>(KV.Value.Rotation.Roll));
					A->SetArrayField(TEXT("rot"), Rot);
					TArray<TSharedPtr<FJsonValue>> Vel;
					Vel.Add(MakeShared<FJsonValueNumber>(KV.Value.Velocity.X));
					Vel.Add(MakeShared<FJsonValueNumber>(KV.Value.Velocity.Y));
					Vel.Add(MakeShared<FJsonValueNumber>(KV.Value.Velocity.Z));
					A->SetArrayField(TEXT("vel"), Vel);
				}
				Actors->SetObjectField(KV.Key, A);
			}
			O->SetObjectField(TEXT("actors"), Actors);
			Buf += WriteJsonToStringCondensed(O);
			Buf += TEXT("\n");
		}
		if (!FFileHelper::SaveStringToFile(Buf, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			OutError = FString::Printf(TEXT("write failed: %s"), *Path);
			return false;
		}
		return true;
	}

	bool LoadTrackedActorsJSONL(const FString& Path, TArray<FTrackedActorRow>& OutRows, FString& OutError)
	{
		FString Raw;
		if (!FFileHelper::LoadFileToString(Raw, *Path))
		{
			OutError = FString::Printf(TEXT("read failed: %s"), *Path);
			return false;
		}
		TArray<FString> Lines;
		Raw.ParseIntoArrayLines(Lines, false);
		for (const FString& L : Lines)
		{
			if (L.IsEmpty()) continue;
			TSharedPtr<FJsonObject> Obj;
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(L);
			if (!FJsonSerializer::Deserialize(Reader, Obj) || !Obj.IsValid()) continue;
			FTrackedActorRow Row;
			double F = 0; Obj->TryGetNumberField(TEXT("frame"), F); Row.Frame = static_cast<uint64>(F);
			Obj->TryGetNumberField(TEXT("time"), Row.Time);
			const TSharedPtr<FJsonObject>* Actors = nullptr;
			if (Obj->TryGetObjectField(TEXT("actors"), Actors) && Actors)
			{
				for (const auto& KV : (*Actors)->Values)
				{
					const TSharedPtr<FJsonObject>* AO = nullptr;
					if (!KV.Value.IsValid() || !KV.Value->TryGetObject(AO) || !AO) continue;
					FActorState S;
					(*AO)->TryGetBoolField(TEXT("resolved"), S.bResolved);
					if (S.bResolved)
					{
						const TArray<TSharedPtr<FJsonValue>>* Pos = nullptr;
						if ((*AO)->TryGetArrayField(TEXT("pos"), Pos) && Pos && Pos->Num() >= 3)
						{
							S.Location = FVector((*Pos)[0]->AsNumber(), (*Pos)[1]->AsNumber(), (*Pos)[2]->AsNumber());
						}
						const TArray<TSharedPtr<FJsonValue>>* Rot = nullptr;
						if ((*AO)->TryGetArrayField(TEXT("rot"), Rot) && Rot && Rot->Num() >= 3)
						{
							S.Rotation = FRotator((*Rot)[1]->AsNumber(), (*Rot)[0]->AsNumber(), (*Rot)[2]->AsNumber());
						}
						const TArray<TSharedPtr<FJsonValue>>* Vel = nullptr;
						if ((*AO)->TryGetArrayField(TEXT("vel"), Vel) && Vel && Vel->Num() >= 3)
						{
							S.Velocity = FVector((*Vel)[0]->AsNumber(), (*Vel)[1]->AsNumber(), (*Vel)[2]->AsNumber());
						}
					}
					Row.Actors.Add(FString(*KV.Key), S);
				}
			}
			OutRows.Add(Row);
		}
		return true;
	}

	// ── Recording-directory helpers ──────────────────────────────────────

	FString DefaultRecordingsRoot()
	{
		return FPaths::ProjectSavedDir() / TEXT("MCPRecordings");
	}

	FString MakeRecordingId(const FString& Override)
	{
		if (!Override.IsEmpty()) return Override;
		const FString Stamp = FDateTime::Now().ToString(TEXT("%Y%m%d-%H%M%S"));
		const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(4).ToLower();
		return FString::Printf(TEXT("recording-%s-%s"), *Stamp, *Suffix);
	}

	FString MakeRecordingDir(const FString& Root, const FString& Id)
	{
		const FString Base = Root.IsEmpty() ? DefaultRecordingsRoot() : Root;
		return Base / Id;
	}

	AActor* FindActorById(UWorld* World, const FString& Id)
	{
		if (!World || Id.IsEmpty()) return nullptr;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* A = *It;
			if (!A) continue;
			if (A->GetName() == Id) return A;
			const UClass* C = A->GetClass();
			if (C && (C->GetName() == Id || C->GetPathName() == Id)) return A;
			if (A->GetPathName().EndsWith(Id)) return A;
		}
		return nullptr;
	}
}
