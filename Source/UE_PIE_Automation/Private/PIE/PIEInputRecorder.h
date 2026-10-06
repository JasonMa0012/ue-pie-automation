#pragma once

#include "CoreMinimal.h"
#include "Framework/Application/IInputProcessor.h"
#include "Input/Events.h"
#include "PIEFrameSampler.h"
#include "PIESequenceFormat.h"
#include "UObject/WeakObjectPtrTemplates.h"

class UWorld;
class AActor;
class SViewport;
class SWindow;
struct FGeometry;

/**
 * PIE input recorder: opt-in arm-then-record. Module-owned singleton, hooked
 * to FEditorDelegates::BeginPIE / EndPIE and FCoreDelegates::OnEndFrame. The
 * recorder is dormant in Idle state; only after pie_record_arm does it bind
 * the end-of-frame tick.
 *
 * Recording artifacts land in <Saved>/MCPRecordings/<id>/:
 *   manifest.json, sequence.json, recording.csv
 *
 * State machine:
 *   Idle              No recording armed; no end-frame tick.
 *   Armed             Will start on the next BeginPIE.
 *   WaitingForPawn    BeginPIE fired; sampler waiting for the player pawn.
 *   Recording         Sampler attached; emit one row per end-of-frame.
 *                     Returns to Idle (and writes artifacts) on EndPIE
 *                     or pie_record_stop.
 */
namespace UE_PIE_Automation
{
	enum class ERecorderState : uint8
	{
		Idle,
		Armed,
		WaitingForPawn,
		Recording
	};

	struct FRecorderArmConfig
	{
		FString Id;                       // empty = auto-generate
		FString RecordingsRoot;           // empty = ProjectSavedDir/MCPRecordings
		TArray<FString> ActionPaths;      // empty = record every bound action
		TArray<FString> TrackedValuePaths;
		// Tracked world actors. Each id is matched against an actor in the PIE
		// world by exact name, by class name, or by full path. First match
		// wins; misses are recorded as { resolved: false } and re-tried each
		// frame.
		TArray<FString> TrackedActorIds;
		// When true the recorder also dispatches StartRecording / StopRecording
		// on the open Take Recorder panel in lockstep with BeginPIE / EndPIE.
		// Requires the Take Recorder plugin + an open panel; falls back to
		// a no-op with a diagnostic in TakeRecorderStatus otherwise.
		bool bTakeRecord = false;
		// Multi-client PIE: which local player to sample. 0 = first
		// (single-client default), 1+ selects subsequent local players.
		int32 ClientId = 0;
		float AxisThreshold = 0.15f;
		int32 SampleHz = 60;
		int32 PinFPS = 60;                // 0 to skip the t.MaxFPS pin
		bool bCapturePawnState = true;
		bool bCaptureMontage = true;
		int64 RngSeed = 0;                // 0 = auto-generate
		bool bUserSuppliedSeed = false;
	};

	struct FRecorderStatus
	{
		ERecorderState State = ERecorderState::Idle;
		FString Id;
		FString RecordingDir;
		int32 CurrentFrame = 0;
		double ElapsedSeconds = 0.0;
		int32 TrackedActionCount = 0;
		int32 RawInputEventCount = 0;
		FString InputError;
	};

	struct FRecorderFinishResult
	{
		bool bSuccess = false;
		FString Error;
		FString Id;
		FString RecordingDir;
		FString ManifestPath;
		FString CSVPath;
		FString SequencePath;
		int32 TotalFrames = 0;
		double DurationSeconds = 0.0;
		TArray<FString> DiscoveredActions;
		TArray<FMarker> Markers;
		bool bTakeRecordAttempted = false;
		FString TakeRecorderStatus;
		int32 RawInputEventCount = 0;
		FString InputError;
	};

	class FPIEInputRouter final : public IInputProcessor, public TSharedFromThis<FPIEInputRouter>
	{
	public:
		static FPIEInputRouter& Get();
		static const FPIEInputEvent* TakeNextDueEvent(const TArray<FPIEInputEvent>& Events, int32& Cursor, double ElapsedMs);
		static bool DispatchDueEvents(const TArray<FPIEInputEvent>& Events, int32& Cursor, double ElapsedMs,
			TFunctionRef<bool(const FPIEInputEvent&, FString&)> DispatchEvent, FString& OutError);
		static TArray<FPIEInputEvent> BuildHeldInputReleaseEvents(
			const TMap<FKey, FPIEInputEvent>& KeysDown,
			const TMap<FKey, FPIEInputEvent>& ButtonsDown);
		void Shutdown();

		bool BeginRecording(UWorld* World, double BaseTimeSeconds, FString& OutError);
		void EndRecording(TArray<FPIEInputEvent>& OutEvents, FVector2D& OutViewportSize);
		bool BeginReplay(const FSequence& Sequence, FString& OutError);
		bool DispatchDue(double ElapsedMs, FString& OutError);
		int32 EndReplay();

		int32 GetExecutedEventCount() const { return ExecutedEventCount; }
		int32 GetRecordedEventCount() const;
		const FString& GetLastError() const { return LastError; }

