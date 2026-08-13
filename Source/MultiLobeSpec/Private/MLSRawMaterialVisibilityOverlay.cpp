#include "MLSRawMaterialVisibilityOverlay.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
	static const TCHAR* MLSConfigInclude = TEXT("#include \"/Engine/Private/MultiLobeSpecConfig.ush\"");
	static const TCHAR* MLSGBufferMarker = TEXT("MLS_GBUFFER_RAW_VISIBILITY_TRANSPORT");

	void NormalizeLineEndings(FString& Source)
	{
		Source.ReplaceInline(TEXT("\r\n"), TEXT("\n"), ESearchCase::CaseSensitive);
		Source.ReplaceInline(TEXT("\r"), TEXT("\n"), ESearchCase::CaseSensitive);
	}

	int32 CountExact(const FString& Source, const FString& Needle)
	{
		if (Needle.IsEmpty())
		{
			return 0;
		}

		int32 Count = 0;
		int32 From = 0;
		while (true)
		{
			const int32 Position = Source.Find(
				Needle,
				ESearchCase::CaseSensitive,
				ESearchDir::FromStart,
				From);
			if (Position == INDEX_NONE)
			{
				break;
			}
			++Count;
			From = Position + Needle.Len();
		}
		return Count;
	}

	bool SaveAtomic(const FString& Path, const FString& Source, FString& OutError)
	{
		const FString TemporaryPath = Path + TEXT(".mls.tmp");
		IFileManager::Get().Delete(*TemporaryPath, false, true, true);
		if (!FFileHelper::SaveStringToFile(
			Source,
			*TemporaryPath,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			OutError = FString::Printf(TEXT("Unable to write temporary shader file: %s"), *TemporaryPath);
			return false;
		}
		if (!IFileManager::Get().Move(*Path, *TemporaryPath, true, true, false, true))
		{
			IFileManager::Get().Delete(*TemporaryPath, false, true, true);
			OutError = FString::Printf(TEXT("Unable to commit shader file: %s"), *Path);
			return false;
		}
		return true;
	}

	void EnsureConfigInclude(FString& Source)
	{
		if (Source.Contains(MLSConfigInclude))
		{
			return;
		}

		const int32 PragmaPosition = Source.Find(TEXT("#pragma once"), ESearchCase::CaseSensitive);
		if (PragmaPosition == INDEX_NONE)
		{
			Source.InsertAt(0, FString::Printf(TEXT("%s\n"), MLSConfigInclude));
			return;
		}

		int32 LineEnd = PragmaPosition;
		while (LineEnd < Source.Len() && Source[LineEnd] != '\n')
		{
			++LineEnd;
		}
		Source.InsertAt(LineEnd, FString::Printf(TEXT("\n%s"), MLSConfigInclude));
	}
}

bool FMLSRawMaterialVisibilityOverlay::PatchBasePassGuard(
	const FString& OverlayDir,
	FString& OutError)
{
	const FString Path = OverlayDir / TEXT("Private/BasePassPixelShader.usf");
	FString Source;
	if (!FFileHelper::LoadFileToString(Source, *Path))
	{
		OutError = FString::Printf(TEXT("Unable to load BasePassPixelShader.usf: %s"), *Path);
		return false;
	}
	NormalizeLineEndings(Source);

	const FString OldGuard =
		TEXT("#if GBUFFER_HAS_DIFFUSE_SAMPLE_OCCLUSION || ALLOW_STATIC_LIGHTING || SUBSTRATE_ENABLED");
	const FString NewGuard =
		TEXT("#if ALLOW_STATIC_LIGHTING || SUBSTRATE_ENABLED // MLS_SAMPLE_OCCLUSION_TRANSPORT_SUPPORTED");

	if (Source.Contains(NewGuard))
	{
		return true;
	}
	if (CountExact(Source, OldGuard) != 1)
	{
		OutError = TEXT("Raw MaterialAO BasePass guard exact-count failure; expected one generated transport guard.");
		return false;
	}

	Source.ReplaceInline(*OldGuard, *NewGuard, ESearchCase::CaseSensitive);
	return SaveAtomic(Path, Source, OutError);
}

