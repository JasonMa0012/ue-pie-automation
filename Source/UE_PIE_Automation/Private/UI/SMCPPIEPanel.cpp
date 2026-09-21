#include "SMCPPIEPanel.h"
#include "UE_PIE_AutomationModule.h"
#include "PIE/PIEInputRecorder.h"
#include "PIE/PIEInputReplayer.h"
#include "PIE/PIEObserver.h"
#include "PIE/MCPObservationProfile.h"
#include "PIE/PIESequenceFormat.h"
#include "Handlers/GameplayHandlers.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Layout/SExpandableArea.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SSpinBox.h"
#include "EditorAssetLibrary.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/SavePackage.h"
#include "Widgets/Text/STextBlock.h"
#include "Framework/Docking/TabManager.h"
#include "ToolMenus.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"
#include "Styling/AppStyle.h"
#include "Styling/SlateStyleRegistry.h"
#include "Framework/Application/SlateApplication.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Misc/MessageDialog.h"
#include "Modules/ModuleManager.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "Kismet/GameplayStatics.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformApplicationMisc.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/SWindow.h"

const FName SMCPPIEPanel::TabId(TEXT("MCPPIEPanel"));

#define IMAGE_BRUSH(RelativePath, ...) FSlateImageBrush(UE_PIE_AutomationStyleSet->RootToContentDir(RelativePath, TEXT(".png")), __VA_ARGS__)

static TSharedPtr<FSlateStyleSet> UE_PIE_AutomationStyleSet;

static void RegisterUE_PIE_AutomationStyle()
{
	if (UE_PIE_AutomationStyleSet.IsValid()) return;

	FString ResourcesDir = FPaths::Combine(
		FPaths::ProjectPluginsDir(), TEXT("UE_PIE_Automation"), TEXT("Resources"));

	UE_PIE_AutomationStyleSet = MakeShareable(new FSlateStyleSet("UE_PIE_AutomationStyle"));
	UE_PIE_AutomationStyleSet->SetContentRoot(ResourcesDir);
	UE_PIE_AutomationStyleSet->Set("UE_PIE_Automation.Record", new IMAGE_BRUSH("Record_Icon40x", CoreStyleConstants::Icon16x16));
	UE_PIE_AutomationStyleSet->Set("UE_PIE_Automation.RecordPlay", new IMAGE_BRUSH("RecordPlay_Icon40x", CoreStyleConstants::Icon16x16));
	FSlateStyleRegistry::RegisterSlateStyle(*UE_PIE_AutomationStyleSet);
}

static void UnregisterUE_PIE_AutomationStyle()
{
	if (UE_PIE_AutomationStyleSet.IsValid())
	{
		FSlateStyleRegistry::UnRegisterSlateStyle(*UE_PIE_AutomationStyleSet);
		UE_PIE_AutomationStyleSet.Reset();
	}
}

#undef IMAGE_BRUSH

namespace
{
	FString RecorderStateStr(UE_PIE_Automation::ERecorderState S)
	{
		switch (S)
		{
		case UE_PIE_Automation::ERecorderState::Idle:           return TEXT("Idle");
		case UE_PIE_Automation::ERecorderState::Armed:          return TEXT("Armed");
		case UE_PIE_Automation::ERecorderState::WaitingForPawn: return TEXT("Waiting for Pawn");
		case UE_PIE_Automation::ERecorderState::Recording:      return TEXT("Recording");
		}
		return TEXT("?");
	}

	FString ReplayerStateStr(UE_PIE_Automation::EReplayerState S)
	{
		switch (S)
		{
		case UE_PIE_Automation::EReplayerState::Idle:           return TEXT("Idle");
		case UE_PIE_Automation::EReplayerState::Armed:          return TEXT("Armed");
		case UE_PIE_Automation::EReplayerState::WaitingForPawn: return TEXT("Waiting for Pawn");
		case UE_PIE_Automation::EReplayerState::Replaying:      return TEXT("Replaying");
		case UE_PIE_Automation::EReplayerState::Completed:      return TEXT("Completed");
		}
		return TEXT("?");
	}

	FString ObserverStateStr(UE_PIE_Automation::EObserverState S)
	{
		switch (S)
		{
		case UE_PIE_Automation::EObserverState::Idle:           return TEXT("Idle");
		case UE_PIE_Automation::EObserverState::Armed:          return TEXT("Armed");
		case UE_PIE_Automation::EObserverState::WaitingForPawn: return TEXT("Waiting for Pawn");
		case UE_PIE_Automation::EObserverState::Observing:      return TEXT("Observing");
		case UE_PIE_Automation::EObserverState::Completed:      return TEXT("Completed");
		}
		return TEXT("?");
	}

	FSlateColor StateColor(bool bActive)
	{
		return bActive ? FSlateColor(FLinearColor::Green) : FSlateColor(FSlateColor::UseForeground());
	}

