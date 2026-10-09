#include "PIEInputRecorder.h"
#include "PIETakeRecorderBridge.h"
#include "UE_PIE_AutomationModule.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "Framework/Application/SlateApplication.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GenericPlatform/GenericWindow.h"
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "HAL/PlatformTime.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/CoreDelegates.h"
#include "Misc/DateTime.h"
#include "Misc/Paths.h"
#include "Math/UnrealMathUtility.h"
#include "Containers/StringConv.h"
#include "Input/Events.h"
#include "InputCoreTypes.h"
#include "Layout/Geometry.h"
#include "Layout/WidgetPath.h"
#include "Slate/SceneViewport.h"
#include "Widgets/SViewport.h"
#include "Widgets/SWindow.h"
#include "UObject/WeakObjectPtrTemplates.h"

namespace UE_PIE_Automation
{
	namespace
	{
		void SampleTrackedActors(UWorld* World,
		                         const TArray<FString>& Ids,
		                         TMap<FString, TWeakObjectPtr<AActor>>& Cache,
		                         FTrackedActorRow& OutRow)
		{
			for (const FString& Id : Ids)
			{
				FActorState S;
				AActor* A = nullptr;
				if (TWeakObjectPtr<AActor>* Cached = Cache.Find(Id))
				{
					A = Cached->Get();
				}
				if (!A)
				{
					A = FindActorById(World, Id);
					if (A) Cache.Add(Id, A);
				}
				if (A)
				{
					S.Location = A->GetActorLocation();
					S.Rotation = A->GetActorRotation();
					S.Velocity = A->GetVelocity();
					S.bResolved = true;
				}
				OutRow.Actors.Add(Id, S);
			}
		}
	}

	FPIEInputRecorder& FPIEInputRecorder::Get()
	{
		static FPIEInputRecorder Instance;
		return Instance;
	}

	void FPIEInputRecorder::Init()
	{
		if (BeginPIEHandle.IsValid()) return;
		BeginPIEHandle = FEditorDelegates::BeginPIE.AddLambda([this](bool bSim)
		{
			this->OnBeginPIE(bSim);
		});
		EndPIEHandle = FEditorDelegates::EndPIE.AddLambda([this](bool bSim)
		{
			this->OnEndPIE(bSim);
		});
	}

	void FPIEInputRecorder::Shutdown()
	{
		FPIEInputRouter::Get().Shutdown();
		if (BeginPIEHandle.IsValid()) FEditorDelegates::BeginPIE.Remove(BeginPIEHandle);
		if (EndPIEHandle.IsValid())   FEditorDelegates::EndPIE.Remove(EndPIEHandle);
		BeginPIEHandle.Reset();
		EndPIEHandle.Reset();
		if (bEndFrameBound && OnEndFrameHandle.IsValid())
		{
			FCoreDelegates::OnEndFrame.Remove(OnEndFrameHandle);
		}
		OnEndFrameHandle.Reset();
		bEndFrameBound = false;
		State = ERecorderState::Idle;
		bArmed = false;
		Rows.Reset();
		Markers.Reset();
	}

	bool FPIEInputRecorder::Arm(const FRecorderArmConfig& Cfg, FString& OutError, FString& OutMessage)
	{
		if (State == ERecorderState::Recording || State == ERecorderState::WaitingForPawn)
		{
			OutError = TEXT("Recording already in flight; call pie_record_stop or wait for EndPIE.");
			return false;
		}

		Pending = Cfg;
		RecordedSequence = FSequence();
		bRawInputStarted = false;
		InputError.Reset();
		if (!Pending.bUserSuppliedSeed || Pending.RngSeed == 0)
		{
			Pending.RngSeed = FDateTime::Now().GetTicks() & 0x7FFFFFFF;
		}
		CurrentId = MakeRecordingId(Pending.Id);
		CurrentDir = MakeRecordingDir(Pending.RecordingsRoot, CurrentId);

		bArmed = true;
		State = ERecorderState::Armed;
		OutMessage = FString::Printf(TEXT("Armed: id=%s dir=%s seed=%lld"),
			*CurrentId, *CurrentDir, static_cast<long long>(Pending.RngSeed));

		// If PIE is already running, transition straight to WaitingForPawn so
		// the next end-of-frame begins sampling without a fresh BeginPIE.
		if (GEditor && GEditor->PlayWorld)
		{
			OnBeginPIE(false);
		}

		return true;
	}

	bool FPIEInputRecorder::Disarm(FString& OutError)
	{
		if (State == ERecorderState::Recording || State == ERecorderState::WaitingForPawn)
		{
			OutError = TEXT("Recording is in flight; pie_record_stop to finalize.");
			return false;
		}
		bArmed = false;
		State = ERecorderState::Idle;
		Pending = FRecorderArmConfig();
		CurrentId.Reset();
		CurrentDir.Reset();
		return true;
	}

	void FPIEInputRecorder::OnBeginPIE(bool /*bIsSimulating*/)
	{
		if (!bArmed) return;
		bArmed = false;

		FPIEFrameSampler::FConfig SC;
		SC.ActionPaths       = Pending.ActionPaths;
		SC.TrackedValuePaths = Pending.TrackedValuePaths;
		SC.AxisThreshold     = Pending.AxisThreshold;
		SC.bCapturePawnState = Pending.bCapturePawnState;
		SC.bCaptureMontage   = Pending.bCaptureMontage;
		SC.ClientIndex       = Pending.ClientId;
		Sampler.Reset();
		Sampler.SetConfig(SC);

			Rows.Reset();
			ActorRows.Reset();
			Markers.Reset();
			RecordedSequence = FSequence();
				bRawInputStarted = false;
			InputError.Reset();
		StartedAt = ISOTimestampNow();

		State = ERecorderState::WaitingForPawn;

		if (Pending.bTakeRecord)
		{
			FString TakeMsg;
			const bool bStarted = TakeRecorderBridge::StartFromPanel(TakeMsg);
			UE_LOG(LogUE_PIE_Automation, Log, TEXT("[PIE-REC] take_record: %s (%s)"),
				bStarted ? TEXT("started") : TEXT("skipped"), *TakeMsg);
		}

		if (!bEndFrameBound)
		{
			OnEndFrameHandle = FCoreDelegates::OnEndFrame.AddLambda([this]()
			{
				this->OnEndFrame();
			});
			bEndFrameBound = true;
		}

		UE_LOG(LogUE_PIE_Automation, Log, TEXT("[PIE-REC] Armed → BeginPIE: id=%s, waiting for pawn"), *CurrentId);
	}