bool FMLSRawMaterialVisibilityOverlay::PatchGBufferHelpers(
	const FString& OverlayDir,
	FString& OutError)
{
	const FString Path = OverlayDir / TEXT("Private/GBufferHelpers.ush");
	FString Source;
	if (!FFileHelper::LoadFileToString(Source, *Path))
	{
		OutError = FString::Printf(TEXT("Unable to load GBufferHelpers.ush: %s"), *Path);
		return false;
	}
	NormalizeLineEndings(Source);

	if (Source.Contains(MLSGBufferMarker))
	{
		return true;
	}

	EnsureConfigInclude(Source);

	const FString EncodeAnchor =
		TEXT("\t\tGBuffer.GenericAO = float(GBuffer.DiffuseIndirectSampleOcclusion) * (1.0f / 255.0f);");
	const FString EncodeReplacement =
		TEXT("#if MLS_ENABLED && MLS_RAW_MATERIAL_VISIBILITY_TRANSPORT\n")
		TEXT("\t\tGBuffer.GenericAO = GBuffer.GBufferAO; // MLS_GBUFFER_RAW_VISIBILITY_TRANSPORT\n")
		TEXT("#else\n")
		+ EncodeAnchor
		+ TEXT("\n#endif");

	const FString DecodeAnchor =
		TEXT("\tRet.DiffuseIndirectSampleOcclusion = 255 * Ret.GenericAO;\n")
		TEXT("\tRet.GBufferAO = saturate(1.0 - float(countbits(Ret.DiffuseIndirectSampleOcclusion)) * rcp(float(INDIRECT_SAMPLE_COUNT)));\n")
		TEXT("\tRet.IndirectIrradiance = 1;");
	const FString DecodeReplacement =
		TEXT("#if MLS_ENABLED && MLS_RAW_MATERIAL_VISIBILITY_TRANSPORT\n")
		TEXT("\t// GenericAO is repurposed as continuous material visibility while the MLS micro-shadow overlay is active.\n")
		TEXT("\tRet.DiffuseIndirectSampleOcclusion = 0x0;\n")
		TEXT("\tRet.GBufferAO = Ret.GenericAO;\n")
		TEXT("\tRet.IndirectIrradiance = 1;\n")
		TEXT("#else\n")
		+ DecodeAnchor
		+ TEXT("\n#endif");

	const int32 EncodeCount = CountExact(Source, EncodeAnchor);
	const int32 DecodeCount = CountExact(Source, DecodeAnchor);
	if (EncodeCount != 1 || DecodeCount != 1)
	{
		OutError = FString::Printf(
			TEXT("GBuffer raw-visibility exact-count failure: encode expected 1/found %d, decode expected 1/found %d."),
			EncodeCount,
			DecodeCount);
		return false;
	}

	Source.ReplaceInline(*EncodeAnchor, *EncodeReplacement, ESearchCase::CaseSensitive);
	Source.ReplaceInline(*DecodeAnchor, *DecodeReplacement, ESearchCase::CaseSensitive);
	return SaveAtomic(Path, Source, OutError);
}

void FMLSRawMaterialVisibilityOverlay::UpdateCapabilityReceipt(const FString& OverlayDir)
{
	const FString Path = OverlayDir / TEXT("MLS_CAPABILITIES.json");
	FString Json;
	if (!FFileHelper::LoadFileToString(Json, *Path))
	{
		return;
	}

	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		return;
	}

	int32 DiffuseSampleOcclusion = -1;
	if (const IConsoleVariable* Variable =
		IConsoleManager::Get().FindConsoleVariable(TEXT("r.GBufferDiffuseSampleOcclusion")))
	{
		DiffuseSampleOcclusion = Variable->GetInt();
	}

	Root->SetBoolField(TEXT("LegacyRawMaterialVisibility_OverlayPrerequisitesMet"), true);
	Root->SetBoolField(TEXT("LegacyRawMaterialVisibility_TransportPatched"), true);
	Root->SetNumberField(TEXT("LegacyRawMaterialVisibility_DiffuseSampleOcclusionCVar"), DiffuseSampleOcclusion);
	Root->SetStringField(
		TEXT("LegacyRawMaterialVisibility_TransportMode"),
		TEXT("Overlay-only GenericAO transport; diffuse-indirect sample mask is disabled while direct micro-shadowing is active"));
	Root->SetBoolField(TEXT("ActivisionDirectMicroShadow_DefaultLit"), true);
	Root->SetBoolField(TEXT("EngineForkRequired"), false);

	FString Output;
	const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Output);
	if (!FJsonSerializer::Serialize(Root.ToSharedRef(), Writer))
	{
		return;
	}
	Output += TEXT("\n");

	FString IgnoredError;
	SaveAtomic(Path, Output, IgnoredError);
}

bool FMLSRawMaterialVisibilityOverlay::Patch(
	const FString& OverlayDir,
	FString& OutError)
{
	if (!PatchBasePassGuard(OverlayDir, OutError))
	{
		return false;
	}
	if (!PatchGBufferHelpers(OverlayDir, OutError))
	{
		return false;
	}

	UpdateCapabilityReceipt(OverlayDir);
	return true;
}
