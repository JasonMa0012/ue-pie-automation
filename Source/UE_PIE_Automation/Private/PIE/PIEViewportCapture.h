#pragma once

#include "CoreMinimal.h"
#include "Async/Future.h"
#include "HAL/CriticalSection.h"
#include "RHIGPUReadback.h"
#include <atomic>

class SWindow;
class FRHITexture;
class ISlateViewportProvider;

namespace UE_PIE_Automation
{
	class FPIEViewportCapture
	{
	public:
		FPIEViewportCapture() = default;
		~FPIEViewportCapture();

		void SetEnabled(bool bEnable);
		bool RequestCapture(const FString& OutputPath);
		int32 GetCapturedCount() const;
		int32 GetCompletedCount() const;
		int32 GetPendingCount() const;
		FString GetLastError() const;

		void SetOutputFormat(bool bInUseJpeg, int32 InQuality);
		void SetResolutionPercent(int32 InPercent);
		void FlushPending();

	private:
		struct FCaptureRequest
		{
			FString Path;
			double RequestedAt = 0.0;
		};

		struct FPendingReadback
		{
			FCaptureRequest Request;
			TUniquePtr<FRHIGPUTextureReadback> Readback;
			FIntPoint Size = FIntPoint::ZeroValue;
		};

		void OnSlatePreTick(float DeltaTime);
		void OnEndFrame();
		void OnBackBufferReadyToPresent(SWindow& Window, ISlateViewportProvider& ViewportProvider);
		void PumpCompletedCaptures();
		void FailRequest(const FCaptureRequest& Request, const FString& Reason);
		void AppendError(const FString& Reason);

		std::atomic<bool> bEnabled{false};
		std::atomic<int32> ResolutionPercent{100};
		std::atomic<int32> CapturedCount{0};
		std::atomic<int32> CompletedCount{0};
		std::atomic<int32> OutstandingCount{0};
		std::atomic<const SWindow*> TargetWindow{nullptr};
		std::atomic<bool> bHaveViewportRegion{false};
		FVector2D ViewportOriginInWindow = FVector2D::ZeroVector;
		FVector2D ViewportSizeInWindow = FVector2D::ZeroVector;
		mutable FCriticalSection Lock;
		TArray<FCaptureRequest> PendingRequests;
		TArray<FPendingReadback> PendingReadbacks;
		TArray<TFuture<TPair<FString, bool>>> PendingWrites;
		FString LastError;
		FDelegateHandle BackBufferHandle;
		FDelegateHandle SlatePreTickHandle;
		FDelegateHandle EndFrameHandle;
		std::atomic<bool> bUseJpeg{false};
		std::atomic<int32> JpegQuality{80};
	};
}