	void FPIEInputRecorder::ApplyFPSPin(UWorld* PIEWorld)
	{
		if (Pending.PinFPS <= 0) return;
		if (!PIEWorld || !GEngine) return;
		const FString Cmd = FString::Printf(TEXT("t.MaxFPS %d"), Pending.PinFPS);
		GEngine->Exec(PIEWorld, *Cmd);
		UE_LOG(LogUE_PIE_Automation, Log, TEXT("[PIE-REC] %s"), *Cmd);
	}

	void FPIEInputRecorder::OnEndFrame()
	{
		if (State == ERecorderState::Idle) return;
		UWorld* PIEWorld = nullptr;
		if (GEditor) PIEWorld = GEditor->PlayWorld;
		if (!PIEWorld) return;

		if (State == ERecorderState::WaitingForPawn)
		{
			if (Sampler.AttachToPIE(PIEWorld))
			{
				// Apply RNG seed and FPS pin once attached.
				FMath::RandInit(static_cast<int32>(Pending.RngSeed));
				ApplyFPSPin(PIEWorld);

				CSVHdr = FCSVHeader();
				CSVHdr.RecordingId = CurrentId;
				CSVHdr.SampleHz = Pending.SampleHz;
				CSVHdr.RngSeed = Pending.RngSeed;
				CSVHdr.Actions = Sampler.GetActions();
				CSVHdr.TrackedValues = Sampler.GetTrackedValues();
				CSVHeader = BuildCSVHeader(CSVHdr);
				CSVBody.Reset();

				State = ERecorderState::Recording;
			}
			return;
		}

			if (State == ERecorderState::Recording)
			{
				const double Dt = PIEWorld->GetDeltaSeconds();
				const uint64 FrameNum = static_cast<uint64>(Rows.Num());
				if (!bRawInputStarted && InputError.IsEmpty())
				{
					bRawInputStarted = FPIEInputRouter::Get().BeginRecording(PIEWorld, Pending.ClientId, InputError);
					if (!InputError.IsEmpty())
					{
						UE_LOG(LogUE_PIE_Automation, Error, TEXT("[PIE-REC] Raw input capture failed: %s"), *InputError);
					}
				}
				if (!Sampler.AttachToPIE(PIEWorld)) return;
				const double GameTime = FPIEInputRouter::Get().GetElapsedSeconds();
				if (!Rows.IsEmpty() && GameTime <= Rows.Last().Time) return;
				FCSVRow Row = Sampler.SampleFrame(PIEWorld, FrameNum, GameTime, Dt);

			// Lift mark:* edge events into the manifest markers list.
			for (const FString& E : Row.EdgeEvents)
			{
				if (E.StartsWith(TEXT("mark:")))
				{
					FMarker M;
					M.Frame = FrameNum;
					M.Time = GameTime;
					M.Label = E.RightChop(5);
					Markers.Add(M);
				}
			}

			if (Pending.TrackedActorIds.Num() > 0)
			{
				FTrackedActorRow AR;
				AR.Frame = FrameNum;
				AR.Time = GameTime;
				SampleTrackedActors(PIEWorld, Pending.TrackedActorIds, TrackedActorCache, AR);
				ActorRows.Add(MoveTemp(AR));
			}

			// Detect late-discovered actions and rebuild CSV header.
			const TArray<FActionSpec>& CurrentActions = Sampler.GetActions();
			if (CurrentActions.Num() != CSVHdr.Actions.Num())
			{
				CSVHdr.Actions = CurrentActions;
				CSVHdr.TrackedValues = Sampler.GetTrackedValues();
				CSVHeader = BuildCSVHeader(CSVHdr);
				CSVBody.Reset();
				for (const FCSVRow& Prev : Rows)
				{
					AppendCSVRow(CSVBody, Prev, CSVHdr);
				}
			}

			AppendCSVRow(CSVBody, Row, CSVHdr);
			Rows.Add(MoveTemp(Row));
		}
	}

	void FPIEInputRecorder::OnEndPIE(bool /*bIsSimulating*/)
	{
		if (State == ERecorderState::Idle) return;
		FinaliseCurrent();
	}

