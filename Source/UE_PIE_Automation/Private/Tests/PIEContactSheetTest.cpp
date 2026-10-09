// Automation coverage for ComposeContactSheet (roadmap item 1b).
// Writes a few solid-colour PNG frames, composes a contact sheet, and asserts a
// decodable JPEG lands on disk.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "PIE/PIEContactSheet.h"
#include "ImageUtils.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Modules/ModuleManager.h"
#include "Misc/Paths.h"
#include "Misc/Guid.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPIEContactSheetComposeTest,
	"UE_PIE_Automation.ContactSheet.ComposesDecodableJpeg",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPIEContactSheetComposeTest::RunTest(const FString& /*Parameters*/)
{
	using namespace UE_PIE_Automation;

	const FString Dir = FPaths::ProjectSavedDir() / TEXT("MCPContactSheetTest") /
		FGuid::NewGuid().ToString(EGuidFormats::Digits);
	IFileManager::Get().MakeDirectory(*Dir, true);

	const int32 W = 64, H = 48;
	const FColor Colors[3] = { FColor(200, 40, 40), FColor(40, 200, 40), FColor(40, 40, 200) };
	TArray<FString> Frames;
	for (int32 i = 0; i < 3; ++i)
	{
		TArray<FColor> Pixels;
		Pixels.Init(Colors[i], W * H);
		TArray64<uint8> PNG;
		FImageUtils::PNGCompressImageArray(W, H, Pixels, PNG);
		const FString Path = Dir / FString::Printf(TEXT("frame_%05d.png"), i);
		TestTrue(TEXT("frame written"), FFileHelper::SaveArrayToFile(PNG, *Path));
		Frames.Add(Path);
	}

	const TArray<FString> Labels = { TEXT("F0"), TEXT("F1"), TEXT("F2") };
	const FString OutPath = Dir / TEXT("contact.jpg");

	FContactSheetParams CP;
	CP.CellWidth = 120;
	const bool bOk = ComposeContactSheet(Frames, OutPath, CP, &Labels);
	TestTrue(TEXT("ComposeContactSheet succeeded"), bOk);

	// The output must be a decodable JPEG with positive dimensions.
	TArray<uint8> Raw;
	TestTrue(TEXT("contact sheet file present"), FFileHelper::LoadFileToArray(Raw, *OutPath));
	if (Raw.Num() > 0)
	{
		IImageWrapperModule& IWM = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
		const EImageFormat Fmt = IWM.DetectImageFormat(Raw.GetData(), Raw.Num());
		TestEqual(TEXT("output is JPEG"), (int32)Fmt, (int32)EImageFormat::JPEG);
		TSharedPtr<IImageWrapper> Wrapper = IWM.CreateImageWrapper(Fmt);
		TestTrue(TEXT("decodes"), Wrapper.IsValid() && Wrapper->SetCompressed(Raw.GetData(), Raw.Num()));
		if (Wrapper.IsValid())
		{
			TestTrue(TEXT("has width"), Wrapper->GetWidth() > 0);
			TestTrue(TEXT("has height"), Wrapper->GetHeight() > 0);
		}
	}

	IFileManager::Get().DeleteDirectory(*Dir, false, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPIEPNGCompressionLosslessTest,
	"UE_PIE_Automation.Capture.PNGCompressionLossless",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPIEPNGCompressionLosslessTest::RunTest(const FString& /*Parameters*/)
{
	IImageWrapperModule& IWM = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
	const int32 W = 64, H = 48;
	TArray<FColor> Pixels;
	for (int32 Index = 0; Index < W * H; ++Index)
	{
		Pixels.Add(FColor(Index % 256, (Index * 7) % 256, (Index * 13) % 256, (Index * 17) % 256));
	}
	const int64 NumBytes = static_cast<int64>(Pixels.Num()) * sizeof(FColor);
	for (const int32 Compression : { -1, -7 })
	{
		TSharedPtr<IImageWrapper> Encoder = IWM.CreateImageWrapper(EImageFormat::PNG);
		if (!TestTrue(TEXT("accepts BGRA pixels"), Encoder.IsValid() &&
			Encoder->SetRaw(Pixels.GetData(), NumBytes, W, H, ERGBFormat::BGRA, 8))) return false;
		const TArray64<uint8> PNG = Encoder->GetCompressed(Compression);
		TSharedPtr<IImageWrapper> Decoder = IWM.CreateImageWrapper(EImageFormat::PNG);
		TArray64<uint8> Decoded;
		if (!TestTrue(TEXT("decodes PNG"), Decoder.IsValid() &&
			Decoder->SetCompressed(PNG.GetData(), PNG.Num()) && Decoder->GetRaw(ERGBFormat::BGRA, 8, Decoded))) return false;
		TestTrue(TEXT("compression preserves every BGRA byte"), Decoded.Num() == NumBytes &&
			FMemory::Memcmp(Decoded.GetData(), Pixels.GetData(), NumBytes) == 0);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
