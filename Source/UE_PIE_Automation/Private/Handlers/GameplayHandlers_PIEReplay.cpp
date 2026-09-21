// PIE replay handlers: pie_replay_arm / _disarm / _stop / _status.
// Members of FGameplayHandlers.

#include "GameplayHandlers.h"
#include "HandlerUtils.h"
#include "PIE/PIEInputReplayer.h"
#include "PIE/PIESequenceFormat.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Modules/ModuleManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"

namespace
{
	using namespace UE_PIE_Automation;

	FString ResolveRecordingFolder(const TSharedPtr<FJsonObject>& Params)
	{
		const FString Id = OptionalString(Params, TEXT("recording_id"));
		const FString RecDir = OptionalString(Params, TEXT("recording_dir"));
		FString Folder;
		if (!RecDir.IsEmpty() && Id.IsEmpty())
		{
			Folder = RecDir;
		}
		else
		{
			const FString Root = RecDir.IsEmpty()
				? (FPaths::ProjectSavedDir() / TEXT("MCPRecordings"))
				: RecDir;
			Folder = Root / Id;
		}
		while (Folder.EndsWith(TEXT("/")) || Folder.EndsWith(TEXT("\\")))
		{
			Folder.LeftChopInline(1);
		}
		return Folder;
	}

	bool LoadFrameImage(const FString& Path, TArray<FColor>& OutPixels, int32& OutWidth, int32& OutHeight)
	{
		TArray<uint8> Raw;
		if (!FFileHelper::LoadFileToArray(Raw, *Path) || Raw.Num() == 0) return false;

		IImageWrapperModule& IWM = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
		const EImageFormat Format = IWM.DetectImageFormat(Raw.GetData(), Raw.Num());
		if (Format == EImageFormat::Invalid) return false;

		TSharedPtr<IImageWrapper> Wrapper = IWM.CreateImageWrapper(Format);
		if (!Wrapper.IsValid() || !Wrapper->SetCompressed(Raw.GetData(), Raw.Num())) return false;

		TArray64<uint8> Rgba;
		if (!Wrapper->GetRaw(ERGBFormat::BGRA, 8, Rgba)) return false;
		OutWidth = Wrapper->GetWidth();
		OutHeight = Wrapper->GetHeight();
		if (OutWidth <= 0 || OutHeight <= 0) return false;

		const int64 ExpectedBytes = static_cast<int64>(OutWidth) * OutHeight * sizeof(FColor);
		if (Rgba.Num() < ExpectedBytes) return false;
		OutPixels.SetNumUninitialized(OutWidth * OutHeight);
		FMemory::Memcpy(OutPixels.GetData(), Rgba.GetData(), ExpectedBytes);
		return true;
	}

	int64 FrameIndexFromName(const FString& Name)
	{
		const FString Base = FPaths::GetBaseFilename(Name);
		return Base.StartsWith(TEXT("frame_")) ? FCString::Atoi64(*Base.Mid(6)) : -1;
	}

	FString StateToString(EReplayerState S)
	{
		switch (S)
		{
		case EReplayerState::Idle:           return TEXT("idle");
		case EReplayerState::Armed:          return TEXT("armed");
		case EReplayerState::WaitingForPawn: return TEXT("waiting_for_pawn");
		case EReplayerState::Replaying:      return TEXT("replaying");
		case EReplayerState::Completed:      return TEXT("completed");
		}
		return TEXT("idle");
	}