	FRecorderFinishResult FPIEInputRecorder::FinaliseCurrent()
	{
		FRecorderFinishResult R;
		if (State == ERecorderState::Idle)
		{
			R.Error = TEXT("Not recording");
			return R;
		}
		const bool bHadData = Rows.Num() > 0 && Sampler.IsAttached();
		R.Id = CurrentId;
		R.RecordingDir = CurrentDir;
		FPIEInputRouter::Get().EndRecording(RecordedSequence);
		R.RawInputEventCount = RecordedSequence.InputEvents.Num();
		R.InputError = InputError;
		if (!InputError.IsEmpty())
		{
			R.Error = InputError;
			if (bEndFrameBound && OnEndFrameHandle.IsValid())
			{
				FCoreDelegates::OnEndFrame.Remove(OnEndFrameHandle);
				OnEndFrameHandle.Reset();
				bEndFrameBound = false;
			}
			State = ERecorderState::Idle;
			CurrentId.Reset();
			CurrentDir.Reset();
			Rows.Reset();
			ActorRows.Reset();
			TrackedActorCache.Reset();
			Markers.Reset();
			RecordedSequence = FSequence();
				bRawInputStarted = false;
			InputError.Reset();
			return R;
		}

		// Even if zero frames were recorded (PIE ended before pawn appeared),
		// we still tear down state cleanly and report what happened.
		if (!bHadData)
		{
			UE_LOG(LogUE_PIE_Automation, Log, TEXT("[PIE-REC] EndPIE without samples (id=%s)"), *CurrentId);
			R.bSuccess = true;
			R.TotalFrames = 0;
			R.DurationSeconds = 0.0;

			if (bEndFrameBound && OnEndFrameHandle.IsValid())
			{
				FCoreDelegates::OnEndFrame.Remove(OnEndFrameHandle);
				OnEndFrameHandle.Reset();
				bEndFrameBound = false;
			}
			State = ERecorderState::Idle;
			CurrentId.Reset();
			CurrentDir.Reset();
			Rows.Reset();
			ActorRows.Reset();
			TrackedActorCache.Reset();
			Markers.Reset();
			RecordedSequence = FSequence();
				bRawInputStarted = false;
			return R;
		}

		IFileManager::Get().MakeDirectory(*CurrentDir, true);

		const FString CSVPath = CurrentDir / TEXT("recording.csv");
		const FString SeqPath = CurrentDir / TEXT("sequence.json");
		const FString ManPath = CurrentDir / TEXT("manifest.json");

		FString WriteErr;
		const FString FullCSV = CSVHeader + CSVBody;
		if (!SaveCSV(CSVPath, FullCSV, WriteErr))
		{
			R.Error = WriteErr;
			State = ERecorderState::Idle;
			return R;
		}

		FSequence Seq = MoveTemp(RecordedSequence);
		Seq.Version = kFormatVersion;
		Seq.SourceRecordingId = CurrentId;
		Seq.SettleMs = 500;
		Seq.SampleHz = Pending.SampleHz;
		Seq.RngSeed = Pending.RngSeed;
		const double BaseTime = Rows[0].Time;
		for (const FMarker& Marker : Markers)
		{
			FStep Step;
			Step.Type = EStepType::Mark;
			Step.DelayMs = FMath::Max(0, FMath::RoundToInt((Marker.Time - BaseTime) * 1000.0));
			Step.Label = Marker.Label;
			Seq.Steps.Add(MoveTemp(Step));
		}
		if (!SaveSequence(SeqPath, Seq, WriteErr))
		{
			R.Error = WriteErr;
			State = ERecorderState::Idle;
			return R;
		}

		FManifest M;
		M.Id = CurrentId;
		M.StartedAt = StartedAt;
		M.EndedAt = ISOTimestampNow();
		M.DurationSeconds = Rows.Num() >= 2 ? (Rows.Last().Time - Rows[0].Time) : 0.0;
		M.TotalFrames = Rows.Num();
		M.SampleHz = Pending.SampleHz;
		M.PinMaxFPS = Pending.PinFPS;
		M.RngSeed = Pending.RngSeed;
		M.PIEWorld = Sampler.GetPIEWorldPath();
		M.PawnClass = Sampler.GetPawnClassPath();
		M.AxisThreshold = Pending.AxisThreshold;
		M.Actions = Sampler.GetActions();
		M.TrackedValues = Sampler.GetTrackedValues();
		M.Markers = Markers;
		M.CSVFile = TEXT("recording.csv");
		M.SequenceFile = TEXT("sequence.json");
		M.TrackedActorIds = Pending.TrackedActorIds;
		M.ClientId = Pending.ClientId;

		if (ActorRows.Num() > 0)
		{
			const FString JsonlPath = CurrentDir / TEXT("tracked.jsonl");
			if (!SaveTrackedActorsJSONL(JsonlPath, ActorRows, WriteErr))
			{
				UE_LOG(LogUE_PIE_Automation, Warning, TEXT("[PIE-REC] tracked.jsonl write failed: %s"), *WriteErr);
			}
			else
			{
				M.TrackedActorsFile = TEXT("tracked.jsonl");
			}
		}

		if (!SaveManifest(ManPath, M, WriteErr))
		{
			R.Error = WriteErr;
			State = ERecorderState::Idle;
			return R;
		}

		R.bSuccess = true;
		R.ManifestPath = ManPath;
		R.CSVPath = CSVPath;
		R.SequencePath = SeqPath;
		R.TotalFrames = M.TotalFrames;
		R.DurationSeconds = M.DurationSeconds;
		R.RawInputEventCount = Seq.InputEvents.Num();

		// Drive Take Recorder Stop in lockstep with the input recorder
		// finalise. Report the outcome so the user knows what happened.
		if (Pending.bTakeRecord)
		{
			FString TakeMsg;
			const bool bStopped = TakeRecorderBridge::StopFromPanel(TakeMsg);
			R.bTakeRecordAttempted = true;
			R.TakeRecorderStatus = bStopped
				? FString::Printf(TEXT("stopped: %s"), *TakeMsg)
				: FString::Printf(TEXT("skipped: %s"), *TakeMsg);
		}
		for (const FActionSpec& A : M.Actions)
		{
			R.DiscoveredActions.Add(FString::Printf(TEXT("%s (%s)"), *A.Name, *ActionValueTypeToString(A.ValueType)));
		}
		R.Markers = M.Markers;

		UE_LOG(LogUE_PIE_Automation, Log, TEXT("[PIE-REC] Recorded %d frames (%.2fs) → %s"),
			M.TotalFrames, M.DurationSeconds, *CurrentDir);

		if (bEndFrameBound && OnEndFrameHandle.IsValid())
		{
			FCoreDelegates::OnEndFrame.Remove(OnEndFrameHandle);
			OnEndFrameHandle.Reset();
			bEndFrameBound = false;
		}
		State = ERecorderState::Idle;
		CurrentId.Reset();
		CurrentDir.Reset();
		Rows.Reset();
		ActorRows.Reset();
		TrackedActorCache.Reset();
		Markers.Reset();
		RecordedSequence = FSequence();
		bRawInputStarted = false;
		InputError.Reset();
		return R;
	}

	FRecorderFinishResult FPIEInputRecorder::ForceStop()
	{
		return FinaliseCurrent();
	}

	FRecorderStatus FPIEInputRecorder::GetStatus() const
	{
		FRecorderStatus S;
		S.State = State;
		S.Id = CurrentId;
		S.RecordingDir = CurrentDir;
		S.CurrentFrame = Rows.Num();
		S.ElapsedSeconds = (Rows.Num() >= 2) ? (Rows.Last().Time - Rows[0].Time) : 0.0;
		S.TrackedActionCount = Sampler.GetActions().Num();
		S.RawInputEventCount = FPIEInputRouter::Get().GetRecordedEventCount();
		S.InputError = InputError;
		return S;
	}

	bool FPIEInputRecorder::Mark(const FString& Label, FRecorderStatus& OutStatus)
	{
		OutStatus = GetStatus();
		if (State != ERecorderState::Recording) return false;
		Sampler.QueueMarker(Label);
		return true;
	}


	FPIEInputRouter& FPIEInputRouter::Get()
	{
		static TSharedPtr<FPIEInputRouter> Instance = MakeShared<FPIEInputRouter>();
		return *Instance;
	}

	bool FPIEInputRouter::DispatchFrameEvents(const TArray<FPIEInputEvent>& Events, int32& Cursor, int32 InputFrame, const FString& Map,
		TFunctionRef<bool(const FPIEInputEvent&, FString&)> DispatchEvent, FString& OutError)
	{
		while (Events.IsValidIndex(Cursor) && Events[Cursor].InputFrame == InputFrame && Events[Cursor].Map == Map)
		{
			if (!DispatchEvent(Events[Cursor], OutError)) return false;
			++Cursor;
		}
		return true;
	}

