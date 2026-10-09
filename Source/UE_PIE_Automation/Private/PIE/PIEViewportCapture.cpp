#include "PIEViewportCapture.h"
#include "UE_PIE_AutomationModule.h"
#include "Editor.h"
#include "Engine/GameViewportClient.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "ImageUtils.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Modules/ModuleManager.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#endif
#include "Misc/CoreDelegates.h"
#include "Misc/FileHelper.h"
#include "Layout/WidgetPath.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "Rendering/SlateRenderer.h"
#include "RenderingThread.h"
#include "RHIGPUReadback.h"
#include "RHICommandList.h"
#include "Slate/SceneViewport.h"
#include "ScreenPass.h"
#include "Layout/Geometry.h"
#include "RHIStaticStates.h"
#include "Slate/SlateViewportProvider.h"
#include "Widgets/SViewport.h"
#include "UnrealClient.h"

namespace UE_PIE_Automation
{
	namespace
	{
	constexpr int32 MaxInFlightCaptures = 3;
	constexpr double CaptureTimeoutSeconds = 30.0;

	bool MakeCaptureRect(FVector2D Origin, FVector2D Size, FIntPoint BufferSize, FIntRect& OutRect)
	{
		if (BufferSize.X <= 0 || BufferSize.Y <= 0 || Size.X <= 0.0 || Size.Y <= 0.0 ||
			!FMath::IsFinite(Origin.X) || !FMath::IsFinite(Origin.Y) || !FMath::IsFinite(Size.X) || !FMath::IsFinite(Size.Y)) return false;
		OutRect = FIntRect(
			FMath::Clamp(FMath::RoundToInt(Origin.X * BufferSize.X), 0, BufferSize.X),
			FMath::Clamp(FMath::RoundToInt(Origin.Y * BufferSize.Y), 0, BufferSize.Y),
			FMath::Clamp(FMath::RoundToInt((Origin.X + Size.X) * BufferSize.X), 0, BufferSize.X),
			FMath::Clamp(FMath::RoundToInt((Origin.Y + Size.Y) * BufferSize.Y), 0, BufferSize.Y));
		return !OutRect.IsEmpty();
	}

	bool CopyReadbackRows(const FColor* Source, int32 RowPitchPixels, FIntPoint Size, TArray<FColor>& OutPixels)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(PIECapture_CopyReadbackRows);
		if (!Source || RowPitchPixels < Size.X || Size.X <= 0 || Size.Y <= 0) return false;
		OutPixels.SetNumUninitialized(Size.X * Size.Y);
		for (int32 Y = 0; Y < Size.Y; ++Y)
		{
			FMemory::Memcpy(OutPixels.GetData() + Y * Size.X, Source + Y * RowPitchPixels, Size.X * sizeof(FColor));
		}
		return true;
	}

		bool EncodeColorsToFile(const TArray<FColor>& Pixels, int32 W, int32 H, bool bJpeg, int32 Quality, const FString& Path)
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(PIECapture_EncodeAndWrite);
			IImageWrapperModule* IWM = FModuleManager::Get().GetModulePtr<IImageWrapperModule>(TEXT("ImageWrapper"));
			if (IWM)
			{
				const EImageFormat Format = bJpeg ? EImageFormat::JPEG : EImageFormat::PNG;
				TSharedPtr<IImageWrapper> Wrapper = IWM->CreateImageWrapper(Format);
				if (Wrapper.IsValid() && Wrapper->SetRaw(Pixels.GetData(), static_cast<int64>(Pixels.Num()) * sizeof(FColor), W, H, ERGBFormat::BGRA, 8))
				{
					const int32 Compression = bJpeg
						? FMath::Clamp(Quality, 1, 100)
						: -1; // Fast lossless PNG compression keeps replay capture from backing up.
					TArray64<uint8> Data;
					{
						TRACE_CPUPROFILER_EVENT_SCOPE(PIECapture_Encode);
						Data = Wrapper->GetCompressed(Compression);
					}
					if (Data.Num() > 0)
					{
						TRACE_CPUPROFILER_EVENT_SCOPE(PIECapture_WriteFile);
						return FFileHelper::SaveArrayToFile(Data, *Path);
					}
				}
			}