	bool PromptForRecordingRename(const FString& CurrentId, FString& OutNewId)
	{
		TSharedPtr<SWindow> Window;
		TSharedPtr<SEditableTextBox> TextBox;
		bool bAccepted = false;

		Window = SNew(SWindow)
			.Title(FText::FromString(TEXT("Rename Recording")))
			.HasCloseButton(true)
			.SupportsMaximize(false)
			.SupportsMinimize(false)
			.SizingRule(ESizingRule::FixedSize)
			.ClientSize(FVector2D(440.f, 140.f))
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight().Padding(12, 12, 12, 6)
				[
					SNew(STextBlock).Text(FText::FromString(TEXT("Recording name")))
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(12, 0, 12, 8)
				[
					SAssignNew(TextBox, SEditableTextBox)
					.Text(FText::FromString(CurrentId))
					.SelectAllTextWhenFocused(true)
					.OnTextCommitted_Lambda([&](const FText& InText, ETextCommit::Type CommitType)
					{
						if (CommitType == ETextCommit::OnEnter)
						{
							OutNewId = InText.ToString();
							bAccepted = !OutNewId.IsEmpty();
							Window->RequestDestroyWindow();
						}
					})
				]
				+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Right).Padding(12, 0, 12, 10)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 6, 0)
					[
						SNew(SButton)
						.Text(FText::FromString(TEXT("Cancel")))
						.OnClicked_Lambda([&]()
						{
							Window->RequestDestroyWindow();
							return FReply::Handled();
						})
					]
					+ SHorizontalBox::Slot().AutoWidth()
					[
						SNew(SButton)
						.Text(FText::FromString(TEXT("Rename")))
						.OnClicked_Lambda([&]()
						{
							OutNewId = TextBox->GetText().ToString();
							bAccepted = !OutNewId.IsEmpty();
							Window->RequestDestroyWindow();
							return FReply::Handled();
						})
					]
				]
			];

		FSlateApplication::Get().AddModalWindow(Window.ToSharedRef(), FSlateApplication::Get().GetActiveTopLevelWindow());
		return bAccepted;
	}

}

void SMCPPIEPanel::RegisterTab()
{
	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(TabId,
		FOnSpawnTab::CreateLambda([](const FSpawnTabArgs&) -> TSharedRef<SDockTab>
		{
			return SNew(SDockTab)
				.TabRole(NomadTab)
				.Label(FText::FromString(TEXT("UE PIE Automation")))
				[
					SNew(SMCPPIEPanel)
				];
		}))
		.SetDisplayName(FText::FromString(TEXT("UE PIE Automation")))
		.SetTooltipText(FText::FromString(TEXT("UE PIE Automation — Record / Replay / Observe")))
		.SetGroup(WorkspaceMenu::GetMenuStructure().GetToolsCategory());
}

void SMCPPIEPanel::UnregisterTab()
{
	FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(TabId);
}

void SMCPPIEPanel::OpenTab()
{
	FGlobalTabmanager::Get()->TryInvokeTab(TabId);
}

TSharedPtr<FExtender> SMCPPIEPanel::ToolbarExtender;

SMCPPIEPanel::~SMCPPIEPanel()
{
	if (EndPIEHandle.IsValid())
	{
		FEditorDelegates::EndPIE.Remove(EndPIEHandle);
		EndPIEHandle.Reset();
	}
}