	void WriteStatusFields(TSharedPtr<FJsonObject> R, const FReplayerStatus& S)
	{
		R->SetStringField(TEXT("state"), StateToString(S.State));
		R->SetStringField(TEXT("source_recording_id"), S.SourceRecordingId);
		R->SetNumberField(TEXT("current_step"), S.CurrentStep);
		R->SetNumberField(TEXT("total_steps"), S.TotalSteps);
		R->SetNumberField(TEXT("elapsed_seconds"), S.ElapsedSeconds);
		R->SetNumberField(TEXT("max_position_drift_cm"), S.MaxPositionDriftCm);
		R->SetNumberField(TEXT("max_velocity_drift_cms"), S.MaxVelocityDriftCms);
		R->SetNumberField(TEXT("frames_captured"), S.FramesCaptured);
		// pie_active lets an unattended replay_run caller poll until the PIE
		// session has ended; when it flips false and last_result is present the
		// drift report on disk is finalized and safe to read.
		R->SetBoolField(TEXT("pie_active"), S.bPIEActive);
		if (S.bHasLastResult)
		{
			TSharedPtr<FJsonObject> Last = MakeShared<FJsonObject>();
			Last->SetStringField(TEXT("drift_report_path"), S.LastDriftReportPath);
			Last->SetNumberField(TEXT("max_position_drift_cm"), S.LastMaxPositionDriftCm);
			Last->SetNumberField(TEXT("max_velocity_drift_cms"), S.LastMaxVelocityDriftCms);
			Last->SetNumberField(TEXT("frames_compared"), S.LastFramesCompared);
			// Item 1b: kept frames + labeled contact sheet the agent can view.
			if (!S.LastFrameDir.IsEmpty()) Last->SetStringField(TEXT("frame_dir"), S.LastFrameDir);
			if (S.LastFrameCount > 0) Last->SetNumberField(TEXT("frame_count"), S.LastFrameCount);
			if (!S.LastContactSheetPath.IsEmpty()) Last->SetStringField(TEXT("contact_sheet_path"), S.LastContactSheetPath);
			R->SetObjectField(TEXT("last_result"), Last);
		}
	}