	TArray<FPIEInputEvent> FPIEInputRouter::BuildHeldInputReleaseEvents(
		const TMap<FKey, FPIEInputEvent>& KeysDown,
		const TMap<FKey, FPIEInputEvent>& ButtonsDown)
	{
		TArray<FPIEInputEvent> Releases;
		Releases.Reserve(KeysDown.Num() + ButtonsDown.Num());
		for (const TPair<FKey, FPIEInputEvent>& Pair : KeysDown)
		{
			FPIEInputEvent Release = Pair.Value;
			Release.Type = EPIEInputEventType::KeyUp;
			Release.bIsRepeat = false;
			Releases.Add(MoveTemp(Release));
		}
		for (const TPair<FKey, FPIEInputEvent>& Pair : ButtonsDown)
		{
			FPIEInputEvent Release = Pair.Value;
			Release.Type = EPIEInputEventType::MouseButtonUp;
			Release.PressedButtons.Reset();
			for (const TPair<FKey, FPIEInputEvent>& Other : ButtonsDown)
			{
				if (Other.Key != Pair.Key) Release.PressedButtons.Add(Other.Key.GetFName().ToString());
			}
			Releases.Add(MoveTemp(Release));
		}
		return Releases;
	}

	bool FPIEInputRouter::RegisterInputProcessor()
	{
		if (RegisteredHandle.IsValid()) return true;
		if (!FSlateApplication::IsInitialized()) return false;
		RegisteredHandle = AsShared();
		FSlateApplication::Get().RegisterInputPreProcessor(RegisteredHandle, EInputPreProcessorType::PreGame);
		return true;
	}

	void FPIEInputRouter::UnregisterInputProcessor()
	{
		if (RegisteredHandle.IsValid() && FSlateApplication::IsInitialized())
		{
			FSlateApplication::Get().UnregisterInputPreProcessor(RegisteredHandle);
		}
		RegisteredHandle.Reset();
	}

	void FPIEInputRouter::Shutdown()
	{
		FSequence DiscardedSequence;
		EndRecording(DiscardedSequence);
		EndReplay();
		UnregisterInputProcessor();
	}

	bool FPIEInputRouter::FindViewport(TSharedPtr<SViewport>& OutWidget, TSharedPtr<SWindow>& OutWindow, FGeometry& OutGeometry) const
	{
		if (!GEditor || !FSlateApplication::IsInitialized()) return false;
		FViewport* PIEViewport = GEditor->GetPIEViewport();
		FSceneViewport* SceneViewport = PIEViewport ? PIEViewport->AsSceneViewport() : nullptr;
		if (!SceneViewport) return false;
		OutWidget = SceneViewport->GetViewportWidget().Pin();
		if (!OutWidget.IsValid()) return false;
		OutWindow = FSlateApplication::Get().FindWidgetWindow(OutWidget.ToSharedRef());
		if (!OutWindow.IsValid()) return false;
		OutGeometry = OutWidget->GetCachedGeometry();
		const FVector2D Size = OutGeometry.GetLocalSize();
		return Size.X > 0.0 && Size.Y > 0.0;
	}

	bool FPIEInputRouter::BeginRecording(UWorld* World, int32 InClientIndex, FString& OutError)
	{
		if (Mode != EMode::Idle)
		{
			OutError = TEXT("Raw PIE input is already being recorded or replayed");
			return false;
		}
		TSharedPtr<SViewport> Widget;
		TSharedPtr<SWindow> Window;
		FGeometry Geometry;
		if (!World || !FindViewport(Widget, Window, Geometry))
		{
			OutError = TEXT("Could not bind raw input recording to the active PIE viewport");
			return false;
		}
		if (!RegisterInputProcessor())
		{
			OutError = TEXT("Slate input processor is unavailable for PIE recording");
			return false;
		}
		Mode = EMode::Recording;
		ViewportWidget = Widget;
		TargetWindow = Window;
		InputWorld.Reset();
		ClientIndex = InClientIndex;
		SessionTime = 0.0;
		FrameTimes.Reset();
		WorldSegments.Reset();
		WorldTickHandle = FWorldDelegates::OnWorldTickStart.AddRaw(this, &FPIEInputRouter::OnWorldTickStart);
		RecordedViewportSize = Geometry.GetLocalSize();
		RecordedEvents.Reset();
		RecordingKeysDown.Reset();
		NextEventOrder = 0;
		PhysicalButtonsSuppressed.Reset();
		return true;
	}

	void FPIEInputRouter::EndRecording(FSequence& OutSequence)
	{
		if (Mode == EMode::Recording)
		{
			// Preserve an edge received after the final world tick.
			if (!RecordedEvents.IsEmpty() && RecordedEvents.Last().InputFrame == FrameTimes.Num())
			{
				FrameTimes.Add(SessionTime);
			}
			OutSequence.InputEvents = MoveTemp(RecordedEvents);
			OutSequence.ViewportSize = RecordedViewportSize;
			OutSequence.InputFrameTimes = MoveTemp(FrameTimes);
			OutSequence.WorldSegments = MoveTemp(WorldSegments);
			InputWorld.Reset();
			UnbindWorldTick();
			Mode = EMode::Idle;
			PhysicalButtonsSuppressed.Reset();
			RecordingKeysDown.Reset();
			UnregisterInputProcessor();
		}
	}

	bool FPIEInputRouter::BeginReplay(const FSequence& Sequence, int32 InClientIndex, double SettleSeconds, bool bInMonitor, FString& OutError)
	{
		if (Sequence.InputFrameTimes.IsEmpty() || Sequence.WorldSegments.IsEmpty())
		{
			OutError = LastError = TEXT("Recording is missing input frames/world segments; create a new recording");
			return false;
		}
		if (Mode == EMode::Recording)
		{
			OutError = LastError = TEXT("Raw PIE input recording is active");
			return false;
		}
		if (Mode == EMode::Replaying) EndReplay();
		ExecutedEventCount = 0;
		TSharedPtr<SViewport> Widget;
		TSharedPtr<SWindow> Window;
		FGeometry Geometry;
		if (!FindViewport(Widget, Window, Geometry))
		{
			OutError = LastError = TEXT("Could not bind raw input replay to the active PIE viewport");
			return false;
		}
		if (Sequence.ViewportSize.X <= 0.0 || Sequence.ViewportSize.Y <= 0.0)
		{
			OutError = LastError = TEXT("Recording is missing raw input viewport data; create a new recording");
			return false;
		}
		if (!RegisterInputProcessor())
		{
			OutError = LastError = TEXT("Slate input processor is unavailable for PIE replay");
			return false;
		}
		ViewportWidget = Widget;
		TargetWindow = Window;
		RecordedViewportSize = Sequence.ViewportSize;
		ReplayViewportSize = Geometry.GetLocalSize();
		ReplayEvents = Sequence.InputEvents;
		FrameTimes = Sequence.InputFrameTimes;
		WorldSegments = Sequence.WorldSegments;
		ClientIndex = InClientIndex;
		SettleRemaining = FMath::Max(0.0, SettleSeconds);
		bMonitor = bInMonitor;
		SessionTime = 0.0;
		ReplayFrame = 0;
		ReplaySegment = 0;
		WaitStartedAt = FPlatformTime::Seconds();
		InputWorld.Reset();
		WorldTickHandle = FWorldDelegates::OnWorldTickStart.AddRaw(this, &FPIEInputRouter::OnWorldTickStart);
		NextReplayEvent = 0;
		ExecutedEventCount = 0;
		bReplayCancelRequested = false;
		PhysicalKeysSuppressed.Reset();
		PhysicalButtonsSuppressed.Reset();
		InjectedKeysDown.Reset();
		InjectedButtonsDown.Reset();
		LastError.Reset();
		Mode = EMode::Replaying;

		FSlateApplication& SlateApp = FSlateApplication::Get();
		SlateApp.SetUserFocus(0, Widget, EFocusCause::SetDirectly);
		SlateApp.SetKeyboardFocus(Widget, EFocusCause::SetDirectly);
		if (!ReplayViewportSize.Equals(RecordedViewportSize, 1.0))
		{
			UE_LOG(LogUE_PIE_Automation, Warning,
				TEXT("[PIE-INPUT] Replay viewport size differs (recorded %.1fx%.1f, current %.1fx%.1f); scaling pointer positions"),
				RecordedViewportSize.X, RecordedViewportSize.Y, ReplayViewportSize.X, ReplayViewportSize.Y);
		}
		return true;
	}

