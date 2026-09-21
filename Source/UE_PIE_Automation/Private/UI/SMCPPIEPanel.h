#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"

class FExtender;

class SMCPPIEPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SMCPPIEPanel) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	~SMCPPIEPanel() override;

	static void RegisterTab();
	static void UnregisterTab();
	static void OpenTab();
	static void RegisterToolbarButton();
	static void UnregisterToolbarButton();

	static const FName TabId;

private:
	static TSharedPtr<FExtender> ToolbarExtender;

private:
	virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override;

	TSharedRef<SWidget> BuildRecorderSection();
	TSharedRef<SWidget> BuildRecordingsSection();
	TSharedRef<SWidget> BuildProfilesSection();
	TSharedRef<SWidget> BuildLiveDebugSection();

	void ApplyTimeScale(float Scale);

	void RefreshRecordings();
	void RefreshProfiles();
	void RefreshLiveDebug();
	void OnEndPIE(bool bIsSimulating);
	void RenameRecording(const FString& RecordingId);

	TSharedPtr<STextBlock> RecorderStateText;

	// Recordings list
	TSharedPtr<SVerticalBox> RecordingsListBox;
	TArray<FString> CachedRecordingIds;
	int32 CaptureFPS = 15;
	int32 CaptureResolutionPercent = 50;

	// Profiles list
	TSharedPtr<SVerticalBox> ProfilesListBox;
	TArray<FString> CachedProfilePaths;
	TSet<FString> ActiveProfilePaths;
	FDelegateHandle EndPIEHandle;

	// Time scale
	float CurrentTimeScale = 1.0f;

	// Live debug
	TSharedPtr<SVerticalBox> LiveDebugContent;
	bool bLiveDebugExpanded = false;
	int32 LiveDebugTickCounter = 0;
};