	// Parse the shared replay arm-config from an MCP params object. Returns
	// false and sets OutErr (a ready-to-return MCPError) on a bad inline
	// sequence. Used by both replay_arm and replay_run so the two stay in
	// lockstep on every knob.
	bool ParseReplayArmConfig(const TSharedPtr<FJsonObject>& Params, FReplayerArmConfig& Cfg, TSharedPtr<FJsonValue>& OutErr)
	{
		Cfg.SourceRecordingId = OptionalString(Params, TEXT("recording_id"));
		Cfg.SequencePath = OptionalString(Params, TEXT("sequence_path"));
		Cfg.SourceDir = OptionalString(Params, TEXT("recording_dir"));

		// Convenience: `recording_dir` alone (no id, no explicit sequence) names
		// the recording folder directly — "replay this ./path/to/recording".
		// Derive the id from the leaf so drift.json lands beside the source.
		if (Cfg.SourceRecordingId.IsEmpty() && Cfg.SequencePath.IsEmpty() && !Cfg.SourceDir.IsEmpty())
		{
			FString Leaf = Cfg.SourceDir;
			Leaf.RemoveFromEnd(TEXT("/"));
			Leaf.RemoveFromEnd(TEXT("\\"));
			Cfg.SourceRecordingId = FPaths::GetCleanFilename(Leaf);
		}

		// Inline steps array — parse via the standard sequence reader by wrapping
		// in a minimal envelope.
		const TArray<TSharedPtr<FJsonValue>>* StepsArr = nullptr;
		if (Params->TryGetArrayField(TEXT("steps"), StepsArr) && StepsArr && StepsArr->Num() > 0)
		{
			TSharedRef<FJsonObject> Env = MakeShared<FJsonObject>();
			Env->SetNumberField(TEXT("version"), kFormatVersion);
			Env->SetNumberField(TEXT("settle_ms"), OptionalInt(Params, TEXT("settle_ms"), 500));
			Env->SetNumberField(TEXT("sample_hz"), OptionalInt(Params, TEXT("sample_hz"), 60));
			Env->SetNumberField(TEXT("rng_seed"), OptionalNumber(Params, TEXT("rng_seed"), 0.0));
			Env->SetArrayField(TEXT("steps"), *StepsArr);
			FSequence S;
			FString Err;
			if (!SequenceFromJson(Env, S, Err)) { OutErr = MCPError(Err); return false; }
			Cfg.InlineSequence = S;
			Cfg.bInlineSequenceProvided = true;
		}

		if (Params->HasField(TEXT("pin_fps")))
		{
			int32 Hz = 60;
			Params->TryGetNumberField(TEXT("pin_fps"), Hz);
			Cfg.PinFPS = Hz;
		}
		if (Params->HasField(TEXT("settle_ms")))
		{
			int32 Ms = 500;
			Params->TryGetNumberField(TEXT("settle_ms"), Ms);
			Cfg.SettleMs = Ms;
		}
		Cfg.bApplyRngSeed = OptionalBool(Params, TEXT("apply_rng_seed"), true);
		Cfg.bRecordDrift  = OptionalBool(Params, TEXT("record_drift"), true);
		Cfg.bAutoStopPIE  = OptionalBool(Params, TEXT("auto_stop_pie"), false);
		Cfg.bFixedTimestep = OptionalBool(Params, TEXT("fixed_timestep"), true);
		Cfg.bEject        = OptionalBool(Params, TEXT("eject"), false);
		Cfg.TimeScale     = static_cast<float>(OptionalNumber(Params, TEXT("time_scale"), 1.0));
		const FString Mode = OptionalString(Params, TEXT("mode"), TEXT("replay")).ToLower();
		Cfg.bMonitor = (Mode == TEXT("monitor"));

		if (Params->HasField(TEXT("capture_frame_every")))
		{
			double D = 0;
			Params->TryGetNumberField(TEXT("capture_frame_every"), D);
			Cfg.CaptureFrameEvery = FMath::Max(0, static_cast<int32>(D));
		}
		Cfg.CaptureFPS = FMath::Clamp(OptionalInt(Params, TEXT("capture_fps"), 15), 1, 240);
		Cfg.CaptureResolutionPercent = FMath::Clamp(
			OptionalInt(Params, TEXT("capture_resolution_percent"), 50), 1, 100);

		if (Params->HasField(TEXT("client_id")))
		{
			double D = 0;
			Params->TryGetNumberField(TEXT("client_id"), D);
			Cfg.ClientId = FMath::Max(0, static_cast<int32>(D));
		}

		const TSharedPtr<FJsonObject>* Thr = nullptr;
		if (Params->TryGetObjectField(TEXT("drift_thresholds"), Thr) && Thr)
		{
			double D;
			if ((*Thr)->TryGetNumberField(TEXT("position_cm"), D)) Cfg.ThrPosCm = static_cast<float>(D);
			if ((*Thr)->TryGetNumberField(TEXT("rotation_deg"), D)) Cfg.ThrRotDeg = static_cast<float>(D);
			if ((*Thr)->TryGetNumberField(TEXT("velocity_cms"), D)) Cfg.ThrVelCms = static_cast<float>(D);
			if ((*Thr)->TryGetNumberField(TEXT("tracked_default"), D)) Cfg.ThrTrackedDefault = static_cast<float>(D);
			const TSharedPtr<FJsonObject>* Tracked = nullptr;
			if ((*Thr)->TryGetObjectField(TEXT("tracked"), Tracked) && Tracked)
			{
				for (const auto& KV : (*Tracked)->Values)
				{
					double TD = 0;
					if (KV.Value.IsValid() && KV.Value->TryGetNumber(TD))
					{
						Cfg.TrackedThresholds.Add(FString(*KV.Key), static_cast<float>(TD));
					}
				}
			}
		}

		return true;
	}
}

TSharedPtr<FJsonValue> FGameplayHandlers::PieReplayArm(const TSharedPtr<FJsonObject>& Params)
{
	MCP_CHECK_GAME_THREAD();
	FReplayerArmConfig Cfg;
	TSharedPtr<FJsonValue> ParseErr;
	if (!ParseReplayArmConfig(Params, Cfg, ParseErr)) return ParseErr;

	FString Err, Msg;
	if (!FPIEInputReplayer::Get().Arm(Cfg, Err, Msg))
	{
		return MCPError(Err);
	}

	const FReplayerStatus S = FPIEInputReplayer::Get().GetStatus();
	auto Result = MCPSuccess();
	Result->SetBoolField(TEXT("armed"), true);
	Result->SetStringField(TEXT("message"), Msg);
	WriteStatusFields(Result, S);

	TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
	MCPSetRollback(Result, TEXT("pie_replay_disarm"), Payload);
	return MCPResult(Result);
}