void SMCPPIEPanel::RegisterToolbarButton()
{
	RegisterUE_PIE_AutomationStyle();

	UToolMenu* ToolBar = UToolMenus::Get()->ExtendMenu("LevelEditor.LevelEditorToolBar.PlayToolBar");
	FToolMenuSection& Section = ToolBar->FindOrAddSection("UE_PIE_Automation");

	Section.AddDynamicEntry("UE_PIE_AutomationActions", FNewToolMenuSectionDelegate::CreateLambda([](FToolMenuSection& InSection)
	{
		{
			FToolMenuEntry Entry =
				FToolMenuEntry::InitToolBarButton(
					"Record",
					FExecuteAction::CreateLambda([]()
					{
						UE_PIE_Automation::FRecorderArmConfig Cfg;
						FString Err, Msg;
						UE_PIE_Automation::FPIEInputRecorder::Get().Arm(Cfg, Err, Msg);
					}),
					FText::GetEmpty(),
					FText::FromString(TEXT("Arm MCP recorder (waits for PIE start)")),
					FSlateIcon("UE_PIE_AutomationStyle", "UE_PIE_Automation.Record"));
			Entry.StyleNameOverride = FName("Toolbar.BackplateLeft");
			InSection.AddEntry(Entry);
		}

		{
			FToolMenuEntry Entry =
				FToolMenuEntry::InitToolBarButton(
					"RecordPlay",
					FExecuteAction::CreateLambda([]()
					{
						UE_PIE_Automation::FRecorderArmConfig Cfg;
						FString Err, Msg;
						UE_PIE_Automation::FPIEInputRecorder::Get().Arm(Cfg, Err, Msg);
						if (GEditor && !GEditor->PlayWorld)
						{
							FRequestPlaySessionParams P;
							GEditor->RequestPlaySession(P);
						}
					}),
					FText::GetEmpty(),
					FText::FromString(TEXT("Arm MCP recorder and start PIE")),
					FSlateIcon("UE_PIE_AutomationStyle", "UE_PIE_Automation.RecordPlay"));
			Entry.StyleNameOverride = FName("Toolbar.BackplateCenter");
			InSection.AddEntry(Entry);
		}

		{
			FToolMenuEntry ComboEntry =
				FToolMenuEntry::InitComboButton(
					"UE_PIE_AutomationMenu",
					FUIAction(),
					FNewMenuDelegate::CreateLambda([](FMenuBuilder& Menu)
					{
						Menu.BeginSection("Recording", FText::FromString(TEXT("Recording")));

						Menu.AddMenuEntry(
							FText::FromString(TEXT("Record + Play")),
							FText::FromString(TEXT("Arm MCP recorder and start PIE")),
							FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Recording"),
							FUIAction(FExecuteAction::CreateLambda([]()
							{
								UE_PIE_Automation::FRecorderArmConfig Cfg;
								FString Err, Msg;
								UE_PIE_Automation::FPIEInputRecorder::Get().Arm(Cfg, Err, Msg);
								if (GEditor && !GEditor->PlayWorld)
								{
									FRequestPlaySessionParams P;
									GEditor->RequestPlaySession(P);
								}
							}))
						);

						Menu.AddMenuEntry(
							FText::FromString(TEXT("Arm Recorder")),
							FText::FromString(TEXT("Arm MCP recorder (waits for PIE start)")),
							FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Recording"),
							FUIAction(FExecuteAction::CreateLambda([]()
							{
								UE_PIE_Automation::FRecorderArmConfig Cfg;
								FString Err, Msg;
								UE_PIE_Automation::FPIEInputRecorder::Get().Arm(Cfg, Err, Msg);
							}))
						);

						const auto RecState = UE_PIE_Automation::FPIEInputRecorder::Get().GetStatus().State;

						if (RecState == UE_PIE_Automation::ERecorderState::Armed || RecState == UE_PIE_Automation::ERecorderState::WaitingForPawn)
						{
							Menu.AddMenuEntry(
								FText::FromString(TEXT("Disarm Recorder")),
								FText::FromString(TEXT("Disarm MCP recorder")),
								FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.X"),
								FUIAction(FExecuteAction::CreateLambda([]()
								{
									FString Err;
									UE_PIE_Automation::FPIEInputRecorder::Get().Disarm(Err);
								}))
							);
						}

						if (RecState == UE_PIE_Automation::ERecorderState::Recording)
						{
							Menu.AddMenuEntry(
								FText::FromString(TEXT("Stop Recording")),
								FText::FromString(TEXT("Force stop MCP recording")),
								FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Delete"),
								FUIAction(FExecuteAction::CreateLambda([]()
								{
									UE_PIE_Automation::FPIEInputRecorder::Get().ForceStop();
								}))
							);
						}

						Menu.EndSection();

						Menu.BeginSection("Panel", FText::FromString(TEXT("Panel")));
						Menu.AddMenuEntry(
							FText::FromString(TEXT("Open UE PIE Automation Panel")),
							FText::FromString(TEXT("Open the full UE PIE Automation control panel")),
							FSlateIcon(),
							FUIAction(FExecuteAction::CreateLambda([]()
							{
								SMCPPIEPanel::OpenTab();
							}))
						);
						Menu.EndSection();
					}),
					FText::GetEmpty(),
					FText::FromString(TEXT("UE PIE Automation Options")));
			ComboEntry.StyleNameOverride = FName("Toolbar.BackplateRightCombo");
			InSection.AddEntry(ComboEntry);
		}
	}));
}

void SMCPPIEPanel::UnregisterToolbarButton()
{
	UnregisterUE_PIE_AutomationStyle();
}

void SMCPPIEPanel::Construct(const FArguments& InArgs)
{
	ChildSlot
	[
		SNew(SScrollBox)
		+ SScrollBox::Slot().Padding(8)
		[
			SNew(SVerticalBox)

			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
			[ BuildRecorderSection() ]

			+ SVerticalBox::Slot().AutoHeight().Padding(0, 4)
			[ SNew(SSeparator) ]

			+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 4)
			[ BuildRecordingsSection() ]

			+ SVerticalBox::Slot().AutoHeight().Padding(0, 4)
			[ SNew(SSeparator) ]

			+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 4)
			[ BuildProfilesSection() ]

			+ SVerticalBox::Slot().AutoHeight().Padding(0, 4)
			[ SNew(SSeparator) ]

			+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 4)
			[ BuildLiveDebugSection() ]
		]
	];

	RefreshRecordings();
	RefreshProfiles();
	if (!EndPIEHandle.IsValid())
	{
		EndPIEHandle = FEditorDelegates::EndPIE.AddRaw(this, &SMCPPIEPanel::OnEndPIE);
	}
}

void SMCPPIEPanel::OnEndPIE(bool bIsSimulating)
{
	RefreshRecordings();
	RefreshProfiles();
}