	int32 FPIEInputRouter::EndReplay()
	{
		const int32 Count = ExecutedEventCount;
		if (Mode != EMode::Replaying) return Count;
		UnbindWorldTick();
		FString ReleaseError;
		ReleaseHeldInputs(ReleaseError);
		if (!ReleaseError.IsEmpty())
		{
			if (!LastError.IsEmpty()) LastError += TEXT("; ");
			LastError += ReleaseError;
		}
		Mode = EMode::Idle;
		ReplayEvents.Reset();
		NextReplayEvent = 0;
		bReplayCancelRequested = false;
		PhysicalKeysSuppressed.Reset();
		PhysicalButtonsSuppressed.Reset();
		if (!bEscapeKeyUpPending) UnregisterInputProcessor();
		return Count;
	}

	bool FPIEInputRouter::ConsumeReplayCancelRequested()
	{
		const bool bRequested = bReplayCancelRequested;
		bReplayCancelRequested = false;
		return bRequested;
	}

	int32 FPIEInputRouter::GetRecordedEventCount() const
	{
		return Mode == EMode::Recording ? RecordedEvents.Num() : 0;
	}

	void FPIEInputRouter::UnbindWorldTick()
	{
		FWorldDelegates::OnWorldTickStart.Remove(WorldTickHandle);
		WorldTickHandle.Reset();
	}

	bool FPIEInputRouter::RefreshViewport()
	{
		TSharedPtr<SViewport> Widget;
		TSharedPtr<SWindow> Window;
		FGeometry Geometry;
		if (!FindViewport(Widget, Window, Geometry)) return false;
		ViewportWidget = Widget;
		TargetWindow = Window;
		return true;
	}

	void FPIEInputRouter::OnWorldTickStart(UWorld* World, ELevelTick TickType, float DeltaSeconds)
	{
		if (!GEditor || World != GEditor->PlayWorld || TickType != LEVELTICK_All || World->IsPaused()) return;
		APlayerController* PC = UGameplayStatics::GetPlayerController(World, ClientIndex);
		const FString Map = UWorld::RemovePIEPrefix(World->GetOutermost()->GetName());
		const bool bReady = PC && PC->GetLocalPlayer() && RefreshViewport() && World->HasBegunPlay();
		if (Mode == EMode::Recording)
		{
			if (!bReady) return;
			if (InputWorld.Get() != World)
			{
				WorldSegments.Add({FrameTimes.Num(), Map});
				InputWorld = World;
				UE_LOG(LogUE_PIE_Automation, Log, TEXT("[PIE-INPUT] Recording segment: %s frame=%d"), *Map, FrameTimes.Num());
			}
			SessionTime += FMath::Max(0.0f, DeltaSeconds);
			FrameTimes.Add(SessionTime);
			return;
		}
		if (Mode != EMode::Replaying || IsReplayComplete() || !LastError.IsEmpty()) return;
		while (WorldSegments.IsValidIndex(ReplaySegment + 1)
			&& WorldSegments[ReplaySegment + 1].InputFrame <= ReplayFrame) ++ReplaySegment;
		const FString& ExpectedMap = WorldSegments[ReplaySegment].Map;
		auto DispatchEvent = [this](const FPIEInputEvent& Event, FString& Error)
		{
			if (!Dispatch(Event, Error)) return false;
			++ExecutedEventCount;
			return true;
		};
		// Slate handles the UI release before travel; its next input frame may belong to the new world.
		// Deliver these old-world events before waiting at the world boundary.
		if (bReady && Map != ExpectedMap && !bMonitor)
		{
			if (!DispatchFrameEvents(ReplayEvents, NextReplayEvent, ReplayFrame, Map, DispatchEvent, LastError))
			{
				FString ReleaseError;
				ReleaseHeldInputs(ReleaseError);
				return;
			}
		}
		if (!bReady || Map != ExpectedMap)
		{
			if (WaitStartedAt == 0.0) WaitStartedAt = FPlatformTime::Seconds();
			if (FPlatformTime::Seconds() - WaitStartedAt > 30.0)
			{
				LastError = FString::Printf(TEXT("Timed out waiting for PIE map/player/viewport: expected %s, current %s"), *ExpectedMap, *Map);
				FString ReleaseError;
				ReleaseHeldInputs(ReleaseError);
			}
			return;
		}
		WaitStartedAt = 0.0;
		if (SettleRemaining > 0.0)
		{
			SettleRemaining = FMath::Max(0.0, SettleRemaining - DeltaSeconds);
			return;
		}
		if (InputWorld.Get() != World)
		{
			InputWorld = World;
			FSlateApplication& SlateApp = FSlateApplication::Get();
			const TSharedPtr<SViewport> Widget = ViewportWidget.Pin();
			// Preserve focus on game UI; restore viewport focus only when outside it.
			if (SlateApp.GetUserFocusedWidget(0) != Widget && !SlateApp.HasFocusedDescendants(Widget.ToSharedRef()))
			{
				SlateApp.SetUserFocus(0, Widget, EFocusCause::SetDirectly);
			}
			if (!bMonitor)
			{
				// Travel flushes PlayerInput. Reapply only the keys held by this replay.
				TArray<FPIEInputEvent> Held;
				for (const auto& Entry : InjectedKeysDown) Held.Add(Entry.Value);
				for (const auto& Entry : InjectedButtonsDown) Held.Add(Entry.Value);
				for (FPIEInputEvent& Event : Held)
				{
					Event.bIsRepeat = false;
					if (!Dispatch(Event, LastError)) return;
				}
			}
			UE_LOG(LogUE_PIE_Automation, Log, TEXT("[PIE-INPUT] Replay segment: %s frame=%d"), *Map, ReplayFrame);
		}
		if (!bMonitor)
		{
			if (!DispatchFrameEvents(ReplayEvents, NextReplayEvent, ReplayFrame, Map, DispatchEvent, LastError))
			{
				FString ReleaseError;
				ReleaseHeldInputs(ReleaseError);
				return;
			}
			if (ReplayEvents.IsValidIndex(NextReplayEvent) && ReplayEvents[NextReplayEvent].InputFrame == ReplayFrame)
			{
				LastError = TEXT("Replay reached a new world before its recorded input events completed");
				return;
			}
			// Slate accumulates mouse axes; flush before PlayerController processes input.
			if (FViewport* Viewport = GEditor->GetPIEViewport())
			{
				if (FSceneViewport* SceneViewport = Viewport->AsSceneViewport()) SceneViewport->OnFinishedPointerInput();
			}
		}
		SessionTime = FrameTimes[ReplayFrame++];
	}