// One-shot unattended replay: arm the replayer and immediately start PIE, so
// an agent can drive a recording to completion without a human (or a separate
// editor(play_in_editor) call) touching the editor. Defaults auto_stop_pie to
// true so PIE tears itself down when the run finishes and drift.json lands on
// disk. Returns immediately (PIE runs across frames) — poll pie_replay_status
// until pie_active is false, then read the drift report.
TSharedPtr<FJsonValue> FGameplayHandlers::PieReplayRun(const TSharedPtr<FJsonObject>& Params)
{
	MCP_CHECK_GAME_THREAD();

	if (!GEditor)
	{
		return MCPError(TEXT("pie_replay_run requires the editor (GEditor unavailable)"));
	}
	if (GEditor->PlayWorld != nullptr)
	{
		return MCPError(TEXT("A PIE session is already running; stop it before pie_replay_run"));
	}

	FReplayerArmConfig Cfg;
	TSharedPtr<FJsonValue> ParseErr;
	if (!ParseReplayArmConfig(Params, Cfg, ParseErr)) return ParseErr;
	// Unattended: default to ending PIE ourselves when the run completes. A
	// caller can still pass auto_stop_pie=false to leave PIE up for inspection.
	Cfg.bAutoStopPIE = OptionalBool(Params, TEXT("auto_stop_pie"), true);

	FString Err, Msg;
	if (!FPIEInputReplayer::Get().Arm(Cfg, Err, Msg))
	{
		return MCPError(Err);
	}

	// Kick off PIE. RequestPlaySession queues the session for the next editor
	// tick; the armed replayer attaches from its BeginPIE delegate.
	FRequestPlaySessionParams PlayParams;
	GEditor->RequestPlaySession(PlayParams);

	const FReplayerStatus S = FPIEInputReplayer::Get().GetStatus();
	auto Result = MCPSuccess();
	Result->SetBoolField(TEXT("started"), true);
	Result->SetBoolField(TEXT("auto_stop_pie"), Cfg.bAutoStopPIE);
	Result->SetStringField(TEXT("message"), Msg);
	Result->SetStringField(TEXT("poll"), TEXT("pie_replay_status until pie_active=false, then read drift via record_read(file=drift)"));
	WriteStatusFields(Result, S);
	return MCPResult(Result);
}

TSharedPtr<FJsonValue> FGameplayHandlers::PieReplayDisarm(const TSharedPtr<FJsonObject>& /*Params*/)
{
	MCP_CHECK_GAME_THREAD();
	FString Err;
	const bool OK = FPIEInputReplayer::Get().Disarm(Err);
	if (!OK) return MCPError(Err);
	auto Result = MCPSuccess();
	Result->SetBoolField(TEXT("disarmed"), true);
	return MCPResult(Result);
}