void SMCPPIEPanel::RenameRecording(const FString& RecordingId)
{
	if (GEditor && GEditor->PlayWorld)
	{
		FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(TEXT("Stop PIE before renaming a recording.")));
		return;
	}

	FString NewId;
	if (!PromptForRecordingRename(RecordingId, NewId)) return;
	NewId.TrimStartAndEndInline();
	if (NewId.IsEmpty() || NewId == RecordingId) return;

	FText InvalidReason;
	if (NewId.Contains(TEXT("/")) || NewId.Contains(TEXT("\\")) || NewId == TEXT(".") || NewId == TEXT("..")
		|| !FPaths::ValidatePath(NewId, &InvalidReason))
	{
		FMessageDialog::Open(EAppMsgType::Ok, InvalidReason.IsEmpty()
			? FText::FromString(TEXT("The recording name contains invalid characters."))
			: InvalidReason);
		return;
	}

	const FString Root = UE_PIE_Automation::DefaultRecordingsRoot();
	const FString OldDir = Root / RecordingId;
	const FString NewDir = Root / NewId;
	if (!IFileManager::Get().DirectoryExists(*OldDir))
	{
		FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(TEXT("The recording directory no longer exists.")));
		RefreshRecordings();
		return;
	}
	if (IFileManager::Get().DirectoryExists(*NewDir))
	{
		FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(TEXT("A recording with that name already exists.")));
		return;
	}

	if (!IFileManager::Get().Move(*NewDir, *OldDir, false, true, false, true))
	{
		FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(TEXT("Failed to rename the recording directory.")));
		return;
	}

	const TCHAR* MetadataFiles[] = { TEXT("manifest.json"), TEXT("sequence.json"), TEXT("recording.csv"), TEXT("drift.json") };
	for (const TCHAR* Filename : MetadataFiles)
	{
		const FString Path = NewDir / Filename;
		if (!FPaths::FileExists(Path)) continue;

		FString Contents;
		if (FFileHelper::LoadFileToString(Contents, *Path))
		{
			Contents.ReplaceInline(*RecordingId, *NewId, ESearchCase::CaseSensitive);
			FFileHelper::SaveStringToFile(Contents, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		}
	}

	RefreshRecordings();
}

TSharedRef<SWidget> SMCPPIEPanel::BuildRecorderSection()
{
	return SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Font(FAppStyle::GetFontStyle("BoldFont"))
				.Text(FText::FromString(TEXT("Recorder")))
			]
			+ SHorizontalBox::Slot().FillWidth(1.f)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(4, 0, 2, 0)
			[
				SNew(STextBlock).Text(FText::FromString(TEXT("Time Scale")))
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(2, 0, 2, 0)
			[
				SNew(SSpinBox<float>)
				.MinValue(1.0f)
				.MaxValue(400.0f)
				.Delta(1.0f)
				.MinFractionalDigits(0)
				.MaxFractionalDigits(2)
				.Value_Lambda([this]() { return CurrentTimeScale * 100.0f; })
				.OnValueChanged_Lambda([this](float Value) { ApplyTimeScale(Value / 100.0f); })
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 6, 0)
			[
				SNew(STextBlock).Text(FText::FromString(TEXT("%")))
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(4, 0)
			[
				SAssignNew(RecorderStateText, STextBlock)
				.Text(FText::FromString(TEXT("Idle")))
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 4)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 4, 0)
			[
				SNew(SButton)
				.Text(FText::FromString(TEXT("Record + Play")))
				.OnClicked_Lambda([]()
				{
					UE_PIE_Automation::FRecorderArmConfig Cfg;
					FString Err, Msg;
					UE_PIE_Automation::FPIEInputRecorder::Get().Arm(Cfg, Err, Msg);
					if (GEditor && !GEditor->PlayWorld)
					{
						FRequestPlaySessionParams P; GEditor->RequestPlaySession(P);
					}
					return FReply::Handled();
				})
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 4, 0)
			[
				SNew(SButton)
				.Text(FText::FromString(TEXT("Arm")))
				.OnClicked_Lambda([]()
				{
					UE_PIE_Automation::FRecorderArmConfig Cfg;
					FString Err, Msg;
					UE_PIE_Automation::FPIEInputRecorder::Get().Arm(Cfg, Err, Msg);
					return FReply::Handled();
				})
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 4, 0)
			[
				SNew(SButton)
				.Text(FText::FromString(TEXT("Disarm")))
				.OnClicked_Lambda([]()
				{
					FString Err;
					UE_PIE_Automation::FPIEInputRecorder::Get().Disarm(Err);
					return FReply::Handled();
				})
			]
			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(SButton)
				.Text(FText::FromString(TEXT("Stop")))
				.OnClicked_Lambda([]()
				{
					UE_PIE_Automation::FPIEInputRecorder::Get().ForceStop();
					return FReply::Handled();
				})
			]
		];
}


void SMCPPIEPanel::ApplyTimeScale(float Scale)
{
	CurrentTimeScale = Scale;
	UWorld* World = GEditor ? GEditor->PlayWorld : nullptr;
	if (World)
	{
		AWorldSettings* WS = World->GetWorldSettings();
		if (WS)
		{
			WS->MaxGlobalTimeDilation = FMath::Max(WS->MaxGlobalTimeDilation, 1000.f);
			WS->MinGlobalTimeDilation = FMath::Min(WS->MinGlobalTimeDilation, 0.0001f);
			UGameplayStatics::SetGlobalTimeDilation(World, Scale);
		}
	}

}