			TArray64<uint8> PNG;
			FImageUtils::PNGCompressImageArray(W, H, Pixels, PNG);
			return FFileHelper::SaveArrayToFile(PNG, *Path);
		}
	}

	FPIEViewportCapture::~FPIEViewportCapture()
	{
		SetEnabled(false);
	}

	void FPIEViewportCapture::SetEnabled(bool bEnable)
	{
		check(IsInGameThread());
		if (bEnable)
		{
			if (bEnabled.exchange(true, std::memory_order_acq_rel)) return;
			if (!FSlateApplication::IsInitialized() || !FSlateApplication::Get().GetRenderer())
			{
				bEnabled.store(false, std::memory_order_release);
				AppendError(TEXT("Slate renderer is unavailable for BackBuffer capture"));
				return;
			}

			FSlateApplication& SlateApp = FSlateApplication::Get();
			BackBufferHandle = SlateApp.GetRenderer()->OnBackBufferReadyToPresent().AddRaw(this, &FPIEViewportCapture::OnBackBufferReadyToPresent);
			SlatePreTickHandle = SlateApp.OnPreTick().AddRaw(this, &FPIEViewportCapture::OnSlatePreTick);
			EndFrameHandle = FCoreDelegates::OnEndFrame.AddRaw(this, &FPIEViewportCapture::OnEndFrame);
			OnSlatePreTick(0.0f);
			return;
		}

		const bool bWasEnabled = bEnabled.exchange(false, std::memory_order_acq_rel);
		if (FSlateApplication::IsInitialized())
		{
			FSlateApplication& SlateApp = FSlateApplication::Get();
			if (FSlateRenderer* Renderer = SlateApp.GetRenderer())
			{
				if (BackBufferHandle.IsValid()) Renderer->OnBackBufferReadyToPresent().Remove(BackBufferHandle);
			}
			if (SlatePreTickHandle.IsValid()) SlateApp.OnPreTick().Remove(SlatePreTickHandle);
		}
		if (EndFrameHandle.IsValid()) FCoreDelegates::OnEndFrame.Remove(EndFrameHandle);
		BackBufferHandle.Reset();
		SlatePreTickHandle.Reset();
		EndFrameHandle.Reset();
		TargetWindow.store(nullptr, std::memory_order_release);
		bHaveViewportRegion.store(false, std::memory_order_release);

		if (bWasEnabled || OutstandingCount.load(std::memory_order_acquire) > 0)
		{
			FlushRenderingCommands();
			TArray<FCaptureRequest> Unsubmitted;
			{
				FScopeLock SL(&Lock);
				Unsubmitted = MoveTemp(PendingRequests);
				PendingRequests.Reset();
			}
			for (const FCaptureRequest& Request : Unsubmitted)
			{
				FailRequest(Request, TEXT("PIE ended before the requested BackBuffer was presented"));
			}
			FlushPending();
		}
	}

	bool FPIEViewportCapture::RequestCapture(const FString& OutputPath, bool bDropIfBusy)
	{
		check(IsInGameThread());
		if (!bEnabled.load(std::memory_order_acquire))
		{
			AppendError(TEXT("BackBuffer capture is not enabled"));
			return false;
		}
		if (bDropIfBusy && OutstandingCount.load(std::memory_order_acquire) >= MaxInFlightCaptures)
		{
			DroppedCount.fetch_add(1, std::memory_order_release);
			return false;
		}
		if (!bDropIfBusy)
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(PIECapture_QueueWait);
			const double Deadline = FPlatformTime::Seconds() + CaptureTimeoutSeconds;
			while (OutstandingCount.load(std::memory_order_acquire) >= MaxInFlightCaptures)
			{
				ENQUEUE_RENDER_COMMAND(PumpPIECaptureReadbacks)([this](FRHICommandListImmediate&)
				{
					PumpReadyReadbacks_RenderThread();
				});
				FlushRenderingCommands();
				PumpCompletedCaptures();
				if (OutstandingCount.load(std::memory_order_acquire) < MaxInFlightCaptures) break;
				if (FPlatformTime::Seconds() >= Deadline)
				{
					AppendError(TEXT("BackBuffer capture queue made no progress for 30 seconds"));
					return false;
				}
				FPlatformProcess::Sleep(0.001f);
			}
		}
		{
			FScopeLock SL(&Lock);
			PendingRequests.Add({OutputPath, FPlatformTime::Seconds()});
			OutstandingCount.fetch_add(1, std::memory_order_release);
		}
		return true;
	}

	int32 FPIEViewportCapture::GetCapturedCount() const
	{
		return CapturedCount.load(std::memory_order_acquire);
	}

	int32 FPIEViewportCapture::GetCompletedCount() const
	{
		return CompletedCount.load(std::memory_order_acquire);
	}

	int32 FPIEViewportCapture::GetPendingCount() const
	{
		return OutstandingCount.load(std::memory_order_acquire);
	}

	int32 FPIEViewportCapture::GetDroppedCount() const
	{
		return DroppedCount.load(std::memory_order_acquire);
	}

	FString FPIEViewportCapture::GetLastError() const
	{
		FScopeLock SL(&Lock);
		return LastError;
	}

	void FPIEViewportCapture::SetOutputFormat(bool bInUseJpeg, int32 InQuality)
	{
		bUseJpeg.store(bInUseJpeg, std::memory_order_release);
		JpegQuality.store(FMath::Clamp(InQuality, 1, 100), std::memory_order_release);
	}

	void FPIEViewportCapture::SetResolutionPercent(int32 InPercent)
	{
		ResolutionPercent.store(FMath::Clamp(InPercent, 1, 100), std::memory_order_release);
	}

	void FPIEViewportCapture::FlushPending()
	{
		check(IsInGameThread());
		const double Deadline = FPlatformTime::Seconds() + CaptureTimeoutSeconds;
		while (OutstandingCount.load(std::memory_order_acquire) > 0)
		{
			ENQUEUE_RENDER_COMMAND(DrainPIECaptureReadbacks)([this](FRHICommandListImmediate&)
			{
				PumpReadyReadbacks_RenderThread();
			});
			FlushRenderingCommands();
			PumpCompletedCaptures();
			if (OutstandingCount.load(std::memory_order_acquire) == 0) break;
			if (FPlatformTime::Seconds() >= Deadline)
			{
				AppendError(TEXT("Timed out while draining pending BackBuffer captures"));
				break;
			}
			FPlatformProcess::Sleep(0.001f);
		}
		PumpCompletedCaptures();
	}

	void FPIEViewportCapture::OnSlatePreTick(float /*DeltaTime*/)
	{
		FSlateApplication& SlateApp = FSlateApplication::Get();
		FViewport* PIEViewport = GEditor ? GEditor->GetPIEViewport() : nullptr;
		FSceneViewport* SceneViewport = PIEViewport ? PIEViewport->AsSceneViewport() : nullptr;
		TSharedPtr<SViewport> Viewport = SceneViewport ? SceneViewport->GetViewportWidget().Pin() : nullptr;
		FWidgetPath ViewportPath;
		TSharedPtr<SWindow> Window = Viewport.IsValid() &&
			SlateApp.GeneratePathToWidgetUnchecked(Viewport.ToSharedRef(), ViewportPath, EVisibility::All)
			? ViewportPath.TopLevelWindow
			: nullptr;
		if (!Viewport.IsValid() || !Window.IsValid() || !ViewportPath.IsValid())
		{
			TargetWindow.store(nullptr, std::memory_order_release);
			bHaveViewportRegion.store(false, std::memory_order_release);
			return;
		}

		// GeneratePathToWidget builds this path from GetWindowGeometryInScreen(), so
		// convert its screen-space widget geometry through the same root. Mixing
		// GetCachedGeometry() with window-local geometry offsets floating PIE crops.
		const FGeometry WindowGeometry = Window->GetWindowGeometryInScreen();
		const FGeometry& ViewportGeometry = ViewportPath.Widgets.Last().Geometry;
		const FVector2D WindowSize = WindowGeometry.GetLocalSize();
		const FVector2D WindowAbsoluteSize = WindowGeometry.GetAbsoluteSize();
		if (WindowSize.X <= 0.0 || WindowSize.Y <= 0.0 || WindowAbsoluteSize.X <= 0.0 || WindowAbsoluteSize.Y <= 0.0)
		{
			TargetWindow.store(nullptr, std::memory_order_release);
			bHaveViewportRegion.store(false, std::memory_order_release);
			return;
		}

		const FVector2D Origin = WindowGeometry.AbsoluteToLocal(ViewportGeometry.GetAbsolutePosition());
		const FVector2D Size = ViewportGeometry.GetAbsoluteSize();
		{
			FScopeLock SL(&Lock);
			ViewportOriginInWindow = Origin / WindowSize;
			ViewportSizeInWindow = Size / WindowAbsoluteSize;
		}
		TargetWindow.store(Window.Get(), std::memory_order_release);
		bHaveViewportRegion.store(true, std::memory_order_release);
	}

	void FPIEViewportCapture::OnEndFrame()
	{
		PumpCompletedCaptures();

		const double Now = FPlatformTime::Seconds();
		TArray<FCaptureRequest> TimedOut;
		{
			FScopeLock SL(&Lock);
			for (int32 Index = PendingRequests.Num() - 1; Index >= 0; --Index)
			{
				if (Now - PendingRequests[Index].RequestedAt >= CaptureTimeoutSeconds)
				{
					TimedOut.Add(MoveTemp(PendingRequests[Index]));
					PendingRequests.RemoveAtSwap(Index, 1, EAllowShrinking::No);
				}
			}
		}
		for (const FCaptureRequest& Request : TimedOut)
		{
			FailRequest(Request, TEXT("Target PIE window did not present a BackBuffer within 30 seconds"));
		}
	}

	void FPIEViewportCapture::OnBackBufferReadyToPresent(SWindow& Window, ISlateViewportProvider& ViewportProvider)
	{
		if (!bEnabled.load(std::memory_order_acquire) || TargetWindow.load(std::memory_order_acquire) != &Window) return;

		FCaptureRequest Request;
		FVector2D ViewportOrigin = FVector2D::ZeroVector;
		FVector2D ViewportSize = FVector2D::ZeroVector;
		bool bHasRequest = false;
		{
			FScopeLock SL(&Lock);
			if (!PendingRequests.IsEmpty())
			{
				Request = MoveTemp(PendingRequests[0]);
				PendingRequests.RemoveAt(0, 1, EAllowShrinking::No);
				ViewportOrigin = ViewportOriginInWindow;
				ViewportSize = ViewportSizeInWindow;
				bHasRequest = true;
			}
		}

		if (!bHasRequest)
		{
			PumpReadyReadbacks_RenderThread();
			return;
		}

		FRHITexture* BackBuffer = ViewportProvider.GetBackBufferResource();
		if (!BackBuffer || !bHaveViewportRegion.load(std::memory_order_acquire))
		{
			FailRequest(Request, TEXT("Target PIE BackBuffer or viewport geometry is unavailable"));
			return;
		}

		const FIntPoint BackBufferSize = BackBuffer->GetDesc().Extent;
		FIntRect SourceRect;
		if (!MakeCaptureRect(ViewportOrigin, ViewportSize, BackBufferSize, SourceRect))
		{
			FailRequest(Request, TEXT("PIE viewport crop is empty or outside the presented BackBuffer"));
			return;
		}

		const int32 Scale = ResolutionPercent.load(std::memory_order_acquire);
		const FIntPoint OutputSize(
			FMath::Max(1, FMath::RoundToInt(SourceRect.Width() * Scale / 100.0f)),
			FMath::Max(1, FMath::RoundToInt(SourceRect.Height() * Scale / 100.0f)));

		FRHICommandListImmediate& RHICmdList = FRHICommandListImmediate::Get();
		FRDGBuilder GraphBuilder(RHICmdList);
		FRDGTextureRef InputTexture = GraphBuilder.RegisterExternalTexture(
			CreateRenderTarget(BackBuffer, TEXT("PIEBackBufferCaptureInput")));
		const ETextureCreateFlags ColorSpaceFlags = BackBuffer->GetDesc().Flags & TexCreate_SRGB;
		FRDGTextureRef OutputTexture = GraphBuilder.CreateTexture(
			FRDGTextureDesc::Create2D(OutputSize, PF_B8G8R8A8, FClearValueBinding::None,
				TexCreate_RenderTargetable | TexCreate_ShaderResource | ColorSpaceFlags),
			TEXT("PIEBackBufferCaptureOutput"));

		FScreenPassViewInfo ViewInfo(GMaxRHIFeatureLevel);
		AddDrawTexturePass(GraphBuilder, ViewInfo, InputTexture, OutputTexture,
			SourceRect.Min, SourceRect.Size(), FIntPoint::ZeroValue, OutputSize,
			TStaticSamplerState<SF_Bilinear>::GetRHI());

		TUniquePtr<FRHIGPUTextureReadback> Readback = MakeUnique<FRHIGPUTextureReadback>(TEXT("PIEBackBufferCapture"));
		AddEnqueueCopyPass(GraphBuilder, Readback.Get(), OutputTexture);
		GraphBuilder.Execute();

		{
			FScopeLock SL(&Lock);
			PendingReadbacks.Add({MoveTemp(Request), MoveTemp(Readback), OutputSize});
		}
		PumpReadyReadbacks_RenderThread();
	}

	void FPIEViewportCapture::PumpReadyReadbacks_RenderThread()
	{
		check(IsInRenderingThread());
		{
			FScopeLock SL(&Lock);
			for (int32 Index = PendingReadbacks.Num() - 1; Index >= 0; --Index)
			{
				FPendingReadback& Pending = PendingReadbacks[Index];
				if (!Pending.Readback->IsReady()) continue;

				int32 RowPitchPixels = 0;
				const FColor* Source = static_cast<const FColor*>(Pending.Readback->Lock(RowPitchPixels));
				if (!Source || RowPitchPixels < Pending.Size.X)
				{
					if (Source) Pending.Readback->Unlock();
					const FCaptureRequest FailedRequest = Pending.Request;
					PendingReadbacks.RemoveAtSwap(Index, 1, EAllowShrinking::No);
					if (LastError.IsEmpty())
					{
						LastError = FString::Printf(TEXT("GPU readback returned an invalid row pitch for %s"), *FailedRequest.Path);
					}
					CompletedCount.fetch_add(1, std::memory_order_release);
					OutstandingCount.fetch_sub(1, std::memory_order_release);
					continue;
				}

				TArray<FColor> Pixels;
				if (!CopyReadbackRows(Source, RowPitchPixels, Pending.Size, Pixels))
				{
					Pending.Readback->Unlock();
					if (LastError.IsEmpty()) LastError = FString::Printf(TEXT("GPU readback returned an invalid row pitch for %s"), *Pending.Request.Path);
					PendingReadbacks.RemoveAtSwap(Index, 1, EAllowShrinking::No);
					CompletedCount.fetch_add(1, std::memory_order_release);
					OutstandingCount.fetch_sub(1, std::memory_order_release);
					continue;
				}

				FCaptureRequest Request = MoveTemp(Pending.Request);
				const FIntPoint Size = Pending.Size;
				Pending.Readback->Unlock();
				PendingReadbacks.RemoveAtSwap(Index, 1, EAllowShrinking::No);

				const bool bJpeg = bUseJpeg.load(std::memory_order_acquire);
				const int32 Quality = JpegQuality.load(std::memory_order_acquire);
				PendingWrites.Add(Async(EAsyncExecution::ThreadPool,
					[Request = MoveTemp(Request), Size, Pixels = MoveTemp(Pixels), bJpeg, Quality]() mutable
					{
						const bool bSaved = EncodeColorsToFile(Pixels, Size.X, Size.Y, bJpeg, Quality, Request.Path);
						return TPair<FString, bool>(Request.Path, bSaved);
					}));
			}
		}
	}

	void FPIEViewportCapture::PumpCompletedCaptures()
	{
		check(IsInGameThread());
		TArray<TFuture<TPair<FString, bool>>> ReadyWrites;
		{
			FScopeLock SL(&Lock);
			for (int32 Index = PendingWrites.Num() - 1; Index >= 0; --Index)
			{
				if (!PendingWrites[Index].IsReady()) continue;
				ReadyWrites.Add(MoveTemp(PendingWrites[Index]));
				PendingWrites.RemoveAtSwap(Index, 1, EAllowShrinking::No);
			}
		}
		for (TFuture<TPair<FString, bool>>& Write : ReadyWrites)
		{
			const TPair<FString, bool> Result = Write.Get();
			if (Result.Value)
			{
				CapturedCount.fetch_add(1, std::memory_order_release);
			}
			else
			{
				AppendError(FString::Printf(TEXT("Failed to encode or write captured frame: %s"), *Result.Key));
			}
			CompletedCount.fetch_add(1, std::memory_order_release);
			OutstandingCount.fetch_sub(1, std::memory_order_release);
		}
	}

	void FPIEViewportCapture::FailRequest(const FCaptureRequest& Request, const FString& Reason)
	{
		AppendError(FString::Printf(TEXT("%s (%s)"), *Reason, *Request.Path));
		CompletedCount.fetch_add(1, std::memory_order_release);
		OutstandingCount.fetch_sub(1, std::memory_order_release);
	}

	void FPIEViewportCapture::AppendError(const FString& Reason)
	{
		FScopeLock SL(&Lock);
		if (LastError.IsEmpty()) LastError = Reason;
	}
}

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPIEBackBufferCaptureDataTest,
	"UE_PIE_Automation.Capture.BackBufferCropAndRows",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPIEBackBufferCaptureDataTest::RunTest(const FString& /*Parameters*/)
{
	FIntRect Crop;
	TestTrue(TEXT("viewport region maps into the presented BackBuffer"),
		UE_PIE_Automation::MakeCaptureRect(FVector2D(0.25, 0.1), FVector2D(0.5, 0.8), FIntPoint(2000, 1000), Crop));
	TestTrue(TEXT("viewport is cropped in physical BackBuffer pixels"), Crop == FIntRect(500, 100, 1500, 900));

	TArray<FColor> PaddedRows;
	for (uint8 Index = 0; Index < 10; ++Index) PaddedRows.Add(FColor(Index, 0, 0, 255));
	TArray<FColor> Pixels;
	TestTrue(TEXT("GPU row-pitch padding is accepted"),
		UE_PIE_Automation::CopyReadbackRows(PaddedRows.GetData(), 5, FIntPoint(3, 2), Pixels));
	TestTrue(TEXT("padding is skipped between rows"), Pixels.Num() == 6 && Pixels[3].R == 5 && Pixels[5].R == 7);
	return true;
}
#endif