TSharedPtr<FJsonValue> FGameplayHandlers::PieReplayStop(const TSharedPtr<FJsonObject>& /*Params*/)
{
	MCP_CHECK_GAME_THREAD();
	const FReplayerFinishResult F = FPIEInputReplayer::Get().ForceStop();
	if (!F.bSuccess && !F.Error.IsEmpty()) return MCPError(F.Error);
	auto Result = MCPSuccess();
	Result->SetBoolField(TEXT("stopped"), true);
	Result->SetNumberField(TEXT("executed_steps"), F.ExecutedSteps);
	Result->SetNumberField(TEXT("frames_captured"), F.FramesCaptured);
	if (F.FrameCount > 0)
	{
		Result->SetStringField(TEXT("frame_dir"), F.FrameDir);
		Result->SetNumberField(TEXT("frame_count"), F.FrameCount);
		if (!F.ContactSheetPath.IsEmpty())
		{
			Result->SetStringField(TEXT("contact_sheet_path"), F.ContactSheetPath);
		}
	}
	if (!F.CaptureDir.IsEmpty())
	{
		Result->SetStringField(TEXT("capture_dir"), F.CaptureDir);
	}
	if (!F.DriftReportPath.IsEmpty())
	{
		Result->SetStringField(TEXT("drift_report_path"), F.DriftReportPath);
		Result->SetNumberField(TEXT("max_position_drift_cm"), F.Drift.MaxPositionDriftCm);
		Result->SetNumberField(TEXT("max_velocity_drift_cms"), F.Drift.MaxVelocityDriftCms);
		Result->SetNumberField(TEXT("frames_compared"), F.Drift.FramesCompared);
	}
	return MCPResult(Result);
}

TSharedPtr<FJsonValue> FGameplayHandlers::PieReplayStatus(const TSharedPtr<FJsonObject>& /*Params*/)
{
	MCP_CHECK_GAME_THREAD();
	const FReplayerStatus S = FPIEInputReplayer::Get().GetStatus();
	auto Result = MCPSuccess();
	WriteStatusFields(Result, S);
	return MCPResult(Result);
}

// Item 1c: read a finalised drift.json and return the synthesised lead (first
// divergence, top channels, correlated errors) plus the images bracketing the
// divergence frame, so the agent gets a conclusion instead of a CSV.
TSharedPtr<FJsonValue> FGameplayHandlers::PieReplayAnalyze(const TSharedPtr<FJsonObject>& Params)
{
	MCP_CHECK_GAME_THREAD();
	const FString Folder = ResolveRecordingFolder(Params);

	const FString DriftPath = Folder / TEXT("drift.json");
	FDriftReport D;
	FString Err;
	if (!LoadDrift(DriftPath, D, Err))
	{
		return MCPError(FString::Printf(TEXT("Could not read drift report at %s: %s"), *DriftPath, *Err));
	}

	// The serialized report already carries the summary block.
	TSharedRef<FJsonObject> Report = DriftToJson(D);
	Report->SetStringField(TEXT("drift_report_path"), DriftPath);

	// Images bracketing the divergence frame, if frames were captured.
	if (D.Summary.First.bFound)
	{
		const uint64 F = D.Summary.First.Frame;
		TArray<TSharedPtr<FJsonValue>> Bracket;
		for (int32 Off = -1; Off <= 1; ++Off)
		{
			const int64 N = static_cast<int64>(F) + Off;
			if (N < 0) continue;
			const FString P = Folder / TEXT("frames") / FString::Printf(TEXT("frame_%05lld.png"), N);
			if (FPaths::FileExists(P))
			{
				Bracket.Add(MakeShared<FJsonValueString>(P));
			}
		}
		if (Bracket.Num() > 0)
		{
			Report->SetArrayField(TEXT("divergence_frames"), Bracket);
		}
	}

	// Newest contact sheet, if present.
	{
		const FString CapturesDir = Folder / TEXT("captures");
		TArray<FString> Sheets;
		IFileManager::Get().FindFiles(Sheets, *(CapturesDir / TEXT("contact_*.jpg")), true, false);
		if (Sheets.Num() > 0)
		{
			Sheets.Sort();
			Report->SetStringField(TEXT("contact_sheet_path"), CapturesDir / Sheets.Last());
		}
	}

	return MCPResult(Report);
}

