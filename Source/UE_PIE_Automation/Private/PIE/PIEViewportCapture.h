#pragma once

#include "CoreMinimal.h"
#include "SceneViewExtension.h"
#include "Async/Future.h"
#include "HAL/CriticalSection.h"
#include <atomic>

namespace UE_PIE_Automation
{

class FPIEViewportCapture : public FSceneViewExtensionBase
{
public:
	FPIEViewportCapture(const FAutoRegister& AutoReg);
	virtual ~FPIEViewportCapture() override;

	virtual void SetupViewFamily(FSceneViewFamily& InViewFamily) override {}
	virtual void SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView) override {}
	virtual void BeginRenderViewFamily(FSceneViewFamily& InViewFamily) override {}
	virtual void PostRenderViewFamily_RenderThread(FRDGBuilder& GraphBuilder, FSceneViewFamily& InViewFamily) override;
	virtual bool IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const override;

	void SetEnabled(bool bEnable);
	void RequestCapture(const FString& OutputPath);
	int32 GetCapturedCount() const;

	// Output encoding for subsequent captures. The path extension passed to
	// RequestCapture should match. PNG is lossless; its quality value maps to a
	// zlib compression level rather than changing image fidelity.
	void SetOutputFormat(bool bInUseJpeg, int32 InQuality);
	void SetResolutionPercent(int32 InPercent);

	// Wait for all queued image writes. Call from the game thread at teardown
	// before generating a contact sheet or GIF.
	void FlushPending();

private:
	std::atomic<bool> bEnabled{false};
	std::atomic<bool> bUseJpeg{true};
	std::atomic<int32> JpegQuality{80};
	std::atomic<int32> ResolutionPercent{100};
	mutable FCriticalSection Lock;
	FString PendingPath;
	// Futures are appended on the render thread and joined by FlushPending after
	// FlushRenderingCommands() has stopped further render-thread mutations.
	TArray<TFuture<bool>> PendingWrites;
	std::atomic<int32> CapturedCount{0};
};

} // namespace UE_PIE_Automation
