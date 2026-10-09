// Automation coverage for replay_state (roadmap item 2a).
// Verifies deterministic scrub: interpolated pawn transform at an arbitrary time.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Handlers/GameplayHandlers.h"
#include "Dom/JsonObject.h"
#include "Misc/Paths.h"
#include "Misc/Guid.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "PIE/PIEInputRecorder.h"
#include "PIE/PIEFrameSampler.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerInput.h"
#include "Components/InputComponent.h"
#include "Engine/LocalPlayer.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "EnhancedPlayerInput.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "InputKeyEventArgs.h"
#include "PIE/PIESequenceFormat.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPIEReplayStateScrubTest,
	"UE_PIE_Automation.StateReplay.ScrubInterpolates",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPIEReplayStateScrubTest::RunTest(const FString& /*Parameters*/)
{
	const FString Folder = FPaths::ProjectSavedDir() / TEXT("MCPStateReplayTest") /
		FGuid::NewGuid().ToString(EGuidFormats::Digits);
	IFileManager::Get().MakeDirectory(*Folder, true);

	// pos_x goes 0 -> 100 -> 200 over t = 0, 1, 2. Sampling at t=0.5 must give 50.
	const FString Csv =
		TEXT("# rec\n")
		TEXT("frame,time,dt,pos_x,pos_y,pos_z,rot_yaw,rot_pitch,rot_roll,vel_x,vel_y,vel_z,speed2d,montage,event\n")
		TEXT("0,0.000,0.5,0,0,0,0,0,0,0,0,0,0,,\n")
		TEXT("1,1.000,0.5,100,0,0,0,0,0,0,0,0,0,,\n")
		TEXT("2,2.000,0.5,200,0,0,0,0,0,0,0,0,0,,\n");
	TestTrue(TEXT("write csv"), FFileHelper::SaveStringToFile(Csv, *(Folder / TEXT("recording.csv"))));

	// Scrub to t=0.5 -> pos_x should interpolate to 50.
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("recording_dir"), Folder);
		P->SetNumberField(TEXT("at_time"), 0.5);
		TSharedPtr<FJsonValue> Res = FGameplayHandlers::PieReplayState(P);
		const TSharedPtr<FJsonObject> O = Res.IsValid() ? Res->AsObject() : nullptr;
		TestTrue(TEXT("result object"), O.IsValid());
		if (O.IsValid())
		{
			bool bDet = false; O->TryGetBoolField(TEXT("deterministic"), bDet);
			TestTrue(TEXT("deterministic"), bDet);
			const TSharedPtr<FJsonObject>* Pawn = nullptr;
			if (O->TryGetObjectField(TEXT("pawn_state"), Pawn) && Pawn)
			{
				const TSharedPtr<FJsonObject>* Loc = nullptr;
				if ((*Pawn)->TryGetObjectField(TEXT("location"), Loc) && Loc)
				{
					double X = -1; (*Loc)->TryGetNumberField(TEXT("x"), X);
					TestTrue(TEXT("interpolated pos_x ~ 50"), FMath::IsNearlyEqual(X, 50.0, 0.01));
				}
			}
			int32 Total = 0; O->TryGetNumberField(TEXT("total_frames"), Total);
			TestEqual(TEXT("total frames"), Total, 3);
		}
	}

	// at_frame=2 -> pos_x exactly 200.
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("recording_dir"), Folder);
		P->SetNumberField(TEXT("at_frame"), 2);
		TSharedPtr<FJsonValue> Res = FGameplayHandlers::PieReplayState(P);
		const TSharedPtr<FJsonObject> O = Res.IsValid() ? Res->AsObject() : nullptr;
		if (O.IsValid())
		{
			const TSharedPtr<FJsonObject>* Pawn = nullptr;
			const TSharedPtr<FJsonObject>* Loc = nullptr;
			if (O->TryGetObjectField(TEXT("pawn_state"), Pawn) && Pawn &&
				(*Pawn)->TryGetObjectField(TEXT("location"), Loc) && Loc)
			{
				double X = -1; (*Loc)->TryGetNumberField(TEXT("x"), X);
				TestTrue(TEXT("frame 2 pos_x = 200"), FMath::IsNearlyEqual(X, 200.0, 0.01));
			}
		}
	}

	IFileManager::Get().DeleteDirectory(*Folder, false, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPIERawInputSequenceTest,
	"UE_PIE_Automation.RawInput.SequenceFormat",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPIERawInputSequenceTest::RunTest(const FString& /*Parameters*/)
{
	using namespace UE_PIE_Automation;
	FSequence Sequence;
	Sequence.SourceRecordingId = TEXT("raw-input-test");
	Sequence.ViewportSize = FVector2D(1280.0, 720.0);
	Sequence.InputFrameTimes = {0.1, 0.2};
	Sequence.WorldSegments = {{0, TEXT("/Game/Entry")}, {1, TEXT("/Game/Battle")}};

	FPIEInputEvent KeyDown;
	KeyDown.Type = EPIEInputEventType::KeyDown;
	KeyDown.TimeSeconds = 0.1;
	KeyDown.Order = 0;
	KeyDown.Map = TEXT("/Game/Entry");
	KeyDown.Key = TEXT("LeftControl");
	KeyDown.KeyCode = 17;
	Sequence.InputEvents.Add(KeyDown);

	FPIEInputEvent KeyUp = KeyDown;
	KeyUp.Type = EPIEInputEventType::KeyUp;
	KeyUp.Order = 1;
	Sequence.InputEvents.Add(KeyUp);

	FPIEInputEvent MouseDown;
	MouseDown.Type = EPIEInputEventType::MouseButtonDown;
	MouseDown.TimeSeconds = 0.2;
	MouseDown.Order = 2;
	MouseDown.InputFrame = 1;
	MouseDown.Map = TEXT("/Game/Battle");
	MouseDown.bRelativeMouse = true;
	MouseDown.Key = EKeys::LeftMouseButton.GetFName().ToString();
	MouseDown.Position = FVector2D(40.0, 80.0);
	MouseDown.PressedButtons.Add(MouseDown.Key);
	Sequence.InputEvents.Add(MouseDown);

	const TSharedRef<FJsonObject> Json = SequenceToJson(Sequence);
	FSequence RoundTripped;
	FString Error;
	TestTrue(TEXT("current raw input format round-trips"), SequenceFromJson(Json, RoundTripped, Error));
	TestEqual(TEXT("all events round-trip"), RoundTripped.InputEvents.Num(), 3);
	if (RoundTripped.InputEvents.Num() == 3)
	{
		TestTrue(TEXT("same-time order is retained"), RoundTripped.InputEvents[0].Order == 0 && RoundTripped.InputEvents[1].Order == 1);
		TestEqual(TEXT("pointer X is retained"), RoundTripped.InputEvents[2].Position.X, 40.0);
		TestTrue(TEXT("mouse button state is retained"), RoundTripped.InputEvents[2].PressedButtons.Contains(MouseDown.Key));
		TestTrue(TEXT("game mouse mode is retained"), RoundTripped.InputEvents[2].bRelativeMouse);
		TestEqual(TEXT("world travel frames are retained"), RoundTripped.WorldSegments[1].InputFrame, 1);
		TestEqual(TEXT("session time survives travel"), RoundTripped.InputFrameTimes.Last(), 0.2);
	}
	FSequence InvalidTimeline = Sequence;
	InvalidTimeline.InputFrameTimes[1] = 0.01;
	TestFalse(TEXT("world time reset is rejected"), SequenceFromJson(SequenceToJson(InvalidTimeline), RoundTripped, Error));
	FSequence InvalidFrame = Sequence;
	InvalidFrame.InputEvents.Last().InputFrame = 2;
	TestFalse(TEXT("events outside input frames are rejected"), SequenceFromJson(SequenceToJson(InvalidFrame), RoundTripped, Error));

	TSharedRef<FJsonObject> Reordered = SequenceToJson(Sequence);
	const TArray<TSharedPtr<FJsonValue>>* StoredEvents = nullptr;
	Reordered->TryGetArrayField(TEXT("input_events"), StoredEvents);
	if (StoredEvents && StoredEvents->Num() >= 2)
	{
		TArray<TSharedPtr<FJsonValue>> InvalidOrder = *StoredEvents;
		Swap(InvalidOrder[0], InvalidOrder[1]);
		Reordered->SetArrayField(TEXT("input_events"), InvalidOrder);
		FSequence Rejected;
		TestFalse(TEXT("out-of-order events are rejected"), SequenceFromJson(Reordered, Rejected, Error));
	}

	TSharedRef<FJsonObject> OldSequence = MakeShared<FJsonObject>();
	OldSequence->SetNumberField(TEXT("version"), 3);
	OldSequence->SetArrayField(TEXT("steps"), TArray<TSharedPtr<FJsonValue>>());
	TestFalse(TEXT("old recordings are rejected"), SequenceFromJson(OldSequence, RoundTripped, Error));
	TestTrue(TEXT("old recording error asks for a new recording"), Error.Contains(TEXT("new recording")));
	TSharedRef<FJsonObject> OldManifest = MakeShared<FJsonObject>();
	OldManifest->SetNumberField(TEXT("version"), 3);
	OldManifest->SetStringField(TEXT("id"), TEXT("old-recording"));
	FManifest RejectedManifest;
	TestFalse(TEXT("old manifest versions are rejected"), ManifestFromJson(OldManifest, RejectedManifest, Error));
	TestTrue(TEXT("old manifest error asks for a new recording"), Error.Contains(TEXT("new recording")));

	TSharedRef<FJsonObject> MissingInput = MakeShared<FJsonObject>();
	MissingInput->SetNumberField(TEXT("version"), kFormatVersion);
	MissingInput->SetArrayField(TEXT("steps"), TArray<TSharedPtr<FJsonValue>>());
	TestFalse(TEXT("missing raw input is rejected"), SequenceFromJson(MissingInput, RoundTripped, Error));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPIERawInputDispatchTest,
	"UE_PIE_Automation.RawInput.DispatchAndCancel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPIERawInputDispatchTest::RunTest(const FString& /*Parameters*/)
{
	using namespace UE_PIE_Automation;
	TArray<FPIEInputEvent> Events;
	FPIEInputEvent First;
	First.Type = EPIEInputEventType::KeyDown;
	First.Key = TEXT("W");
	First.TimeSeconds = 0.1;
	First.Order = 0;
	Events.Add(First);
	FPIEInputEvent Second = First;
	Second.bIsRepeat = true;
	Second.Order = 1;
	Events.Add(Second);
	FPIEInputEvent Later = First;
	Later.Type = EPIEInputEventType::KeyUp;
	Later.bIsRepeat = false;
	Later.TimeSeconds = 0.2;
	Later.InputFrame = 1;
	Later.Order = 2;
	Events.Add(Later);
	FPIEInputEvent RightDown = First;
	RightDown.Key = TEXT("D");
	RightDown.TimeSeconds = 0.2;
	RightDown.InputFrame = 1;
	RightDown.Order = 3;
	Events.Add(RightDown);
	FPIEInputEvent RightUp = RightDown;
	RightUp.Type = EPIEInputEventType::KeyUp;
	RightUp.Order = 4;
	Events.Add(RightUp);

	int32 Cursor = 0;
	TArray<int32> DispatchedOrders;
	TSet<FString> KeysDownDuringDispatch;
	auto DispatchEvent = [&DispatchedOrders, &KeysDownDuringDispatch](const FPIEInputEvent& Event, FString&)
	{
		DispatchedOrders.Add(Event.Order);
		if (Event.Type == EPIEInputEventType::KeyDown) KeysDownDuringDispatch.Add(Event.Key);
		if (Event.Type == EPIEInputEventType::KeyUp) KeysDownDuringDispatch.Remove(Event.Key);
		return true;
	};
	FString DispatchError;
	TestTrue(TEXT("due events dispatch successfully"),
		FPIEInputRouter::DispatchFrameEvents(Events, Cursor, 0, TEXT(""), DispatchEvent, DispatchError));
	TestEqual(TEXT("only the first processing frame is dispatched"), DispatchedOrders.Num(), 2);
	TestTrue(TEXT("taking an event does not also advance the cursor"), Cursor == 2);
	TestTrue(TEXT("the repeated W key remains held"), KeysDownDuringDispatch.Contains(TEXT("W")));
	TestTrue(TEXT("later events dispatch successfully"),
		FPIEInputRouter::DispatchFrameEvents(Events, Cursor, 1, TEXT(""), DispatchEvent, DispatchError));
	TestEqual(TEXT("all ordered events dispatch exactly once"), DispatchedOrders.Num(), Events.Num());
	TestTrue(TEXT("same-time W release and D press preserve order"),
		DispatchedOrders.IsValidIndex(4) && DispatchedOrders[2] == 2 && DispatchedOrders[3] == 3 && DispatchedOrders[4] == 4);
	TestTrue(TEXT("all direction keys are released"), KeysDownDuringDispatch.IsEmpty());
	FPIEInputRouter::DispatchFrameEvents(Events, Cursor, 1, TEXT(""), DispatchEvent, DispatchError);
	TestEqual(TEXT("completed input is not dispatched twice"), DispatchedOrders.Num(), Events.Num());

	int32 FailedCursor = 0;
	auto RejectEvent = [](const FPIEInputEvent&, FString& Error)
	{
		Error = TEXT("expected test dispatch failure");
		return false;
	};
	TestFalse(TEXT("dispatch errors are returned"),
		FPIEInputRouter::DispatchFrameEvents(Events, FailedCursor, 0, TEXT(""), RejectEvent, DispatchError));
	TestEqual(TEXT("failed events are not consumed"), FailedCursor, 0);
	FPIEInputEvent EnterRelease;
	EnterRelease.Type = EPIEInputEventType::MouseButtonUp;
	EnterRelease.InputFrame = 1;
	EnterRelease.Map = TEXT("/Game/Entry");
	FPIEInputEvent BattlePress = EnterRelease;
	BattlePress.Type = EPIEInputEventType::KeyDown;
	BattlePress.Map = TEXT("/Game/Battle");
	TArray<FPIEInputEvent> BoundaryEvents = {EnterRelease, BattlePress};
	int32 BoundaryCursor = 0;
	FPIEInputRouter::DispatchFrameEvents(BoundaryEvents, BoundaryCursor, 1, TEXT("/Game/Entry"), DispatchEvent, DispatchError);
	TestEqual(TEXT("Enter release executes before waiting for new map"), BoundaryCursor, 1);
	FPIEInputRouter::DispatchFrameEvents(BoundaryEvents, BoundaryCursor, 1, TEXT("/Game/Battle"), DispatchEvent, DispatchError);
	TestEqual(TEXT("game input executes after the new map is ready"), BoundaryCursor, 2);

	TMap<FKey, FPIEInputEvent> KeysDown;
	FPIEInputEvent KeyPress;
	KeyPress.Type = EPIEInputEventType::KeyDown;
	KeyPress.Key = TEXT("W");
	KeysDown.Add(FKey(FName(TEXT("W"))), KeyPress);
	TMap<FKey, FPIEInputEvent> ButtonsDown;
	FPIEInputEvent MousePress;
	MousePress.Type = EPIEInputEventType::MouseButtonDown;
	MousePress.Key = EKeys::LeftMouseButton.GetFName().ToString();
	MousePress.PressedButtons.Add(MousePress.Key);
	ButtonsDown.Add(EKeys::LeftMouseButton, MousePress);

	const TArray<FPIEInputEvent> Releases = FPIEInputRouter::BuildHeldInputReleaseEvents(KeysDown, ButtonsDown);
	TestEqual(TEXT("cancel creates a release for each held input"), Releases.Num(), 2);
	bool bKeyReleased = false;
	bool bButtonReleased = false;
	for (const FPIEInputEvent& Release : Releases)
	{
		bKeyReleased |= Release.Type == EPIEInputEventType::KeyUp && Release.Key == TEXT("W");
		bButtonReleased |= Release.Type == EPIEInputEventType::MouseButtonUp
			&& Release.Key == EKeys::LeftMouseButton.GetFName().ToString()
			&& Release.PressedButtons.IsEmpty();
	}
	TestTrue(TEXT("held key is released on cancel"), bKeyReleased);
	TestTrue(TEXT("held mouse button is released on cancel"), bButtonReleased);
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPIESamplerWithoutPawnTest,
	"UE_PIE_Automation.RawInput.SamplerWithoutPawn",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPIESamplerWithoutPawnTest::RunTest(const FString& /*Parameters*/)
{
	using namespace UE_PIE_Automation;
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("test world"), World)) return false;
	APlayerController* Controller = World->SpawnActor<APlayerController>();
	if (TestNotNull(TEXT("player controller"), Controller))
	{
		World->AddController(Controller);
		TestNull(TEXT("controller has no pawn"), Controller->GetPawn());
		FPIEFrameSampler Sampler;
		TestTrue(TEXT("raw recording sampler attaches without a pawn"), Sampler.AttachToPIE(World));
		Sampler.QueueMarker(TEXT("without-pawn"));
		const FCSVRow Row = Sampler.SampleFrame(World, 1, 0.1, 0.1);
		TestTrue(TEXT("markers are sampled without a pawn"), Row.EdgeEvents.Contains(TEXT("mark:without-pawn")));
		Sampler.Reset();
		UWorld* NextWorld = UWorld::CreateWorld(EWorldType::Game, false);
		APlayerController* NextController = NextWorld->SpawnActor<APlayerController>();
		NextWorld->AddController(NextController);
		TestTrue(TEXT("sampler attaches to first world"), Sampler.AttachToPIE(World));
		TestTrue(TEXT("sampler rebinds to next world without a pawn"), Sampler.AttachToPIE(NextWorld));
		Sampler.QueueMarker(TEXT("after-travel"));
		TestTrue(TEXT("sampling continues after travel"), Sampler.SampleFrame(NextWorld, 2, 0.2, 0.1).EdgeEvents.Contains(TEXT("mark:after-travel")));
		Sampler.Reset();
		NextWorld->DestroyWorld(false);
	}
	World->DestroyWorld(false);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPIERawInputConsumersTest,
	"UE_PIE_Automation.RawInput.LegacyAndEnhancedConsumers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPIERawInputConsumersTest::RunTest(const FString& /*Parameters*/)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("input test world"), World)) return false;
	APlayerController* PC = World->SpawnActor<APlayerController>();
	World->AddController(PC);
	AActor* Actor = World->SpawnActor<AActor>();
	TestNull(TEXT("input consumer does not require a pawn"), PC->GetPawn());

	PC->PlayerInput = NewObject<UPlayerInput>(PC);
	UInputComponent* Legacy = NewObject<UInputComponent>(Actor);
	int32 Presses = 0;
	FInputKeyBinding Binding(FInputChord(EKeys::W), IE_Pressed);
	Binding.KeyDelegate.GetDelegateForManualSet().BindLambda([&Presses]() { ++Presses; });
	Legacy->KeyBindings.Add(MoveTemp(Binding));
	Legacy->BindAxisKey(EKeys::MouseX);
	PC->InputKey(FInputKeyEventArgs::CreateSimulated(EKeys::W, IE_Pressed, 1.0f));
	PC->InputKey(FInputKeyEventArgs::CreateSimulated(EKeys::MouseX, IE_Axis, 5.0f));
	PC->PlayerInput->ProcessInputStack({Legacy}, 1.0f / 60.0f, false);
	TestEqual(TEXT("raw key reaches legacy actor binding once"), Presses, 1);
	TestTrue(TEXT("raw mouse reaches legacy axis"), Legacy->GetAxisKeyValue(EKeys::MouseX) > 0.0f);
	PC->InputKey(FInputKeyEventArgs::CreateSimulated(EKeys::W, IE_Released, 0.0f));
	PC->PlayerInput->ProcessInputStack({Legacy}, 1.0f / 60.0f, false);
	TestFalse(TEXT("legacy key released"), PC->PlayerInput->IsPressed(EKeys::W));

	ULocalPlayer* LocalPlayer = NewObject<ULocalPlayer>(GEngine);
	LocalPlayer->PlayerController = PC;
	PC->Player = LocalPlayer;
	PC->PlayerInput = NewObject<UEnhancedPlayerInput>(PC);
	UEnhancedInputComponent* Enhanced = NewObject<UEnhancedInputComponent>(Actor);
	UEnhancedInputLocalPlayerSubsystem* Subsystem = NewObject<UEnhancedInputLocalPlayerSubsystem>(LocalPlayer);
	UInputMappingContext* Context = NewObject<UInputMappingContext>();
	UInputAction* Move = NewObject<UInputAction>();
	Move->ValueType = EInputActionValueType::Axis1D;
	UInputAction* Look = NewObject<UInputAction>();
	Look->ValueType = EInputActionValueType::Axis1D;
	Context->MapKey(Move, EKeys::W);
	Context->MapKey(Look, EKeys::MouseX);
	Enhanced->BindActionValue(Move);
	Enhanced->BindActionValue(Look);
	FModifyContextOptions Options;
	Options.bForceImmediately = true;
	Subsystem->AddMappingContext(Context, 0, Options);
	PC->InputKey(FInputKeyEventArgs::CreateSimulated(EKeys::W, IE_Pressed, 1.0f));
	PC->InputKey(FInputKeyEventArgs::CreateSimulated(EKeys::MouseX, IE_Axis, 5.0f));
	PC->PlayerInput->ProcessInputStack({Enhanced}, 1.0f / 60.0f, false);
	TestTrue(TEXT("raw key reaches Enhanced Input on ordinary actor"), Enhanced->GetBoundActionValue(Move).Get<float>() > 0.0f);
	TestTrue(TEXT("raw mouse reaches Enhanced Input"), Enhanced->GetBoundActionValue(Look).Get<float>() > 0.0f);
	PC->PlayerInput->ProcessInputStack({Enhanced}, 1.0f / 60.0f, false);
	TestTrue(TEXT("held Enhanced key persists without repeated injection"), Enhanced->GetBoundActionValue(Move).Get<float>() > 0.0f);
	PC->InputKey(FInputKeyEventArgs::CreateSimulated(EKeys::W, IE_Released, 0.0f));
	PC->PlayerInput->ProcessInputStack({Enhanced}, 1.0f / 60.0f, false);
	TestEqual(TEXT("Enhanced key releases"), Enhanced->GetBoundActionValue(Move).Get<float>(), 0.0f);
	Subsystem->RemoveMappingContext(Context, Options);
	PC->Player = nullptr;
	LocalPlayer->PlayerController = nullptr;
	World->DestroyWorld(false);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