TSharedPtr<FJsonValue> FGameplayHandlers::PieReferenceSave(const TSharedPtr<FJsonObject>& Params)
{
	MCP_CHECK_GAME_THREAD();
	const FString Id = OptionalString(Params, TEXT("recording_id"));
	const FString RecDir = OptionalString(Params, TEXT("recording_dir"));
	if (Id.IsEmpty() && RecDir.IsEmpty())
	{
		return MCPError(TEXT("reference_save requires recording_id or recording_dir"));
	}

	const FString Folder = ResolveRecordingFolder(Params);
	const FString SourceDir = Folder / TEXT("frames");
	const FString ReferenceDir = Folder / TEXT("ref_frames");
	TArray<FString> Frames;
	IFileManager::Get().FindFiles(Frames, *(SourceDir / TEXT("frame_*.png")), true, false);
	Frames.Sort();
	if (Frames.Num() == 0)
	{
		return MCPError(FString::Printf(TEXT("no captured PNG frames found in %s"), *SourceDir));
	}

	IFileManager::Get().DeleteDirectory(*ReferenceDir, false, true);
	if (!IFileManager::Get().MakeDirectory(*ReferenceDir, true))
	{
		return MCPError(FString::Printf(TEXT("could not create reference directory: %s"), *ReferenceDir));
	}

	int32 Copied = 0;
	for (const FString& Name : Frames)
	{
		const FString SourcePath = SourceDir / Name;
		const FString DestPath = ReferenceDir / Name;
		if (IFileManager::Get().Copy(*DestPath, *SourcePath, true, true) != COPY_OK)
		{
			return MCPError(FString::Printf(TEXT("failed to copy reference frame: %s"), *SourcePath));
		}
		++Copied;
	}

	auto Result = MCPSuccess();
	Result->SetStringField(TEXT("recording_dir"), Folder);
	Result->SetStringField(TEXT("reference_dir"), ReferenceDir);
	Result->SetNumberField(TEXT("source_count"), Frames.Num());
	Result->SetNumberField(TEXT("copied_count"), Copied);
	return MCPResult(Result);
}