TSharedRef<SWidget> SMCPPIEPanel::BuildRecordingsSection()
{
	return SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Font(FAppStyle::GetFontStyle("BoldFont"))
				.Text(FText::FromString(TEXT("Recordings")))
			]
			+ SHorizontalBox::Slot().FillWidth(1.f)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(6, 0, 2, 0)
			[
				SNew(STextBlock).Text(FText::FromString(TEXT("Capture PNG FPS")))
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(SSpinBox<int32>)
				.MinValue(1)
				.MaxValue(120)
				.Delta(1)
				.Value_Lambda([this]() { return CaptureFPS; })
				.OnValueChanged_Lambda([this](int32 V) { CaptureFPS = FMath::Clamp(V, 1, 120); })
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(6, 0, 2, 0)
			[
				SNew(STextBlock).Text(FText::FromString(TEXT("Resolution %")))
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(SSpinBox<int32>)
				.MinValue(1)
				.MaxValue(100)
				.Delta(5)
				.Value_Lambda([this]() { return CaptureResolutionPercent; })
				.OnValueChanged_Lambda([this](int32 V) { CaptureResolutionPercent = FMath::Clamp(V, 1, 100); })
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(4, 0)
			[
				SNew(SButton)
				.Text(FText::FromString(TEXT("Refresh")))
				.OnClicked_Lambda([this]()
				{
					RefreshRecordings();
					return FReply::Handled();
				})
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 4)
		[
			SAssignNew(RecordingsListBox, SVerticalBox)
		];
}

TSharedRef<SWidget> SMCPPIEPanel::BuildProfilesSection()
{
	return SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Font(FAppStyle::GetFontStyle("BoldFont"))
				.Text(FText::FromString(TEXT("Observation Profiles")))
			]
			+ SHorizontalBox::Slot().FillWidth(1.f)
			+ SHorizontalBox::Slot().AutoWidth().Padding(4, 0)
			[
				SNew(SButton)
				.Text(FText::FromString(TEXT("Create")))
				.OnClicked_Lambda([this]()
				{
					FString PackagePath = TEXT("/Game/UE_PIE_Automation");
					FString AssetName = FString::Printf(TEXT("OP_%s"), *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")));
					FString FullPath = PackagePath / AssetName;

					UPackage* Pkg = CreatePackage(*FullPath);
					UMCPObservationProfile* NewProfile = NewObject<UMCPObservationProfile>(Pkg, *AssetName, RF_Public | RF_Standalone);
					FAssetRegistryModule::AssetCreated(NewProfile);
					NewProfile->MarkPackageDirty();

					FString PackageFilename = FPackageName::LongPackageNameToFilename(FullPath, FPackageName::GetAssetPackageExtension());
					UPackage::SavePackage(Pkg, NewProfile, *PackageFilename, FSavePackageArgs());

					GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAsset(NewProfile);
					RefreshProfiles();
					return FReply::Handled();
				})
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(4, 0)
			[
				SNew(SButton)
				.Text(FText::FromString(TEXT("Refresh")))
				.OnClicked_Lambda([this]()
				{
					RefreshProfiles();
					return FReply::Handled();
				})
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 4)
		[
			SAssignNew(ProfilesListBox, SVerticalBox)
		];
}

void SMCPPIEPanel::Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime)
{
	SCompoundWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);

	// Update state labels
	{
		const auto RS = UE_PIE_Automation::FPIEInputRecorder::Get().GetStatus();
		const FString RecText = FString::Printf(TEXT("%s  %s  F:%d  %.1fs"),
			*RecorderStateStr(RS.State), *RS.Id, RS.CurrentFrame, RS.ElapsedSeconds);
		const bool bRecActive = RS.State == UE_PIE_Automation::ERecorderState::Recording;
		RecorderStateText->SetText(FText::FromString(RecText));
		RecorderStateText->SetColorAndOpacity(StateColor(bRecActive));
	}

	// Re-apply time scale if PIE is running and dilation doesn't match
	if (CurrentTimeScale != 1.0f)
	{
		UWorld* World = GEditor ? GEditor->PlayWorld : nullptr;
		if (World)
		{
			AWorldSettings* WS = World->GetWorldSettings();
			if (WS && !FMath::IsNearlyEqual(UGameplayStatics::GetGlobalTimeDilation(World), CurrentTimeScale, 0.001f))
			{
				ApplyTimeScale(CurrentTimeScale);
			}
		}
	}

	// Live debug: refresh every 3rd tick when expanded and active
	if (bLiveDebugExpanded && LiveDebugContent.IsValid())
	{
		const bool bReplayActive = UE_PIE_Automation::FPIEInputReplayer::Get().IsActive();
		const bool bObserverActive = UE_PIE_Automation::FPIEObserver::Get().IsActive();
		if (bReplayActive || bObserverActive)
		{
			if (++LiveDebugTickCounter >= 3)
			{
				LiveDebugTickCounter = 0;
				RefreshLiveDebug();
			}
		}
		else if (LiveDebugContent->NumSlots() > 0)
		{
			LiveDebugContent->ClearChildren();
			LiveDebugContent->AddSlot().AutoHeight()
			[
				SNew(STextBlock)
				.Text(FText::FromString(TEXT("No active replay or observation")))
				.ColorAndOpacity(FSlateColor(FLinearColor::Gray))
			];
		}
	}
}