	void FPIEInputRouter::Tick(float, FSlateApplication&, TSharedRef<ICursor>)
	{
		if (Mode != EMode::Idle) RefreshViewport();
		if (Mode == EMode::Replaying && !IsReplayComplete() && LastError.IsEmpty())
		{
			if (!GEditor || !GEditor->PlayWorld || (InputWorld.IsValid() && InputWorld.Get() != GEditor->PlayWorld))
			{
				if (WaitStartedAt == 0.0) WaitStartedAt = FPlatformTime::Seconds();
			}
			if (WaitStartedAt > 0.0 && FPlatformTime::Seconds() - WaitStartedAt > 30.0)
			{
				LastError = TEXT("Timed out waiting for PIE world/player/viewport after travel");
				FString ReleaseError;
				ReleaseHeldInputs(ReleaseError);
			}
		}
	}

	bool FPIEInputRouter::IsTargetWindowActive(FSlateApplication& SlateApp) const
	{
		const TSharedPtr<SWindow> Target = TargetWindow.Pin();
		const TSharedPtr<SWindow> Active = SlateApp.GetActiveTopLevelWindow();
		return Target.IsValid() && Active == Target;
	}

	bool FPIEInputRouter::IsKeyboardTargetActive(FSlateApplication& SlateApp) const
	{
		const TSharedPtr<SViewport> Widget = ViewportWidget.Pin();
		return Widget.IsValid() && IsTargetWindowActive(SlateApp)
			&& (SlateApp.GetUserFocusedWidget(0) == Widget || SlateApp.HasFocusedDescendants(Widget.ToSharedRef()));
	}

	bool FPIEInputRouter::IsInsideViewport(const FVector2D& ScreenPosition, FVector2D& OutLocalPosition) const
	{
		const TSharedPtr<SViewport> Widget = ViewportWidget.Pin();
		if (!Widget.IsValid()) return false;
		const FGeometry Geometry = Widget->GetCachedGeometry();
		OutLocalPosition = Geometry.AbsoluteToLocal(ScreenPosition);
		const FVector2D Size = Geometry.GetLocalSize();
		return OutLocalPosition.X >= 0.0 && OutLocalPosition.Y >= 0.0
			&& OutLocalPosition.X < Size.X && OutLocalPosition.Y < Size.Y;
	}

	FPIEInputEvent FPIEInputRouter::MakeKeyEvent(EPIEInputEventType Type, const FKeyEvent& Event) const
	{
		FPIEInputEvent Result;
		Result.Type = Type;
		Result.Key = Event.GetKey().GetFName().ToString();
		Result.UserIndex = Event.GetUserIndex();
		Result.KeyCode = Event.GetKeyCode();
		Result.bIsRepeat = Event.IsRepeat();
		Result.bShift = Event.IsShiftDown();
		Result.bControl = Event.IsControlDown();
		Result.bAlt = Event.IsAltDown();
		Result.bCommand = Event.IsCommandDown();
		Result.bCapsLocked = Event.GetModifierKeys().AreCapsLocked();
		return Result;
	}

	FPIEInputEvent FPIEInputRouter::MakePointerEvent(EPIEInputEventType Type, const FPointerEvent& Event) const
	{
		FPIEInputEvent Result;
		Result.Type = Type;
		Result.UserIndex = Event.GetUserIndex();
		Result.PointerIndex = Event.GetPointerIndex();
		Result.Key = Event.GetEffectingButton().IsValid() ? Event.GetEffectingButton().GetFName().ToString() : FString();
		const FVector2D ScreenPosition(Event.GetScreenSpacePosition());
		const FVector2D ScreenLastPosition = ScreenPosition - FVector2D(Event.GetCursorDelta());
		const TSharedPtr<SViewport> Widget = ViewportWidget.Pin();
		if (Widget.IsValid())
		{
			const FGeometry Geometry = Widget->GetCachedGeometry();
			Result.Position = Geometry.AbsoluteToLocal(ScreenPosition);
			Result.Delta = Result.Position - Geometry.AbsoluteToLocal(ScreenLastPosition);
			UWorld* World = GEditor ? GEditor->PlayWorld : nullptr;
			APlayerController* PC = World ? UGameplayStatics::GetPlayerController(World, ClientIndex) : nullptr;
			Result.bRelativeMouse = Widget->HasMouseCapture() && PC && !PC->ShouldShowMouseCursor();
			if (Result.bRelativeMouse) Result.Delta = FVector2D(Event.GetCursorDelta());
		}
		for (const FKey& Button : Event.GetPressedButtons())
		{
			Result.PressedButtons.Add(Button.GetFName().ToString());
		}
		Result.WheelDelta = Event.GetWheelDelta();
		Result.bShift = Event.IsShiftDown();
		Result.bControl = Event.IsControlDown();
		Result.bAlt = Event.IsAltDown();
		Result.bCommand = Event.IsCommandDown();
		Result.bCapsLocked = Event.GetModifierKeys().AreCapsLocked();
		return Result;
	}

	void FPIEInputRouter::Record(FPIEInputEvent&& Event)
	{
		UWorld* World = GEditor ? GEditor->PlayWorld : nullptr;
		if (!World) return;
		Event.Map = UWorld::RemovePIEPrefix(World->GetOutermost()->GetName());
		Event.TimeSeconds = SessionTime;
		Event.InputFrame = FrameTimes.Num();
		Event.Order = NextEventOrder++;
		RecordedEvents.Add(MoveTemp(Event));
	}