TSharedPtr<FJsonValue> FGameplayHandlers::PieFrameDiff(const TSharedPtr<FJsonObject>& Params)
{
	MCP_CHECK_GAME_THREAD();
	const FString Id = OptionalString(Params, TEXT("recording_id"));
	const FString RecDir = OptionalString(Params, TEXT("recording_dir"));
	if (Id.IsEmpty() && RecDir.IsEmpty())
	{
		return MCPError(TEXT("frame_diff requires recording_id or recording_dir"));
	}

	const FString Folder = ResolveRecordingFolder(Params);
	const FString LatestDir = Folder / TEXT("frames");
	const FString ReferenceDir = Folder / TEXT("ref_frames");
	TArray<FString> LatestFrames;
	TArray<FString> ReferenceFrames;
	IFileManager::Get().FindFiles(LatestFrames, *(LatestDir / TEXT("frame_*.png")), true, false);
	IFileManager::Get().FindFiles(ReferenceFrames, *(ReferenceDir / TEXT("frame_*.png")), true, false);
	LatestFrames.Sort();
	ReferenceFrames.Sort();
	if (ReferenceFrames.Num() == 0)
	{
		return MCPError(FString::Printf(TEXT("no reference PNG frames found in %s"), *ReferenceDir));
	}
	if (LatestFrames.Num() == 0)
	{
		return MCPError(FString::Printf(TEXT("no latest PNG frames found in %s"), *LatestDir));
	}

	const int32 PixelTolerance = FMath::Clamp(OptionalInt(Params, TEXT("pixel_tolerance"), 8), 0, 255);
	const double ChangedPixelPercentThreshold = FMath::Clamp(
		OptionalNumber(Params, TEXT("changed_pixel_percent"), 0.5), 0.0, 100.0);

	TSet<FString> LatestSet;
	TSet<FString> ReferenceSet;
	for (const FString& Name : LatestFrames) LatestSet.Add(Name);
	for (const FString& Name : ReferenceFrames) ReferenceSet.Add(Name);

	TArray<FString> AllFrames = ReferenceFrames;
	for (const FString& Name : LatestFrames)
	{
		if (!ReferenceSet.Contains(Name)) AllFrames.Add(Name);
	}
	AllFrames.Sort();

	TArray<TSharedPtr<FJsonValue>> FrameResults;
	TArray<TSharedPtr<FJsonValue>> MissingReference;
	TArray<TSharedPtr<FJsonValue>> MissingLatest;
	TArray<TSharedPtr<FJsonValue>> InvalidFrames;
	TSharedRef<FJsonObject> Thresholds = MakeShared<FJsonObject>();
	Thresholds->SetNumberField(TEXT("pixel_tolerance"), PixelTolerance);
	Thresholds->SetNumberField(TEXT("changed_pixel_percent"), ChangedPixelPercentThreshold);

	bool bDiverged = false;
	int32 DifferentCount = 0;
	int32 ComparedCount = 0;
	double MaxChangedPercent = 0.0;
	double MaxMeanAbsError = 0.0;
	FString FirstDivergentFrame;

	// ponytail: synchronous scan keeps the AI call one-shot; move decode/compare off-thread if frame sets become large.
	for (const FString& Name : AllFrames)
	{
		const bool bHasLatest = LatestSet.Contains(Name);
		const bool bHasReference = ReferenceSet.Contains(Name);
		TSharedRef<FJsonObject> Frame = MakeShared<FJsonObject>();
		Frame->SetStringField(TEXT("frame"), Name);
		const int64 FrameIndex = FrameIndexFromName(Name);
		if (FrameIndex >= 0) Frame->SetNumberField(TEXT("frame_index"), static_cast<double>(FrameIndex));

		bool bDifferent = false;
		if (!bHasReference)
		{
			Frame->SetStringField(TEXT("status"), TEXT("missing_reference"));
			MissingReference.Add(MakeShared<FJsonValueString>(Name));
			bDifferent = true;
		}
		else if (!bHasLatest)
		{
			Frame->SetStringField(TEXT("status"), TEXT("missing_latest"));
			MissingLatest.Add(MakeShared<FJsonValueString>(Name));
			bDifferent = true;
		}
		else
		{
			TArray<FColor> ReferencePixels;
			TArray<FColor> LatestPixels;
			int32 ReferenceWidth = 0, ReferenceHeight = 0;
			int32 LatestWidth = 0, LatestHeight = 0;
			const bool bReferenceLoaded = LoadFrameImage(ReferenceDir / Name, ReferencePixels, ReferenceWidth, ReferenceHeight);
			const bool bLatestLoaded = LoadFrameImage(LatestDir / Name, LatestPixels, LatestWidth, LatestHeight);
			Frame->SetNumberField(TEXT("reference_width"), ReferenceWidth);
			Frame->SetNumberField(TEXT("reference_height"), ReferenceHeight);
			Frame->SetNumberField(TEXT("latest_width"), LatestWidth);
			Frame->SetNumberField(TEXT("latest_height"), LatestHeight);

			if (!bReferenceLoaded || !bLatestLoaded)
			{
				Frame->SetStringField(TEXT("status"), TEXT("invalid"));
				Frame->SetStringField(TEXT("error"), !bReferenceLoaded ? TEXT("reference frame could not be decoded") : TEXT("latest frame could not be decoded"));
				InvalidFrames.Add(MakeShared<FJsonValueString>(Name));
				bDifferent = true;
			}
			else if (ReferenceWidth != LatestWidth || ReferenceHeight != LatestHeight || ReferencePixels.Num() != LatestPixels.Num())
			{
				Frame->SetStringField(TEXT("status"), TEXT("different"));
				Frame->SetBoolField(TEXT("dimension_mismatch"), true);
				bDifferent = true;
			}
			else
			{
				uint64 ChangedPixels = 0;
				uint8 MaxChannelError = 0;
				double SumAbsError = 0.0;
				for (int32 Pixel = 0; Pixel < ReferencePixels.Num(); ++Pixel)
				{
					const FColor& A = ReferencePixels[Pixel];
					const FColor& B = LatestPixels[Pixel];
					const int32 DR = FMath::Abs(static_cast<int32>(A.R) - static_cast<int32>(B.R));
					const int32 DG = FMath::Abs(static_cast<int32>(A.G) - static_cast<int32>(B.G));
					const int32 DB = FMath::Abs(static_cast<int32>(A.B) - static_cast<int32>(B.B));
					const int32 MaxError = FMath::Max(DR, FMath::Max(DG, DB));
					if (MaxError > PixelTolerance) ++ChangedPixels;
					MaxChannelError = FMath::Max(MaxChannelError, static_cast<uint8>(MaxError));
					SumAbsError += (DR + DG + DB) / 3.0;
				}

				const double PixelCount = static_cast<double>(ReferencePixels.Num());
				const double ChangedPercent = PixelCount > 0.0 ? (static_cast<double>(ChangedPixels) * 100.0 / PixelCount) : 0.0;
				const double MeanAbsError = PixelCount > 0.0 ? SumAbsError / PixelCount : 0.0;
				Frame->SetStringField(TEXT("status"), ChangedPercent > ChangedPixelPercentThreshold ? TEXT("different") : TEXT("match"));
				Frame->SetNumberField(TEXT("changed_pixels"), static_cast<double>(ChangedPixels));
				Frame->SetNumberField(TEXT("pixel_count"), PixelCount);
				Frame->SetNumberField(TEXT("changed_pixel_percent"), ChangedPercent);
				Frame->SetNumberField(TEXT("mean_abs_error"), MeanAbsError);
				Frame->SetNumberField(TEXT("max_channel_error"), MaxChannelError);
				bDifferent = ChangedPercent > ChangedPixelPercentThreshold;
				++ComparedCount;
				MaxChangedPercent = FMath::Max(MaxChangedPercent, ChangedPercent);
				MaxMeanAbsError = FMath::Max(MaxMeanAbsError, MeanAbsError);
			}
		}

		Frame->SetBoolField(TEXT("different"), bDifferent);
		FrameResults.Add(MakeShared<FJsonValueObject>(Frame));
		if (bDifferent)
		{
			bDiverged = true;
			++DifferentCount;
			if (FirstDivergentFrame.IsEmpty()) FirstDivergentFrame = Name;
		}
	}

	auto Result = MCPSuccess();
	Result->SetStringField(TEXT("recording_dir"), Folder);
	Result->SetStringField(TEXT("latest_dir"), LatestDir);
	Result->SetStringField(TEXT("reference_dir"), ReferenceDir);
	Result->SetNumberField(TEXT("latest_count"), LatestFrames.Num());
	Result->SetNumberField(TEXT("reference_count"), ReferenceFrames.Num());
	Result->SetNumberField(TEXT("compared_count"), ComparedCount);
	Result->SetNumberField(TEXT("different_count"), DifferentCount);
	Result->SetNumberField(TEXT("max_changed_pixel_percent"), MaxChangedPercent);
	Result->SetNumberField(TEXT("max_mean_abs_error"), MaxMeanAbsError);
	Result->SetBoolField(TEXT("diverged"), bDiverged);
	Result->SetObjectField(TEXT("thresholds"), Thresholds);
	Result->SetStringField(TEXT("tolerance_note"), TEXT("Per-channel RGB differences at or below pixel_tolerance are ignored; PNG encoding is lossless."));
	if (!FirstDivergentFrame.IsEmpty())
	{
		Result->SetStringField(TEXT("first_divergent_frame"), FirstDivergentFrame);
		const int64 FirstIndex = FrameIndexFromName(FirstDivergentFrame);
		if (FirstIndex >= 0) Result->SetNumberField(TEXT("first_divergent_frame_index"), static_cast<double>(FirstIndex));
	}
	Result->SetArrayField(TEXT("missing_reference_frames"), MissingReference);
	Result->SetArrayField(TEXT("missing_latest_frames"), MissingLatest);
	Result->SetArrayField(TEXT("invalid_frames"), InvalidFrames);
	Result->SetArrayField(TEXT("frames"), FrameResults);
	return MCPResult(Result);
}