		virtual void Tick(float DeltaTime, FSlateApplication& SlateApp, TSharedRef<ICursor> Cursor) override;
		virtual bool HandleKeyDownEvent(FSlateApplication& SlateApp, const FKeyEvent& Event) override;
		virtual bool HandleKeyUpEvent(FSlateApplication& SlateApp, const FKeyEvent& Event) override;
		virtual bool HandleMouseMoveEvent(FSlateApplication& SlateApp, const FPointerEvent& Event) override;
		virtual bool HandleMouseButtonDownEvent(FSlateApplication& SlateApp, const FPointerEvent& Event) override;
		virtual bool HandleMouseButtonUpEvent(FSlateApplication& SlateApp, const FPointerEvent& Event) override;
		virtual bool HandleMouseButtonDoubleClickEvent(FSlateApplication& SlateApp, const FPointerEvent& Event) override;
		virtual bool HandleMouseWheelOrGestureEvent(FSlateApplication& SlateApp, const FPointerEvent& Event, const FPointerEvent* GestureEvent) override;
		virtual const TCHAR* GetDebugName() const override { return TEXT("PIE Raw Input Recorder/Replayer"); }

	private:
		enum class EMode : uint8 { Idle, Recording, Replaying };
		bool FindViewport(TSharedPtr<SViewport>& OutWidget, TSharedPtr<SWindow>& OutWindow, FGeometry& OutGeometry) const;
		bool IsTargetWindowActive(FSlateApplication& SlateApp) const;
		bool IsKeyboardTargetActive(FSlateApplication& SlateApp) const;
		bool IsInsideViewport(const FVector2D& ScreenPosition, FVector2D& OutLocalPosition) const;
		bool RegisterInputProcessor();
		void UnregisterInputProcessor();
		FPIEInputEvent MakePointerEvent(EPIEInputEventType Type, const FPointerEvent& Event) const;
		FPIEInputEvent MakeKeyEvent(EPIEInputEventType Type, const FKeyEvent& Event) const;
		void Record(FPIEInputEvent&& Event);
		bool Dispatch(const FPIEInputEvent& Event, FString& OutError);
		void ReleaseHeldInputs(FString& OutError);
		FModifierKeysState MakeModifiers(const FPIEInputEvent& Event) const;
		TSet<FKey> MakePressedButtons(const FPIEInputEvent& Event) const;
		FVector2D ToScreenPosition(const FVector2D& LocalPosition) const;
		FVector2D ToScreenDelta(const FVector2D& LocalPosition, const FVector2D& LocalDelta) const;

		TSharedPtr<FPIEInputRouter> RegisteredHandle;
		TWeakPtr<SViewport> ViewportWidget;
		TWeakPtr<SWindow> TargetWindow;
		TWeakObjectPtr<UWorld> RecordingWorld;
		EMode Mode = EMode::Idle;
		FVector2D RecordedViewportSize = FVector2D::ZeroVector;
		FVector2D ReplayViewportSize = FVector2D::ZeroVector;
		double RecordingBaseTime = 0.0;
		int32 NextEventOrder = 0;
		int32 NextReplayEvent = 0;
		int32 ExecutedEventCount = 0;
		bool bDispatchingSynthetic = false;
		TArray<FPIEInputEvent> RecordedEvents;
		TArray<FPIEInputEvent> ReplayEvents;
		TSet<FKey> RecordingKeysDown;
		TSet<FKey> PhysicalKeysSuppressed;
		TSet<FKey> PhysicalButtonsSuppressed;
		TMap<FKey, FPIEInputEvent> InjectedKeysDown;
		TMap<FKey, FPIEInputEvent> InjectedButtonsDown;
		FString LastError;
	};

	class FPIEInputRecorder
	{
	public:
		static FPIEInputRecorder& Get();

		// Lifecycle. Init binds the editor delegates; Shutdown clears them.
		void Init();
		void Shutdown();

		// Arm a recording for the next BeginPIE (or immediately if PIE is
		// already running). Returns true on success and writes a short
		// description into OutMessage; on failure (already recording, etc.)
		// returns false with OutError populated.
		bool Arm(const FRecorderArmConfig& Cfg, FString& OutError, FString& OutMessage);
		bool Disarm(FString& OutError);

		// Force-finalise the in-flight recording even if EndPIE has not
		// fired. Returns the same shape EndPIE would have produced.
		FRecorderFinishResult ForceStop();

		// Insert a marker into the current frame's edge events. Returns
		// false (and writes "not recording" diagnostic) when idle.
		bool Mark(const FString& Label, FRecorderStatus& OutStatus);

		FRecorderStatus GetStatus() const;

		bool IsActive() const { return State != ERecorderState::Idle; }

	private:
		void OnBeginPIE(bool bIsSimulating);
		void OnEndPIE(bool bIsSimulating);
		void OnEndFrame();

		FRecorderFinishResult FinaliseCurrent();
		void ApplyFPSPin(UWorld* PIEWorld);

		FRecorderArmConfig Pending;
		bool bArmed = false;
		ERecorderState State = ERecorderState::Idle;
		FPIEFrameSampler Sampler;
		FString CurrentId;
		FString CurrentDir;
		FString CSVHeader;
		FString CSVBody;
		FCSVHeader CSVHdr;
		TArray<FCSVRow> Rows;
		TArray<FTrackedActorRow> ActorRows;
		TMap<FString, TWeakObjectPtr<AActor>> TrackedActorCache;
		TArray<FMarker> Markers;
		TArray<FPIEInputEvent> InputEvents;
		FVector2D RecordedViewportSize = FVector2D::ZeroVector;
		bool bRawInputStarted = false;
		FString InputError;
		double StartTime = 0.0;
		FString StartedAt;

		FDelegateHandle BeginPIEHandle;
		FDelegateHandle EndPIEHandle;
		FDelegateHandle OnEndFrameHandle;
		bool bEndFrameBound = false;
	};
}