	bool FPIEInputRouter::HandleKeyDownEvent(FSlateApplication& SlateApp, const FKeyEvent& Event)
	{
		if (bDispatchingSynthetic) return false;
		if (Mode == EMode::Recording)
		{
			if (IsKeyboardTargetActive(SlateApp))
			{
				Record(MakeKeyEvent(EPIEInputEventType::KeyDown, Event));
				RecordingKeysDown.Add(Event.GetKey());
			}
			return false;
		}
		if (Mode == EMode::Replaying && !bMonitor)
		{
			const FKey Key = Event.GetKey();
			if (Key == EKeys::Escape && IsTargetWindowActive(SlateApp))
			{
				PhysicalKeysSuppressed.Add(Key);
				bEscapeKeyUpPending = true;
				bReplayCancelRequested = true;
				return true;
			}
			if (PhysicalKeysSuppressed.Contains(Key) || IsKeyboardTargetActive(SlateApp))
			{
				PhysicalKeysSuppressed.Add(Key);
				return true;
			}
		}
		return false;
	}

	bool FPIEInputRouter::HandleKeyUpEvent(FSlateApplication& SlateApp, const FKeyEvent& Event)
	{
		if (bDispatchingSynthetic) return false;
		if (bEscapeKeyUpPending && Event.GetKey() == EKeys::Escape)
		{
			bEscapeKeyUpPending = false;
			PhysicalKeysSuppressed.Remove(EKeys::Escape);
			if (Mode == EMode::Idle) UnregisterInputProcessor();
			return true;
		}
		if (Mode == EMode::Recording)
		{
			FVector2D Local;
			const FKey Key = Event.GetKey();
			const bool bTracked = RecordingKeysDown.Contains(Key);
			const bool bInside = IsTargetWindowActive(SlateApp) && IsInsideViewport(SlateApp.GetCursorPos(), Local);
			if (bTracked || bInside)
			{
				Record(MakeKeyEvent(EPIEInputEventType::KeyUp, Event));
				RecordingKeysDown.Remove(Key);
			}
			return false;
		}
		if (Mode == EMode::Replaying && !bMonitor && PhysicalKeysSuppressed.Remove(Event.GetKey()) > 0) return true;
		return false;
	}

	bool FPIEInputRouter::HandleMouseMoveEvent(FSlateApplication& SlateApp, const FPointerEvent& Event)
	{
		if (bDispatchingSynthetic) return false;
		FVector2D Local;
		const bool bInside = IsTargetWindowActive(SlateApp) && IsInsideViewport(FVector2D(Event.GetScreenSpacePosition()), Local);
		if (Mode == EMode::Recording)
		{
			if (bInside || PhysicalButtonsSuppressed.Num() > 0) Record(MakePointerEvent(EPIEInputEventType::MouseMove, Event));
			return false;
		}
		if (Mode == EMode::Replaying && !bMonitor && (bInside || PhysicalButtonsSuppressed.Num() > 0)) return true;
		return false;
	}

	bool FPIEInputRouter::HandleMouseButtonDownEvent(FSlateApplication& SlateApp, const FPointerEvent& Event)
	{
		if (bDispatchingSynthetic) return false;
		FVector2D Local;
		const FKey Button = Event.GetEffectingButton();
		const bool bInside = IsTargetWindowActive(SlateApp) && IsInsideViewport(FVector2D(Event.GetScreenSpacePosition()), Local);
		if (Mode == EMode::Recording)
		{
			if (bInside)
			{
				PhysicalButtonsSuppressed.Add(Button);
				Record(MakePointerEvent(EPIEInputEventType::MouseButtonDown, Event));
			}
			return false;
		}
		if (Mode == EMode::Replaying && !bMonitor && (bInside || PhysicalButtonsSuppressed.Contains(Button)))
		{
			PhysicalButtonsSuppressed.Add(Button);
			return true;
		}
		return false;
	}

	bool FPIEInputRouter::HandleMouseButtonUpEvent(FSlateApplication& SlateApp, const FPointerEvent& Event)
	{
		if (bDispatchingSynthetic) return false;
		const FKey Button = Event.GetEffectingButton();
		if (Mode == EMode::Recording)
		{
			FVector2D Local;
			const bool bTracked = PhysicalButtonsSuppressed.Contains(Button);
			const bool bInside = IsTargetWindowActive(SlateApp) && IsInsideViewport(FVector2D(Event.GetScreenSpacePosition()), Local);
			if (bTracked || bInside)
			{
				Record(MakePointerEvent(EPIEInputEventType::MouseButtonUp, Event));
				PhysicalButtonsSuppressed.Remove(Button);
			}
			return false;
		}
		if (Mode == EMode::Replaying && !bMonitor && PhysicalButtonsSuppressed.Remove(Button) > 0) return true;
		return false;
	}

	bool FPIEInputRouter::HandleMouseButtonDoubleClickEvent(FSlateApplication& SlateApp, const FPointerEvent& Event)
	{
		if (bDispatchingSynthetic) return false;
		FVector2D Local;
		const bool bInside = IsTargetWindowActive(SlateApp) && IsInsideViewport(FVector2D(Event.GetScreenSpacePosition()), Local);
		if (Mode == EMode::Recording && bInside)
		{
			PhysicalButtonsSuppressed.Add(Event.GetEffectingButton());
			Record(MakePointerEvent(EPIEInputEventType::MouseDoubleClick, Event));
			return false;
		}
		if (Mode == EMode::Replaying && !bMonitor && bInside)
		{
			PhysicalButtonsSuppressed.Add(Event.GetEffectingButton());
			return true;
		}
		return false;
	}

	bool FPIEInputRouter::HandleMouseWheelOrGestureEvent(FSlateApplication& SlateApp, const FPointerEvent& Event, const FPointerEvent* GestureEvent)
	{
		if (bDispatchingSynthetic || GestureEvent) return false;
		FVector2D Local;
		const bool bInside = IsTargetWindowActive(SlateApp) && IsInsideViewport(FVector2D(Event.GetScreenSpacePosition()), Local);
		if (Mode == EMode::Recording && bInside)
		{
			Record(MakePointerEvent(EPIEInputEventType::MouseWheel, Event));
			return false;
		}
		if (Mode == EMode::Replaying && !bMonitor && bInside) return true;
		return false;
	}

	FModifierKeysState FPIEInputRouter::MakeModifiers(const FPIEInputEvent& Event) const
	{
		return FModifierKeysState(Event.bShift, false, Event.bControl, false, Event.bAlt, false,
			Event.bCommand, false, Event.bCapsLocked);
	}