void SMCPPIEPanel::RefreshRecordings()
{
	if (!RecordingsListBox.IsValid()) return;
	RecordingsListBox->ClearChildren();
	CachedRecordingIds.Reset();

	const FString Root = UE_PIE_Automation::DefaultRecordingsRoot();
	TArray<FString> Dirs;
	IFileManager::Get().FindFiles(Dirs, *(Root / TEXT("*")), false, true);
	Dirs.Sort([](const FString& A, const FString& B) { return A > B; });
	if (Dirs.Num() > 50) Dirs.SetNum(50);

	for (const FString& D : Dirs)
	{
		CachedRecordingIds.Add(D);
		const FString Id = D;
		const FString RecDir = Root / Id;

		TArray<FString> PNGFrames;
		IFileManager::Get().FindFiles(PNGFrames, *(RecDir / TEXT("frames/frame_*.png")), true, false);
		TArray<FString> ReferenceFrames;
		IFileManager::Get().FindFiles(ReferenceFrames, *(RecDir / TEXT("ref_frames/frame_*.png")), true, false);

		RecordingsListBox->AddSlot().AutoHeight().Padding(0, 2)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
					SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(STextBlock).Text(FText::FromString(Id))
				]
				+ SHorizontalBox::Slot().FillWidth(1.f).HAlign(HAlign_Center).VAlign(VAlign_Center).Padding(12, 0)
				[
					SNew(STextBlock)
					.Text(FText::FromString(FString::Printf(TEXT("Capture: %d  |  Reference: %d"), PNGFrames.Num(), ReferenceFrames.Num())))
					.ColorAndOpacity(FSlateColor(FLinearColor(0.7f, 0.7f, 0.7f)))
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(2, 0)
				[
					SNew(SButton)
					.ButtonStyle(FAppStyle::Get(), "FlatButton.Danger")
					.TextStyle(FAppStyle::Get(), "FlatButton.DefaultTextStyle")
					.Text(FText::FromString(TEXT("Delete")))
					.ToolTipText(FText::FromString(TEXT("Delete this recording and its PNG frames")))
					.OnClicked_Lambda([this, Id]()
					{
						const FString RecPath = UE_PIE_Automation::DefaultRecordingsRoot() / Id;
						IFileManager::Get().DeleteDirectory(*RecPath, false, true);
						RefreshRecordings();
						return FReply::Handled();
					})
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(2, 0)
				[
					SNew(SButton)
					.Text(FText::FromString(TEXT("Open Location")))
					.ToolTipText(FText::FromString(TEXT("Open the recording directory")))
					.OnClicked_Lambda([RecDir]()
					{
						FPlatformProcess::ExploreFolder(*FPaths::ConvertRelativePathToFull(RecDir));
						return FReply::Handled();
					})
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(2, 0)
				[
					SNew(SButton)
					.Text(FText::FromString(TEXT("Copy Name")))
					.ToolTipText(FText::FromString(TEXT("Copy the recording name")))
					.OnClicked_Lambda([Id]()
					{
						FPlatformApplicationMisc::ClipboardCopy(*Id);
						return FReply::Handled();
					})
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(2, 0)
				[
					SNew(SButton)
					.Text(FText::FromString(TEXT("Rename")))
					.ToolTipText(FText::FromString(TEXT("Rename this recording")))
					.OnClicked_Lambda([this, Id]()
					{
						RenameRecording(Id);
						return FReply::Handled();
					})
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(2, 0)
				[
					SNew(SButton)
					.Text(FText::FromString(TEXT("Save as Reference")))
					.ToolTipText(FText::FromString(TEXT("Replace ref_frames with the current PNG frames")))
					.OnClicked_Lambda([Id]()
					{
						TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
						Params->SetStringField(TEXT("recording_id"), Id);
						FGameplayHandlers::PieReferenceSave(Params);
						return FReply::Handled();
					})
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(2, 0)
				[
					SNew(SButton)
					.ButtonStyle(FAppStyle::Get(), "FlatButton.Success")
					.TextStyle(FAppStyle::Get(), "FlatButton.DefaultTextStyle")
					.Text(FText::FromString(TEXT("Play & Capture")))
					.ToolTipText(FText::FromString(TEXT("Replay and capture the PNG sequence")))
					.OnClicked_Lambda([this, Id]()
					{
						UE_PIE_Automation::FReplayerArmConfig Cfg;
						Cfg.SourceRecordingId = Id;
						Cfg.CaptureFPS = CaptureFPS;
						Cfg.CaptureResolutionPercent = CaptureResolutionPercent;
						FString Err, Msg;
						UE_PIE_Automation::FPIEInputReplayer::Get().Arm(Cfg, Err, Msg);
						for (const FString& ProfilePath : ActiveProfilePaths)
						{
							UE_PIE_Automation::FObserverArmConfig OCfg;
							OCfg.ProfilePath = ProfilePath;
							FString OErr, OMsg;
							UE_PIE_Automation::FPIEObserver::Get().Arm(OCfg, OErr, OMsg);
						}
						if (GEditor && !GEditor->PlayWorld)
						{
							FRequestPlaySessionParams P;
							GEditor->RequestPlaySession(P);
						}
						return FReply::Handled();
					})
				]
			]
		];

	}

	if (Dirs.Num() == 0)
	{
		RecordingsListBox->AddSlot().AutoHeight()
		[
			SNew(STextBlock)
			.Text(FText::FromString(TEXT("No recordings found")))
			.ColorAndOpacity(FSlateColor(FLinearColor::Gray))
		];
	}
}

void SMCPPIEPanel::RefreshProfiles()
{
	if (!ProfilesListBox.IsValid()) return;
	ProfilesListBox->ClearChildren();
	CachedProfilePaths.Reset();

	FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	TArray<FAssetData> Assets;
	ARM.Get().GetAssetsByClass(UMCPObservationProfile::StaticClass()->GetClassPathName(), Assets, true);

	for (const FAssetData& A : Assets)
	{
		const FString Path = A.GetObjectPathString();
		CachedProfilePaths.Add(Path);
		const FString Name = A.AssetName.ToString();
		const bool bActive = ActiveProfilePaths.Contains(Path);

		ProfilesListBox->AddSlot().AutoHeight().Padding(0, 2)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 4, 0)
			[
				SNew(SCheckBox)
				.IsChecked(bActive ? ECheckBoxState::Checked : ECheckBoxState::Unchecked)
				.OnCheckStateChanged_Lambda([this, Path](ECheckBoxState NewState)
				{
					if (NewState == ECheckBoxState::Checked)
						ActiveProfilePaths.Add(Path);
					else
						ActiveProfilePaths.Remove(Path);
				})
			]
			+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
			[
				SNew(STextBlock).Text(FText::FromString(Name))
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(2, 0)
			[
				SNew(SButton)
				.Text(FText::FromString(TEXT("Edit")))
				.OnClicked_Lambda([Path]()
				{
					UObject* Asset = LoadObject<UMCPObservationProfile>(nullptr, *Path);
					if (Asset)
					{
						GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAsset(Asset);
					}
					return FReply::Handled();
				})
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(2, 0)
			[
				SNew(SButton)
				.Text(FText::FromString(TEXT("Delete")))
				.OnClicked_Lambda([this, Path]()
				{
					UEditorAssetLibrary::DeleteAsset(Path);
					ActiveProfilePaths.Remove(Path);
					RefreshProfiles();
					return FReply::Handled();
				})
			]
		];
	}

	if (Assets.Num() == 0)
	{
		ProfilesListBox->AddSlot().AutoHeight()
		[
			SNew(STextBlock)
			.Text(FText::FromString(TEXT("No profiles found")))
			.ColorAndOpacity(FSlateColor(FLinearColor::Gray))
		];
	}
}

TSharedRef<SWidget> SMCPPIEPanel::BuildLiveDebugSection()
{
	return SNew(SExpandableArea)
		.InitiallyCollapsed(true)
		.OnAreaExpansionChanged_Lambda([this](bool bExpanded) { bLiveDebugExpanded = bExpanded; })
		.HeaderContent()
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Font(FAppStyle::GetFontStyle("BoldFont"))
				.Text(FText::FromString(TEXT("Live Debug")))
			]
			+ SHorizontalBox::Slot().FillWidth(1.f)
			+ SHorizontalBox::Slot().AutoWidth().Padding(4, 0)
			[
				SNew(STextBlock)
				.Text_Lambda([]() -> FText
				{
					const bool bRep = UE_PIE_Automation::FPIEInputReplayer::Get().IsActive();
					const bool bObs = UE_PIE_Automation::FPIEObserver::Get().IsActive();
					return FText::FromString((bRep || bObs) ? TEXT("Active") : TEXT("Idle"));
				})
				.ColorAndOpacity_Lambda([]() -> FSlateColor
				{
					const bool bRep = UE_PIE_Automation::FPIEInputReplayer::Get().IsActive();
					const bool bObs = UE_PIE_Automation::FPIEObserver::Get().IsActive();
					return (bRep || bObs) ? FSlateColor(FLinearColor::Green) : FSlateColor(FSlateColor::UseForeground());
				})
			]
		]
		.BodyContent()
		[
			SAssignNew(LiveDebugContent, SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(STextBlock)
				.Text(FText::FromString(TEXT("No active replay or observation")))
				.ColorAndOpacity(FSlateColor(FLinearColor::Gray))
			]
		];
}

namespace
{
	FSlateColor DeltaColor(bool bChanged)
	{
		return bChanged
			? FSlateColor(FLinearColor(1.0f, 0.8f, 0.0f))
			: FSlateColor(FSlateColor::UseForeground());
	}

	void AddPropertyRow(SVerticalBox& Box, const FString& Label, const FString& Value, bool bChanged)
	{
		Box.AddSlot().AutoHeight().Padding(0, 1)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0)
			[
				SNew(STextBlock)
				.Text(FText::FromString(Label))
				.MinDesiredWidth(140.f)
				.ColorAndOpacity(FSlateColor(FLinearColor(0.6f, 0.6f, 0.6f)))
			]
			+ SHorizontalBox::Slot().FillWidth(1.f)
			[
				SNew(STextBlock)
				.Text(FText::FromString(Value))
				.ColorAndOpacity(DeltaColor(bChanged))
			]
		];
	}

	FString VecStr(const FVector& V)
	{
		return FString::Printf(TEXT("%.1f, %.1f, %.1f"), V.X, V.Y, V.Z);
	}

	FString RotStr(const FRotator& R)
	{
		return FString::Printf(TEXT("Y:%.1f P:%.1f R:%.1f"), R.Yaw, R.Pitch, R.Roll);
	}
}

