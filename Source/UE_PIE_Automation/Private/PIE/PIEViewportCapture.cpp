#include "PIEViewportCapture.h"
#include "UE_PIE_AutomationModule.h"
#include "RenderGraphBuilder.h"
#include "RHICommandList.h"
#include "ImageUtils.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Modules/ModuleManager.h"
#include "Misc/FileHelper.h"
#include "Async/Async.h"
#include "Engine/GameViewportClient.h"
#include "Engine/Engine.h"
#include "UnrealClient.h"

namespace UE_PIE_Automation
{

// Encode a BGRA FColor buffer to disk as JPEG or PNG. Safe on a background
// thread: IImageWrapper is fine once the ImageWrapper module is loaded (it is a
// module dependency, loaded at startup). PNG is lossless; map the user-facing
// quality value to a zlib level so 80 means level 7 without changing pixels.
static bool EncodeColorsToFile(const TArray<FColor>& Pixels, int32 W, int32 H, bool bJpeg, int32 Quality, const FString& Path)
{
	IImageWrapperModule* IWM = FModuleManager::Get().GetModulePtr<IImageWrapperModule>(TEXT("ImageWrapper"));
	if (IWM)
	{
		const EImageFormat Format = bJpeg ? EImageFormat::JPEG : EImageFormat::PNG;
		TSharedPtr<IImageWrapper> Wrapper = IWM->CreateImageWrapper(Format);
		if (Wrapper.IsValid() &&
			Wrapper->SetRaw(Pixels.GetData(), static_cast<int64>(Pixels.Num()) * sizeof(FColor), W, H, ERGBFormat::BGRA, 8))
		{
			const int32 Compression = bJpeg
				? FMath::Clamp(Quality, 1, 100)
				: -FMath::Clamp(FMath::RoundToInt(Quality * 9.0f / 100.0f), 1, 9);
			const TArray64<uint8>& Data = Wrapper->GetCompressed(Compression);
			if (Data.Num() > 0)
			{
				return FFileHelper::SaveArrayToFile(Data, *Path);
			}
		}
	}
	// Keep the old PNG fallback for environments where the wrapper cannot encode.
	TArray64<uint8> PNG;
	FImageUtils::PNGCompressImageArray(W, H, Pixels, PNG);
	return FFileHelper::SaveArrayToFile(PNG, *Path);
}

FPIEViewportCapture::FPIEViewportCapture(const FAutoRegister& AutoReg)
	: FSceneViewExtensionBase(AutoReg)
{
}

FPIEViewportCapture::~FPIEViewportCapture()
{
}

bool FPIEViewportCapture::IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const
{
	if (!bEnabled.load(std::memory_order_acquire)) return false;
	if (!GEngine || !GEngine->GameViewport) return false;
	return Context.Viewport == GEngine->GameViewport->Viewport;
}

void FPIEViewportCapture::SetEnabled(bool bEnable)
{
	bEnabled.store(bEnable, std::memory_order_release);
	if (!bEnable)
	{
		{
			FScopeLock SL(&Lock);
			PendingPath.Reset();
		}
		// Wait for all captured images to finish encoding before teardown.
		FlushPending();
	}
}

void FPIEViewportCapture::RequestCapture(const FString& OutputPath)
{
	FScopeLock SL(&Lock);
	PendingPath = OutputPath;
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

int32 FPIEViewportCapture::GetCapturedCount() const
{
	return CapturedCount.load(std::memory_order_acquire);
}

void FPIEViewportCapture::FlushPending()
{
	// Ensure PostRenderViewFamily_RenderThread has finished appending futures.
	FlushRenderingCommands();
	for (TFuture<bool>& Write : PendingWrites)
	{
		if (!Write.Get())
		{
			UE_LOG(LogUE_PIE_Automation, Warning, TEXT("[CAPTURE] Image write failed"));
		}
	}
	PendingWrites.Reset();
}

void FPIEViewportCapture::PostRenderViewFamily_RenderThread(
	FRDGBuilder& GraphBuilder, FSceneViewFamily& InViewFamily)
{
	FString Path;
	{
		FScopeLock SL(&Lock);
		if (PendingPath.IsEmpty()) return;
		Path = PendingPath;
		PendingPath.Reset();
	}

	const FRenderTarget* RT = InViewFamily.RenderTarget;
	if (!RT) return;

	FTextureRHIRef Texture = RT->GetRenderTargetTexture();
	if (!Texture.IsValid()) return;

	const FIntPoint Size = RT->GetSizeXY();
	if (Size.X <= 0 || Size.Y <= 0) return;

	// ReadSurfaceData converts HDR/float render targets to display-ready FColor.
	// The previous native-format GPU readback reinterpreted PF_FloatRGBA bytes as
	// BGRA8, producing the psychedelic frames seen in captured PIE images.
	TArray<FColor> Pixels;
	const FReadSurfaceDataFlags ReadFlags(RCM_UNorm, CubeFace_MAX);
	GraphBuilder.RHICmdList.ReadSurfaceData(
		Texture.GetReference(), FIntRect(0, 0, Size.X, Size.Y), Pixels, ReadFlags);
	if (Pixels.Num() != Size.X * Size.Y)
	{
		UE_LOG(LogUE_PIE_Automation, Warning,
			TEXT("[CAPTURE] ReadSurfaceData returned %d pixels, expected %d for %dx%d"),
			Pixels.Num(), Size.X * Size.Y, Size.X, Size.Y);
		return;
	}

	const bool bJpeg = bUseJpeg.load(std::memory_order_acquire);
	const int32 Quality = JpegQuality.load(std::memory_order_acquire);
	const int32 ScalePercent = ResolutionPercent.load(std::memory_order_acquire);
	const int32 OutputW = FMath::Max(1, FMath::RoundToInt(Size.X * ScalePercent / 100.0f));
	const int32 OutputH = FMath::Max(1, FMath::RoundToInt(Size.Y * ScalePercent / 100.0f));
	CapturedCount.fetch_add(1, std::memory_order_relaxed);
	PendingWrites.Add(Async(EAsyncExecution::ThreadPool,
		[Pix = MoveTemp(Pixels), SourceW = Size.X, SourceH = Size.Y,
			W = OutputW, H = OutputH, Path, bJpeg, Quality]() mutable
		{
			if (W != SourceW || H != SourceH)
			{
				TArray<FColor> Resized;
				FImageUtils::ImageResize(SourceW, SourceH, Pix, W, H, Resized, false, true);
				return EncodeColorsToFile(Resized, W, H, bJpeg, Quality, Path);
			}
			return EncodeColorsToFile(Pix, W, H, bJpeg, Quality, Path);
		}));
}

} // namespace UE_PIE_Automation