	TSet<FKey> FPIEInputRouter::MakePressedButtons(const FPIEInputEvent& Event) const
	{
		TSet<FKey> Buttons;
		for (const FString& Name : Event.PressedButtons) Buttons.Add(FKey(FName(*Name)));
		return Buttons;
	}

	FVector2D FPIEInputRouter::ToScreenPosition(const FVector2D& LocalPosition) const
	{
		const TSharedPtr<SViewport> Widget = ViewportWidget.Pin();
		if (!Widget.IsValid()) return FVector2D::ZeroVector;
		const FGeometry Geometry = Widget->GetCachedGeometry();
		const FVector2D Size = Geometry.GetLocalSize();
		const FVector2D ScaledLocal(
			RecordedViewportSize.X > 0.0 ? LocalPosition.X * Size.X / RecordedViewportSize.X : LocalPosition.X,
			RecordedViewportSize.Y > 0.0 ? LocalPosition.Y * Size.Y / RecordedViewportSize.Y : LocalPosition.Y);
		return Geometry.LocalToAbsolute(ScaledLocal);
	}

	FVector2D FPIEInputRouter::ToScreenDelta(const FVector2D& LocalPosition, const FVector2D& LocalDelta) const
	{
		return ToScreenPosition(LocalPosition) - ToScreenPosition(LocalPosition - LocalDelta);
	}

	bool FPIEInputRouter::Dispatch(const FPIEInputEvent& Event, FString& OutError)
	{
		FSlateApplication& SlateApp = FSlateApplication::Get();
		if (!TargetWindow.Pin().IsValid() || !ViewportWidget.Pin().IsValid())
		{
			OutError = TEXT("PIE input target window was closed during replay");
			return false;
		}
		TGuardValue<bool> SyntheticGuard(bDispatchingSynthetic, true);
		if (Event.Type == EPIEInputEventType::KeyDown || Event.Type == EPIEInputEventType::KeyUp)
		{
			const FKey Key(FName(*Event.Key));
			const FKeyEvent KeyEvent(Key, MakeModifiers(Event), Event.UserIndex, Event.bIsRepeat, 0, Event.KeyCode);
			if (Event.Type == EPIEInputEventType::KeyDown)
			{
				SlateApp.ProcessKeyDownEvent(KeyEvent);
				InjectedKeysDown.Add(Key, Event);
			}
			else
			{
				SlateApp.ProcessKeyUpEvent(KeyEvent);
				InjectedKeysDown.Remove(Key);
			}
			return true;
		}

		TSharedPtr<SWindow> Window = TargetWindow.Pin();
		const TSharedPtr<FGenericWindow> NativeWindow = Window.IsValid() ? Window->GetNativeWindow() : nullptr;
		if (!Window.IsValid() || !NativeWindow.IsValid())
		{
			OutError = TEXT("PIE input target window was closed during replay");
			return false;
		}
		const FVector2D ScreenPosition = Event.bRelativeMouse ? SlateApp.GetCursorPos() : ToScreenPosition(Event.Position);
		const FVector2D ScreenDelta = Event.bRelativeMouse ? Event.Delta : ToScreenDelta(Event.Position, Event.Delta);
		const FVector2D LastScreenPosition = ScreenPosition - ScreenDelta;
		// Slate button release checks hover state; keep the platform cursor aligned with replay coordinates.
		if (!Event.bRelativeMouse) SlateApp.SetCursorPos(ScreenPosition);
		const TSet<FKey> Buttons = MakePressedButtons(Event);
		const FKey EffectingButton = Event.Key.IsEmpty() ? FKey() : FKey(FName(*Event.Key));
		const FPointerEvent PointerEvent(Event.UserIndex, Event.PointerIndex, ScreenPosition, LastScreenPosition,
			Buttons, EffectingButton, Event.WheelDelta, MakeModifiers(Event));
		const TSharedPtr<SViewport> Widget = ViewportWidget.Pin();
		if (Event.bRelativeMouse && !Widget->HasMouseCapture())
		{
			FWidgetPath Path;
			if (!SlateApp.GeneratePathToWidgetUnchecked(Widget.ToSharedRef(), Path))
			{
				OutError = TEXT("Could not restore PIE mouse capture");
				return false;
			}
			SlateApp.ProcessReply(Path, FReply::Handled().UseHighPrecisionMouseMovement(Widget.ToSharedRef())
				.SetUserFocus(Widget.ToSharedRef()), nullptr, &PointerEvent, Event.UserIndex);
		}

		switch (Event.Type)
		{
		case EPIEInputEventType::MouseMove:
			SlateApp.ProcessMouseMoveEvent(PointerEvent);
			break;
		case EPIEInputEventType::MouseButtonDown:
			SlateApp.ProcessMouseButtonDownEvent(NativeWindow, PointerEvent);
			InjectedButtonsDown.Add(EffectingButton, Event);
			break;
		case EPIEInputEventType::MouseButtonUp:
			SlateApp.ProcessMouseButtonUpEvent(PointerEvent);
			InjectedButtonsDown.Remove(EffectingButton);
			break;
		case EPIEInputEventType::MouseDoubleClick:
			SlateApp.ProcessMouseButtonDoubleClickEvent(NativeWindow, PointerEvent);
			InjectedButtonsDown.Add(EffectingButton, Event);
			break;
		case EPIEInputEventType::MouseWheel:
			SlateApp.ProcessMouseWheelOrGestureEvent(PointerEvent, nullptr);
			break;
		default:
			OutError = TEXT("Unsupported raw PIE pointer event");
			return false;
		}
		return true;
	}

	void FPIEInputRouter::ReleaseHeldInputs(FString& OutError)
	{
		if (!FSlateApplication::IsInitialized())
		{
			if (InjectedKeysDown.Num() > 0 || InjectedButtonsDown.Num() > 0)
			{
				OutError = TEXT("Slate shut down before held PIE inputs could be released");
			}
			InjectedKeysDown.Reset();
			InjectedButtonsDown.Reset();
			return;
		}
		if (!TargetWindow.Pin().IsValid() || !ViewportWidget.Pin().IsValid())
		{
			OutError = TEXT("PIE input target window closed before held inputs could be released");
			InjectedKeysDown.Reset();
			InjectedButtonsDown.Reset();
			return;
		}
		TGuardValue<bool> SyntheticGuard(bDispatchingSynthetic, true);
		const TArray<FPIEInputEvent> Releases = BuildHeldInputReleaseEvents(InjectedKeysDown, InjectedButtonsDown);
		for (const FPIEInputEvent& Release : Releases)
		{
			if (!Dispatch(Release, OutError)) break;
		}
		InjectedKeysDown.Reset();
		InjectedButtonsDown.Reset();
	}

}