void SMCPPIEPanel::RefreshLiveDebug()
{
	if (!LiveDebugContent.IsValid()) return;
	LiveDebugContent->ClearChildren();

	// Replay status
	const auto Rep = UE_PIE_Automation::FPIEInputReplayer::Get().GetLiveSnapshot();
	if (Rep.State == UE_PIE_Automation::EReplayerState::Replaying)
	{
		LiveDebugContent->AddSlot().AutoHeight().Padding(0, 0, 0, 4)
		[
			SNew(STextBlock)
			.Font(FAppStyle::GetFontStyle("BoldFont"))
			.Text(FText::FromString(FString::Printf(TEXT("Replay: %s  Step %d/%d  %.1fs"),
				*Rep.SourceRecordingId, Rep.CurrentStep, Rep.TotalSteps, Rep.ElapsedSeconds)))
		];

		// Drift summary
		LiveDebugContent->AddSlot().AutoHeight().Padding(8, 0, 0, 2)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(STextBlock)
				.Text(FText::FromString(FString::Printf(
					TEXT("Drift — Pos: %.1f cm  Vel: %.1f cm/s  Rot: %.1f°  Montage: %d  Frames: %d"),
					Rep.MaxPositionDriftCm, Rep.MaxVelocityDriftCms, Rep.MaxRotationDriftDeg,
					Rep.MontageMismatches, Rep.FramesCompared)))
				.ColorAndOpacity(FSlateColor(Rep.MaxPositionDriftCm > 50.f
					? FLinearColor(1.f, 0.3f, 0.3f)
					: FLinearColor(0.7f, 0.7f, 0.7f)))
			]
		];

		if (Rep.MaxTrackedDeltas.Num() > 0)
		{
			TSharedRef<SVerticalBox> TrackedDriftBox = SNew(SVerticalBox);
			for (const auto& KV : Rep.MaxTrackedDeltas)
			{
				AddPropertyRow(*TrackedDriftBox,
					FString::Printf(TEXT("  %s"), *KV.Key),
					FString::Printf(TEXT("Δ %.4f"), KV.Value),
					KV.Value > 0.01f);
			}
			LiveDebugContent->AddSlot().AutoHeight().Padding(8, 0, 0, 4)
			[
				TrackedDriftBox
			];
		}
	}

	// Observation profiles
	TArray<UE_PIE_Automation::FLiveObservationSnapshot> Snaps = UE_PIE_Automation::FPIEObserver::Get().GetLiveSnapshots();
	for (const auto& Snap : Snaps)
	{
		LiveDebugContent->AddSlot().AutoHeight().Padding(0, 4, 0, 0)
		[
			SNew(STextBlock)
			.Font(FAppStyle::GetFontStyle("BoldFont"))
			.Text(FText::FromString(FString::Printf(TEXT("Profile: %s  F:%d  %.1fs"),
				*Snap.ProfileName, Snap.FramesSampled, Snap.ElapsedSeconds)))
		];

		TSharedRef<SVerticalBox> PropsBox = SNew(SVerticalBox);

		// Pawn state
		const auto& Cur = Snap.LastRow;
		const auto& Prev = Snap.PrevRow;

		AddPropertyRow(*PropsBox, TEXT("Position"), VecStr(Cur.PawnLocation),
			!(Cur.PawnLocation - Prev.PawnLocation).IsNearlyZero(0.1));
		AddPropertyRow(*PropsBox, TEXT("Rotation"), RotStr(Cur.PawnRotation),
			!(Cur.PawnRotation - Prev.PawnRotation).IsNearlyZero(0.1));
		AddPropertyRow(*PropsBox, TEXT("Velocity"), VecStr(Cur.PawnVelocity),
			!(Cur.PawnVelocity - Prev.PawnVelocity).IsNearlyZero(0.1));
		AddPropertyRow(*PropsBox, TEXT("Speed2D"),
			FString::Printf(TEXT("%.1f"), Cur.Speed2D),
			!FMath::IsNearlyEqual(Cur.Speed2D, Prev.Speed2D, 0.1f));

		if (!Cur.MontageSection.IsEmpty())
		{
			AddPropertyRow(*PropsBox, TEXT("Montage"), Cur.MontageSection,
				Cur.MontageSection != Prev.MontageSection);
		}

		// Tracked values
		for (const auto& KV : Cur.TrackedValues)
		{
			const double* PrevVal = Prev.TrackedValues.Find(KV.Key);
			const bool bChanged = !PrevVal || !FMath::IsNearlyEqual(KV.Value, *PrevVal, 0.0001);
			AddPropertyRow(*PropsBox, KV.Key,
				FString::Printf(TEXT("%.4f"), KV.Value), bChanged);
		}

		// Tracked actors
		for (const auto& KV : Snap.LastActorRow.Actors)
		{
			if (!KV.Value.bResolved) continue;
			AddPropertyRow(*PropsBox,
				FString::Printf(TEXT("[%s] Pos"), *KV.Key),
				VecStr(KV.Value.Location), true);
			AddPropertyRow(*PropsBox,
				FString::Printf(TEXT("[%s] Rot"), *KV.Key),
				RotStr(KV.Value.Rotation), true);
		}

		LiveDebugContent->AddSlot().AutoHeight().Padding(8, 0, 0, 0)
		[
			PropsBox
		];
	}

	if (Rep.State != UE_PIE_Automation::EReplayerState::Replaying && Snaps.Num() == 0)
	{
		LiveDebugContent->AddSlot().AutoHeight()
		[
			SNew(STextBlock)
			.Text(FText::FromString(TEXT("Waiting for data...")))
			.ColorAndOpacity(FSlateColor(FLinearColor::Gray))
		];
	}
}
