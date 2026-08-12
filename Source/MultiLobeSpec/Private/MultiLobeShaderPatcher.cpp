#include "MultiLobeShaderPatcher.h"
#include "MultiLobeSpec.h"

#include "HAL/PlatformFileManager.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/EngineVersion.h"
#include "Misc/SecureHash.h"
#include "Interfaces/IPluginManager.h"
#include "Dom/JsonObject.h"
#include "HAL/IConsoleManager.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

static const TCHAR* MLS_ConfigFileName = TEXT("MultiLobeSpecConfig.ush");
static const TCHAR* MLS_AgXFileName    = TEXT("MLS_AgX.ush");
static const TCHAR* MLS_LumenFileName  = TEXT("MLS_LumenDualBlur.ush");
static const TCHAR* MLS_ConeEnvRuntimeFileName = TEXT("MLS_ConeEnvBRDFRuntime.ush");
static const TCHAR* MLS_LumenInclude   = TEXT("#include \"/Engine/Private/MLS_LumenDualBlur.ush\"");
static const TCHAR* MLS_ConeEnvRuntimeInclude = TEXT("#include \"/Engine/Private/MLS_ConeEnvBRDFRuntime.ush\"");
static const TCHAR* MLS_ConfigInclude  = TEXT("#include \"/Engine/Private/MultiLobeSpecConfig.ush\"");
static const TCHAR* MLS_AgXInclude     = TEXT("#include \"/Engine/Private/MLS_AgX.ush\"");
static const TCHAR* MLS_PatchMarker    = TEXT("// MLS_PATCHED");
static const TCHAR* MLS_StampFileName  = TEXT("MLS_STAMP.txt");
static const TCHAR* MLS_CloneSuffix    = TEXT("_MLS_Orig");
static const TCHAR* MLS_VNDFLUTFileName = TEXT("MLS_MicroShadowLUT.ush");
static const TCHAR* MLS_VNDFManifestFileName = TEXT("MLS_MicroShadowLUT.manifest.json");
static const TCHAR* MLS_VNDFValidationFileName = TEXT("MLS_MicroShadowLUT.validation.json");
static const TCHAR* MLS_ConeEnvLUTFileName = TEXT("MLS_ConeEnvBRDF.ush");
static const TCHAR* MLS_ConeEnvManifestFileName = TEXT("MLS_ConeEnvBRDF.manifest.json");

static const TCHAR* MLS_CandidateFiles[] =
{
	TEXT("Private/ShadingModels.ush"),
	TEXT("Private/BRDF.ush"),
	TEXT("Private/ShadingCommon.ush"),
	TEXT("Private/BxDF.ush"),
};

static bool MLS_GetVNDFArtifactPaths(FString& OutShaderInclude, FString& OutManifest)
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("MultiLobeSpec"));
	if (!Plugin.IsValid())
	{
		return false;
	}
	const FString GeneratedDir = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Resources"), TEXT("Generated"));
	OutShaderInclude = FPaths::Combine(GeneratedDir, MLS_VNDFLUTFileName);
	OutManifest = FPaths::Combine(GeneratedDir, MLS_VNDFManifestFileName);
	return true;
}

static bool MLS_GetVNDFValidationPath(FString& OutValidation)
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("MultiLobeSpec"));
	if (!Plugin.IsValid())
	{
		return false;
	}
	OutValidation = FPaths::Combine(
		Plugin->GetBaseDir(), TEXT("Resources"), TEXT("Generated"), MLS_VNDFValidationFileName);
	return true;
}

static bool MLS_GetConeEnvArtifactPaths(FString& OutShaderInclude, FString& OutManifest)
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("MultiLobeSpec"));
	if (!Plugin.IsValid())
	{
		return false;
	}
	const FString GeneratedDir = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Resources"), TEXT("Generated"));
	OutShaderInclude = FPaths::Combine(GeneratedDir, MLS_ConeEnvLUTFileName);
	OutManifest = FPaths::Combine(GeneratedDir, MLS_ConeEnvManifestFileName);
	return true;
}

static FString MLS_HashFileSHA1(const FString& Path)
{
	TArray<uint8> Bytes;
	return FFileHelper::LoadFileToArray(Bytes, *Path)
		? FSHA1::HashBuffer(Bytes.GetData(), static_cast<uint64>(Bytes.Num())).ToString().ToLower()
		: TEXT("missing");
}

static FString MLS_HashBytesSHA256(const TArray<uint8>& Bytes)
{
	// FPlatformMisc::GetSHA256Signature is deliberately unimplemented on some UE
	// desktop platforms. Keep receipt validation deterministic and platform-neutral.
	static constexpr uint32 RoundConstants[64] =
	{
		0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
		0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
		0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
		0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
		0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
		0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
		0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
		0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
		0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
		0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
		0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
		0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
		0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
		0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
		0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
		0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
	};

	auto RotateRight = [](uint32 Value, uint32 Bits)
	{
		return (Value >> Bits) | (Value << (32u - Bits));
	};
	auto ProcessBlock = [&RotateRight](const uint8* Block, uint32 State[8])
	{
		uint32 Schedule[64];
		for (int32 Index = 0; Index < 16; ++Index)
		{
			const uint8* Word = Block + Index * 4;
			Schedule[Index] = (static_cast<uint32>(Word[0]) << 24u)
				| (static_cast<uint32>(Word[1]) << 16u)
				| (static_cast<uint32>(Word[2]) << 8u)
				| static_cast<uint32>(Word[3]);
		}
		for (int32 Index = 16; Index < 64; ++Index)
		{
			const uint32 Sigma0 = RotateRight(Schedule[Index - 15], 7u)
				^ RotateRight(Schedule[Index - 15], 18u)
				^ (Schedule[Index - 15] >> 3u);
			const uint32 Sigma1 = RotateRight(Schedule[Index - 2], 17u)
				^ RotateRight(Schedule[Index - 2], 19u)
				^ (Schedule[Index - 2] >> 10u);
			Schedule[Index] = Schedule[Index - 16] + Sigma0
				+ Schedule[Index - 7] + Sigma1;
		}

		uint32 A = State[0];
		uint32 B = State[1];
		uint32 C = State[2];
		uint32 D = State[3];
		uint32 E = State[4];
		uint32 F = State[5];
		uint32 G = State[6];
		uint32 H = State[7];
		for (int32 Index = 0; Index < 64; ++Index)
		{
			const uint32 BigSigma1 = RotateRight(E, 6u) ^ RotateRight(E, 11u) ^ RotateRight(E, 25u);
			const uint32 Choice = (E & F) ^ (~E & G);
			const uint32 Temp1 = H + BigSigma1 + Choice + RoundConstants[Index] + Schedule[Index];
			const uint32 BigSigma0 = RotateRight(A, 2u) ^ RotateRight(A, 13u) ^ RotateRight(A, 22u);
			const uint32 Majority = (A & B) ^ (A & C) ^ (B & C);
			const uint32 Temp2 = BigSigma0 + Majority;
			H = G;
			G = F;
			F = E;
			E = D + Temp1;
			D = C;
			C = B;
			B = A;
			A = Temp1 + Temp2;
		}
		State[0] += A;
		State[1] += B;
		State[2] += C;
		State[3] += D;
		State[4] += E;
		State[5] += F;
		State[6] += G;
		State[7] += H;
	};

	uint32 State[8] =
	{
		0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
		0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
	};
	const int64 ByteCount = Bytes.Num();
	int64 Offset = 0;
	while (ByteCount - Offset >= 64)
	{
		ProcessBlock(Bytes.GetData() + Offset, State);
		Offset += 64;
	}

	uint8 Tail[128] = { 0 };
	const int32 Remainder = static_cast<int32>(ByteCount - Offset);
	if (Remainder > 0)
	{
		FMemory::Memcpy(Tail, Bytes.GetData() + Offset, Remainder);
	}
	Tail[Remainder] = 0x80u;
	const int32 TailBytes = Remainder <= 55 ? 64 : 128;
	const uint64 BitCount = static_cast<uint64>(ByteCount) * 8u;
	for (int32 Index = 0; Index < 8; ++Index)
	{
		Tail[TailBytes - 1 - Index] = static_cast<uint8>(BitCount >> (Index * 8));
	}
	ProcessBlock(Tail, State);
	if (TailBytes == 128)
	{
		ProcessBlock(Tail + 64, State);
	}

	uint8 Digest[32];
	for (int32 WordIndex = 0; WordIndex < 8; ++WordIndex)
	{
		Digest[WordIndex * 4] = static_cast<uint8>(State[WordIndex] >> 24u);
		Digest[WordIndex * 4 + 1] = static_cast<uint8>(State[WordIndex] >> 16u);
		Digest[WordIndex * 4 + 2] = static_cast<uint8>(State[WordIndex] >> 8u);
		Digest[WordIndex * 4 + 3] = static_cast<uint8>(State[WordIndex]);
	}
	FString Result;
	Result.Reserve(64);
	for (const uint8 Byte : Digest)
	{
		Result += FString::Printf(TEXT("%02x"), Byte);
	}
	return Result;
}

static FString MLS_HashFileSHA256(const FString& Path)
{
	TArray<uint8> Bytes;
	return FFileHelper::LoadFileToArray(Bytes, *Path)
		? MLS_HashBytesSHA256(Bytes)
		: TEXT("missing");
}

static bool MLS_LoadJsonObject(const FString& Path, TSharedPtr<FJsonObject>& OutObject)
{
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *Path))
	{
		return false;
	}
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
	return FJsonSerializer::Deserialize(Reader, OutObject) && OutObject.IsValid();
}

static bool MLS_ComputePackedVNDFPayloadSHA256(
	const FString& ShaderInclude,
	FString& OutSHA256,
	int32& OutTexelCount,
	FString& OutError)
{
	FString Source;
	if (!FFileHelper::LoadFileToString(Source, *ShaderInclude))
	{
		OutError = TEXT("unable to load packed LUT include");
		return false;
	}

	const int32 Marker = Source.Find(TEXT("static const uint MLS_VNDF_LUT_PACKED"));
	const int32 OpenBrace = Marker == INDEX_NONE
		? INDEX_NONE
		: Source.Find(TEXT("{"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Marker);
	const int32 CloseBrace = OpenBrace == INDEX_NONE
		? INDEX_NONE
		: Source.Find(TEXT("};"), ESearchCase::CaseSensitive, ESearchDir::FromStart, OpenBrace);
	if (OpenBrace == INDEX_NONE || CloseBrace == INDEX_NONE)
	{
		OutError = TEXT("packed LUT array bounds were not found");
		return false;
	}

	TArray<uint8> PackedBytes;
	int32 SearchFrom = OpenBrace + 1;
	OutTexelCount = 0;
	while (true)
	{
		const int32 HexPrefix = Source.Find(
			TEXT("0x"), ESearchCase::CaseSensitive, ESearchDir::FromStart, SearchFrom);
		if (HexPrefix == INDEX_NONE || HexPrefix >= CloseBrace)
		{
			break;
		}
		if (HexPrefix + 10 > CloseBrace)
		{
			OutError = TEXT("truncated packed LUT word");
			return false;
		}
		const FString Hex = Source.Mid(HexPrefix + 2, 8);
		for (const TCHAR Character : Hex)
		{
			if (!FChar::IsHexDigit(Character))
			{
				OutError = TEXT("invalid packed LUT hex word");
				return false;
			}
		}
		const uint32 Word = FParse::HexNumber(*Hex);
		PackedBytes.Add(static_cast<uint8>(Word & 0xffu));
		PackedBytes.Add(static_cast<uint8>((Word >> 8u) & 0xffu));
		PackedBytes.Add(static_cast<uint8>((Word >> 16u) & 0xffu));
		PackedBytes.Add(static_cast<uint8>((Word >> 24u) & 0xffu));
		++OutTexelCount;
		SearchFrom = HexPrefix + 10;
	}

	if (OutTexelCount == 0)
	{
		OutError = TEXT("packed LUT array is empty");
		return false;
	}
	OutSHA256 = MLS_HashBytesSHA256(PackedBytes);
	if (OutSHA256 == TEXT("unavailable"))
	{
		OutError = TEXT("SHA-256 implementation unavailable");
		return false;
	}
	return true;
}

static bool MLS_ValidateVNDFAdmission(
	const FString& ShaderInclude,
	const FString& ManifestPath,
	const FString& ValidationPath,
	FString& OutReason)
{
	if (!FPaths::FileExists(ShaderInclude)
		|| !FPaths::FileExists(ManifestPath)
		|| !FPaths::FileExists(ValidationPath))
	{
		OutReason = TEXT("include, manifest, or validation receipt is missing");
		return false;
	}

	TSharedPtr<FJsonObject> Manifest;
	TSharedPtr<FJsonObject> Receipt;
	if (!MLS_LoadJsonObject(ManifestPath, Manifest) || !MLS_LoadJsonObject(ValidationPath, Receipt))
	{
		OutReason = TEXT("manifest or validation receipt is not valid JSON");
		return false;
	}

	FString Schema;
	FString Algorithm;
	FString ManifestDataSHA;
	FString ReceiptDataSHA;
	FString ReceiptManifestSHA;
	FString ReceiptIncludeSHA;
	bool bPassed = false;
	bool bQualified = false;
	double ValidatorVersion = 0.0;
	double ExpectedTexels = 0.0;
	if (!Receipt->TryGetStringField(TEXT("schema"), Schema)
		|| Schema != TEXT("MLSMicroShadowValidationReceiptV1")
		|| !Receipt->TryGetNumberField(TEXT("validatorVersion"), ValidatorVersion)
		|| ValidatorVersion != 1.0
		|| !Receipt->TryGetStringField(TEXT("algorithm"), Algorithm)
		|| Algorithm != TEXT("GenericVNDFBasedMicroShadowing")
		|| !Receipt->TryGetBoolField(TEXT("passed"), bPassed) || !bPassed
		|| !Receipt->TryGetBoolField(TEXT("acceptanceQualified"), bQualified) || !bQualified
		|| !Manifest->TryGetStringField(TEXT("dataSHA256"), ManifestDataSHA)
		|| !Receipt->TryGetStringField(TEXT("dataSHA256"), ReceiptDataSHA)
		|| !Receipt->TryGetStringField(TEXT("manifestFileSHA256"), ReceiptManifestSHA)
		|| !Receipt->TryGetStringField(TEXT("includeFileSHA256"), ReceiptIncludeSHA)
		|| !Receipt->TryGetNumberField(TEXT("packedTexelCount"), ExpectedTexels))
	{
		OutReason = TEXT("validation receipt contract is incomplete or unsupported");
		return false;
	}

	FString PayloadSHA;
	int32 ActualTexels = 0;
	if (!MLS_ComputePackedVNDFPayloadSHA256(ShaderInclude, PayloadSHA, ActualTexels, OutReason))
	{
		return false;
	}
	if (ActualTexels != static_cast<int32>(ExpectedTexels)
		|| !PayloadSHA.Equals(ManifestDataSHA, ESearchCase::IgnoreCase)
		|| !PayloadSHA.Equals(ReceiptDataSHA, ESearchCase::IgnoreCase)
		|| !MLS_HashFileSHA256(ManifestPath).Equals(ReceiptManifestSHA, ESearchCase::IgnoreCase)
		|| !MLS_HashFileSHA256(ShaderInclude).Equals(ReceiptIncludeSHA, ESearchCase::IgnoreCase))
	{
		OutReason = TEXT("payload/manifest/receipt SHA-256 binding or packed texel count mismatch");
		return false;
	}

	OutReason = TEXT("canonical receipt and SHA-256 binding verified");
	return true;
}

static bool MLS_CopyVNDFArtifacts(const FString& OverlayDir, bool bRequired, FString& OutError)
{
	FString ShaderInclude;
	FString Manifest;
	FString Validation;
	if (!MLS_GetVNDFArtifactPaths(ShaderInclude, Manifest)
		|| !FPaths::FileExists(ShaderInclude)
		|| !FPaths::FileExists(Manifest))
	{
		if (bRequired)
		{
			OutError = TEXT("Generic VNDF generated LUT/include manifest pair is missing from Resources/Generated.");
			return false;
		}
		return true;
	}
	MLS_GetVNDFValidationPath(Validation);
	if (bRequired)
	{
		FString AdmissionReason;
		if (!MLS_ValidateVNDFAdmission(ShaderInclude, Manifest, Validation, AdmissionReason))
		{
			OutError = FString::Printf(
				TEXT("Generic VNDF LUT numerical admission failed closed: %s."), *AdmissionReason);
			return false;
		}
	}

	const FString DestinationInclude = OverlayDir / TEXT("Private") / MLS_VNDFLUTFileName;
	const FString DestinationManifest = OverlayDir / TEXT("Private") / MLS_VNDFManifestFileName;
	const FString DestinationValidation = OverlayDir / TEXT("Private") / MLS_VNDFValidationFileName;
	IFileManager& FileManager = IFileManager::Get();
	if (FileManager.Copy(*DestinationInclude, *ShaderInclude, true, true) != COPY_OK
		|| FileManager.Copy(*DestinationManifest, *Manifest, true, true) != COPY_OK
		|| (FPaths::FileExists(Validation)
			&& FileManager.Copy(*DestinationValidation, *Validation, true, true) != COPY_OK))
	{
		OutError = TEXT("Failed to copy Generic VNDF LUT artifacts into the staging overlay.");
		return false;
	}
	return true;
}

static bool MLS_CopyConeEnvArtifacts(const FString& OverlayDir, bool bRequired, FString& OutError)
{
	FString ShaderInclude;
	FString Manifest;
	if (!MLS_GetConeEnvArtifactPaths(ShaderInclude, Manifest)
		|| !FPaths::FileExists(ShaderInclude)
		|| !FPaths::FileExists(Manifest))
	{
		if (bRequired)
		{
			OutError = TEXT("Cone-aware EnvBRDF generated LUT/include manifest pair is missing from Resources/Generated.");
			return false;
		}
		return true;
	}

	IFileManager& FileManager = IFileManager::Get();
	if (FileManager.Copy(*(OverlayDir / TEXT("Private") / MLS_ConeEnvLUTFileName), *ShaderInclude, true, true) != COPY_OK
		|| FileManager.Copy(*(OverlayDir / TEXT("Private") / MLS_ConeEnvManifestFileName), *Manifest, true, true) != COPY_OK)
	{
		OutError = TEXT("Failed to copy Cone EnvBRDF LUT artifacts into the staging overlay.");
		return false;
	}
	return true;
}

static int32 MLS_CountExactOccurrences(const FString& Source, const FString& Needle)
{
	int32 Count = 0;
	int32 SearchFrom = 0;
	while (true)
	{
		const int32 Position = Source.Find(Needle, ESearchCase::CaseSensitive, ESearchDir::FromStart, SearchFrom);
		if (Position == INDEX_NONE)
		{
			break;
		}
		++Count;
		SearchFrom = Position + Needle.Len();
	}
	return Count;
}

// ---------------------------------------------------------------------------

FString FMultiLobeShaderPatcher::GetOverlayBuildId(const FMLSShaderConfig& Cfg)
{
	FString LUTIncludePath;
	FString LUTManifestPath;
	FString LUTValidationPath;
	const bool bHaveArtifactPaths = MLS_GetVNDFArtifactPaths(LUTIncludePath, LUTManifestPath);
	MLS_GetVNDFValidationPath(LUTValidationPath);
	const FString LUTIncludeDigest = bHaveArtifactPaths ? MLS_HashFileSHA1(LUTIncludePath) : TEXT("missing");
	const FString LUTManifestDigest = bHaveArtifactPaths ? MLS_HashFileSHA1(LUTManifestPath) : TEXT("missing");
	const FString LUTValidationDigest = MLS_HashFileSHA1(LUTValidationPath);
	FString ConeEnvIncludePath;
	FString ConeEnvManifestPath;
	const bool bHaveConeEnvArtifactPaths = MLS_GetConeEnvArtifactPaths(ConeEnvIncludePath, ConeEnvManifestPath);
	const FString ConeEnvIncludeDigest = bHaveConeEnvArtifactPaths ? MLS_HashFileSHA1(ConeEnvIncludePath) : TEXT("missing");
	const FString ConeEnvManifestDigest = bHaveConeEnvArtifactPaths ? MLS_HashFileSHA1(ConeEnvManifestPath) : TEXT("missing");
	const FString Identity = FString::Printf(
		TEXT("engine=%s|patch=%d|enabled=%d|w=%.9g|envw=%.9g|scale=%.9g|offset=%.9g|core=%.9g|")
		TEXT("env=%d|enva=%d|diff=%d|tone=%d|micro=%d|md=%.9g|ms=%.9g|mmode=%d|")
		TEXT("lumen=%d|lumenw=%.9g|ibl=%d|coneibl=%d|debug=%d|indirectvis=%d|lut=%s|lutmanifest=%s|lutvalidation=%s|coneenv=%s|conemanifest=%s"),
		*FEngineVersion::Current().ToString(), PatchVersion,
		Cfg.bEnabled ? 1 : 0, Cfg.Lobe2Weight, Cfg.EnvLobe2Weight, Cfg.Lobe2Scale,
		Cfg.Lobe2Offset, Cfg.CoreFade, Cfg.bPatchEnvBRDF ? 1 : 0,
		Cfg.bPatchEnvBRDFApprox ? 1 : 0, Cfg.DiffuseModel, Cfg.TonemapMode,
		Cfg.bMicroShadow ? 1 : 0, Cfg.MicroShadowDiffuseStrength,
		Cfg.MicroShadowSpecularStrength, Cfg.MicroShadowMode,
		Cfg.bLumenDualBlur ? 1 : 0, Cfg.LumenBlurWeight, Cfg.bTwoSampleIBL ? 1 : 0,
		Cfg.bConeAwareIndirectSpecular ? 1 : 0,
		Cfg.DebugView, Cfg.IndirectMaterialVisibilityMode,
		*LUTIncludeDigest, *LUTManifestDigest, *LUTValidationDigest,
		*ConeEnvIncludeDigest, *ConeEnvManifestDigest);

	FTCHARToUTF8 Utf8(*Identity);
	return FSHA1::HashBuffer(Utf8.Get(), static_cast<uint64>(Utf8.Length())).ToString().ToLower();
}

bool FMultiLobeShaderPatcher::EnsureOverlayCopy(const FString& EngineShaderDir, const FString& OverlayDir,
                                                const FMLSShaderConfig& Cfg, bool& bOutFreshCopy, FString& OutError)
{
	bOutFreshCopy = false;

	const FString Stamp = GetOverlayBuildId(Cfg);
	const FString StampPath = OverlayDir / MLS_StampFileName;

	FString ExistingStamp;
	if (FPaths::DirectoryExists(OverlayDir) &&
	    FFileHelper::LoadFileToString(ExistingStamp, *StampPath) &&
	    ExistingStamp == Stamp)
	{
		return true;
	}

	IFileManager::Get().DeleteDirectory(*OverlayDir, /*RequireExists*/false, /*Tree*/true);

	IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
	if (!PF.CopyDirectoryTree(*OverlayDir, *EngineShaderDir, /*bOverwriteExisting*/true))
	{
		OutError = FString::Printf(TEXT("Failed to copy '%s' -> '%s'"), *EngineShaderDir, *OverlayDir);
		return false;
	}

	// NOTE: the stamp is deliberately NOT written here. BuildOverlay commits it
	// only after every patch step has succeeded — otherwise a failed patch run
	// would leave a valid stamp and the next start would map a pristine or
	// half-patched overlay (reviewer finding 13.1).
	bOutFreshCopy = true;
	return true;
}

// ---------------------------------------------------------------------------

bool FMultiLobeShaderPatcher::FindNextFunctionDef(const FString& Src, const FString& Name, int32 SearchFrom, FFuncDef& Out)
{
	const int32 SrcLen = Src.Len();
	int32 Pos = SearchFrom;

	while (true)
	{
		Pos = Src.Find(Name, ESearchCase::CaseSensitive, ESearchDir::FromStart, Pos);
		if (Pos == INDEX_NONE)
		{
			return false;
		}
		const int32 After = Pos + Name.Len();

		if (Pos > 0)
		{
			const TCHAR C = Src[Pos - 1];
			if (FChar::IsAlnum(C) || C == '_') { Pos = After; continue; }
		}
		// identifier boundary on the right (protects FilmToneMapInverse-style prefixes)
		if (After < SrcLen)
		{
			const TCHAR C = Src[After];
			if (FChar::IsAlnum(C) || C == '_') { Pos = After; continue; }
		}

		int32 P = After;
		while (P < SrcLen && FChar::IsWhitespace(Src[P])) ++P;
		if (P >= SrcLen || Src[P] != '(') { Pos = After; continue; }

		int32 Depth = 0, Q = P;
		for (; Q < SrcLen; ++Q)
		{
			if (Src[Q] == '(') ++Depth;
			else if (Src[Q] == ')') { --Depth; if (Depth == 0) break; }
		}
		if (Q >= SrcLen) { return false; }

		int32 R = Q + 1;
		while (R < SrcLen && FChar::IsWhitespace(Src[R])) ++R;
		if (R >= SrcLen || Src[R] != '{') { Pos = After; continue; }

		int32 BDepth = 0, S = R;
		for (; S < SrcLen; ++S)
		{
			if (Src[S] == '{') ++BDepth;
			else if (Src[S] == '}') { --BDepth; if (BDepth == 0) break; }
		}
		if (S >= SrcLen) { return false; }

		int32 LineStart = Pos;
		while (LineStart > 0 && Src[LineStart - 1] != '\n') --LineStart;

		Out.Start = LineStart;
		Out.NamePos = Pos;
		Out.ParamsOpen = P;
		Out.ParamsClose = Q;
		Out.BodyOpen = R;
		Out.BodyClose = S;
		return true;
	}
}

void FMultiLobeShaderPatcher::ParseParamNames(const FString& ParamList, TArray<FString>& OutNames)
{
	TArray<FString> Items;
	{
		int32 Depth = 0;
		FString Cur;
		for (const TCHAR C : ParamList)
		{
			if (C == '(' || C == '<' || C == '[') ++Depth;
			else if (C == ')' || C == '>' || C == ']') --Depth;

			if (C == ',' && Depth == 0)
			{
				Items.Add(Cur);
				Cur.Empty();
			}
			else
			{
				Cur.AppendChar(C);
			}
		}
		if (!Cur.TrimStartAndEnd().IsEmpty())
		{
			Items.Add(Cur);
		}
	}

	for (FString& It : Items)
	{
		It.TrimStartAndEndInline();

		int32 EqIdx;
		if (It.FindChar('=', EqIdx))
		{
			It = It.Left(EqIdx).TrimStartAndEnd();
		}

		int32 End = It.Len();
		int32 Begin = End;
		while (Begin > 0 && (FChar::IsAlnum(It[Begin - 1]) || It[Begin - 1] == '_')) --Begin;
		OutNames.Add(It.Mid(Begin, End - Begin));
	}
}

int32 FMultiLobeShaderPatcher::PatchFunctionDualLobe(FString& Source, const FString& FunctionName,
                                                     const FString& WeightExpr,
                                                     const TCHAR* GuardExpr,
                                                     const TCHAR* RequiredParam,
                                                     const TCHAR* PerLobeMulTemplate)
{
	TArray<FFuncDef> Defs;
	{
		int32 From = 0;
		FFuncDef Def;
		while (FindNextFunctionDef(Source, FunctionName, From, Def))
		{
			Defs.Add(Def);
			From = Def.BodyClose + 1;
		}
	}

	int32 NumPatched = 0;
	const FString CloneName = FunctionName + MLS_CloneSuffix;

	for (int32 i = Defs.Num() - 1; i >= 0; --i)
	{
		const FFuncDef& D = Defs[i];

		const FString ParamList = Source.Mid(D.ParamsOpen + 1, D.ParamsClose - D.ParamsOpen - 1);
		TArray<FString> ParamNames;
		ParseParamNames(ParamList, ParamNames);

		if (!ParamNames.Contains(TEXT("Roughness")))
		{
			continue;
		}
		if (RequiredParam && !ParamNames.Contains(RequiredParam))
		{
			continue; // e.g. thin forwarder overloads — patching them would double-apply the mixture
		}

		const FString OriginalFull = Source.Mid(D.Start, D.BodyClose - D.Start + 1);
		const FString Signature    = Source.Mid(D.Start, D.BodyOpen - D.Start);

		FString Clone = OriginalFull;
		{
			const int32 NameOffset = D.NamePos - D.Start;
			Clone.RemoveAt(NameOffset, FunctionName.Len());
			Clone.InsertAt(NameOffset, CloneName);
		}

		auto MakeArgs = [&ParamNames](const TCHAR* RoughnessWrap) -> FString
		{
			FString Args;
			for (int32 p = 0; p < ParamNames.Num(); ++p)
			{
				if (p > 0) Args += TEXT(", ");
				if (ParamNames[p] == TEXT("Roughness"))
				{
					Args += FString::Printf(TEXT("%s(Roughness)"), RoughnessWrap);
				}
				else
				{
					Args += ParamNames[p];
				}
			}
			return Args;
		};

		const FString CallR1 = FString::Printf(TEXT("%s( %s )"), *CloneName, *MakeArgs(TEXT("MLS_R1")));
		const FString CallR2 = FString::Printf(TEXT("%s( %s )"), *CloneName, *MakeArgs(TEXT("MLS_R2")));
		const FString CallId = FString::Printf(TEXT("%s( %s )"), *CloneName, *MakeArgs(TEXT("")));

		FString Wrapper;
		Wrapper += FString::Printf(TEXT("{\n#if %s\n"), GuardExpr);
		if (PerLobeMulTemplate)
		{
			const FString MulR1 = FString(PerLobeMulTemplate).Replace(TEXT("{R}"), TEXT("MLS_R1(Roughness)"));
			const FString MulR2 = FString(PerLobeMulTemplate).Replace(TEXT("{R}"), TEXT("MLS_R2(Roughness)"));
			Wrapper += TEXT("#if MLS_ENERGY_MIX\n");
			Wrapper += TEXT("\t// Per-lobe multiscatter compensation applied INSIDE the mixture (review v2 \u00a71).\n");
			Wrapper += FString::Printf(TEXT("\treturn lerp(\n\t\t%s * (%s),\n\t\t%s * (%s),\n\t\t%s );\n"),
				*CallR1, *MulR1, *CallR2, *MulR2, *WeightExpr);
			Wrapper += TEXT("#else\n");
		}
		Wrapper += FString::Printf(TEXT("\treturn lerp(\n\t\t%s,\n\t\t%s,\n\t\t%s );\n"),
			*CallR1, *CallR2, *WeightExpr);
		if (PerLobeMulTemplate)
		{
			Wrapper += TEXT("#endif\n");
		}
		Wrapper += FString::Printf(TEXT("#else\n\treturn %s;\n#endif\n}"), *CallId);

		const FString Replacement = Clone + TEXT("\n\n") + Signature + Wrapper;

		Source.RemoveAt(D.Start, D.BodyClose - D.Start + 1);
		Source.InsertAt(D.Start, Replacement);

		++NumPatched;
	}

	return NumPatched;
}

int32 FMultiLobeShaderPatcher::PatchDiffuseCallSite(FString& Source)
{
	// Locate DefaultLitBxDF and rewrite Diffuse_Lambert(...) calls inside its
	// body to the config-switched macro (GBuffer / Context / NoL are all in
	// scope there in every 5.x we know of).
	FFuncDef Fn;
	if (!FindNextFunctionDef(Source, TEXT("DefaultLitBxDF"), 0, Fn))
	{
		return 0;
	}

	struct FCall { int32 Start; int32 End; }; // inclusive of ')'
	TArray<FCall> Calls;

	int32 Pos = Fn.BodyOpen;
	while (true)
	{
		Pos = Source.Find(TEXT("Diffuse_Lambert"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Pos);
		if (Pos == INDEX_NONE || Pos > Fn.BodyClose)
		{
			break;
		}
		const int32 After = Pos + 15; // strlen("Diffuse_Lambert")
		int32 P = After;
		while (P < Source.Len() && FChar::IsWhitespace(Source[P])) ++P;
		if (P < Source.Len() && Source[P] == '(')
		{
			int32 Depth = 0, Q = P;
			for (; Q < Source.Len(); ++Q)
			{
				if (Source[Q] == '(') ++Depth;
				else if (Source[Q] == ')') { --Depth; if (Depth == 0) break; }
			}
			if (Q < Source.Len())
			{
				Calls.Add({ Pos, Q });
				Pos = Q + 1;
				continue;
			}
		}
		Pos = After;
	}

	static const TCHAR* Replacement = TEXT("MLS_DefaultDiffuse(GBuffer, AreaLight)");
	for (int32 i = Calls.Num() - 1; i >= 0; --i)
	{
		Source.RemoveAt(Calls[i].Start, Calls[i].End - Calls[i].Start + 1);
		Source.InsertAt(Calls[i].Start, Replacement);
	}
	return Calls.Num();
}

int32 FMultiLobeShaderPatcher::PatchDefaultLitDualSpecular(FString& Source)
{
	FFuncDef Fn;
	if (!FindNextFunctionDef(Source, TEXT("DefaultLitBxDF"), 0, Fn))
	{
		return 0;
	}

	static const TCHAR* Anchor =
		TEXT("SpecularGGX(GBuffer.Roughness, GBuffer.SpecularColor, Context, AreaLight)");
	const int32 CallPos = Source.Find(Anchor, ESearchCase::CaseSensitive, ESearchDir::FromStart, Fn.BodyOpen);
	if (CallPos == INDEX_NONE || CallPos > Fn.BodyClose)
	{
		return 0;
	}
	const int32 Duplicate = Source.Find(
		Anchor,
		ESearchCase::CaseSensitive,
		ESearchDir::FromStart,
		CallPos + FCString::Strlen(Anchor));
	if (Duplicate != INDEX_NONE && Duplicate <= Fn.BodyClose)
	{
		return 0;
	}

	static const TCHAR* Replacement =
		TEXT("MLS_DefaultLitDualSpecularGGX(GBuffer.Roughness, GBuffer.SpecularColor, Context, AreaLight, GBuffer.GBufferAO, GBuffer.ShadingModelID)");
	Source.RemoveAt(CallPos, FCString::Strlen(Anchor));
	Source.InsertAt(CallPos, Replacement);

	const FString Helper =
		TEXT("float3 MLS_DefaultLitSpecularLobe(float Roughness, float3 SpecularColor, BxDFContext Context, FAreaLight AreaLight, float MaterialVisibility)\n")
		TEXT("{\n")
		TEXT("\tfloat3 Result = SpecularGGX(Roughness, SpecularColor, Context, AreaLight);\n")
		TEXT("#if MLS_ENERGY_MIX\n")
		TEXT("\tResult *= ComputeEnergyConservation(ComputeGGXSpecEnergyTerms(Roughness, Context.NoV, SpecularColor));\n")
		TEXT("#endif\n")
		TEXT("#if MLS_MICROSHADOW\n")
		TEXT("\tfloat MicroShadow = MLS_EvaluateMicroShadow(MaterialVisibility, Roughness, AreaLight.NoL, Context.NoV);\n")
		TEXT("\tResult *= lerp(1.0, MicroShadow, MLS_MICROSHADOW_SPECULAR_STRENGTH);\n")
		TEXT("#endif\n")
		TEXT("\treturn Result;\n")
		TEXT("}\n\n")
		TEXT("float3 MLS_DefaultLitDualSpecularGGX(float Roughness, float3 SpecularColor, BxDFContext Context, FAreaLight AreaLight, float MaterialVisibility, uint ShadingModelID)\n")
		TEXT("{\n")
		TEXT("#if MLS_ENABLED\n")
		TEXT("\tif (ShadingModelID == SHADINGMODELID_DEFAULT_LIT)\n")
		TEXT("\t{\n")
		TEXT("\t\tfloat Weight = MLS_LOBE2_W(Roughness);\n")
		TEXT("\t\tfloat Roughness1 = MLS_R1(Roughness);\n")
		TEXT("\t\tfloat Roughness2 = MLS_R2(Roughness);\n")
		TEXT("\t\tif (Weight <= 1e-4 || abs(Roughness2 - Roughness1) < 1e-5)\n")
		TEXT("\t\t{\n")
		TEXT("\t\t\treturn MLS_DefaultLitSpecularLobe(Roughness1, SpecularColor, Context, AreaLight, MaterialVisibility);\n")
		TEXT("\t\t}\n")
		TEXT("\t\tif (Weight >= 1.0 - 1e-4)\n")
		TEXT("\t\t{\n")
		TEXT("\t\t\treturn MLS_DefaultLitSpecularLobe(Roughness2, SpecularColor, Context, AreaLight, MaterialVisibility);\n")
		TEXT("\t\t}\n")
		TEXT("\t\tfloat3 Specular1 = MLS_DefaultLitSpecularLobe(Roughness1, SpecularColor, Context, AreaLight, MaterialVisibility);\n")
		TEXT("\t\tfloat3 Specular2 = MLS_DefaultLitSpecularLobe(Roughness2, SpecularColor, Context, AreaLight, MaterialVisibility);\n")
		TEXT("\t\treturn lerp(Specular1, Specular2, Weight);\n")
		TEXT("\t}\n")
		TEXT("#endif\n")
		TEXT("\treturn SpecularGGX(Roughness, SpecularColor, Context, AreaLight);\n")
		TEXT("}\n\n");
	Source.InsertAt(Fn.Start, Helper);
	return 1;
}

int32 FMultiLobeShaderPatcher::PatchMicroShadow(FString& Source)
{
	// Multiply direct diffuse/specular by the AO-cone visibility term right
	// before DefaultLitBxDF returns. GBuffer / NoL are in scope there; the
	// local result variable has been named 'Lighting' throughout 5.x.
	FFuncDef Fn;
	if (!FindNextFunctionDef(Source, TEXT("DefaultLitBxDF"), 0, Fn))
	{
		return 0;
	}

	static const TCHAR* Anchor = TEXT("return Lighting;");
	const int32 Pos = Source.Find(Anchor, ESearchCase::CaseSensitive, ESearchDir::FromStart, Fn.BodyOpen);
	if (Pos == INDEX_NONE || Pos > Fn.BodyClose)
	{
		return 0;
	}

	static const TCHAR* Block =
		TEXT("#if MLS_ENABLED && MLS_DEBUG_VIEW == 1\n")
		TEXT("\tLighting.Diffuse = (MLS_LOBE2_W(GBuffer.Roughness)).xxx; Lighting.Specular = 0;\n")
		TEXT("#elif MLS_ENABLED && MLS_DEBUG_VIEW == 2\n")
		TEXT("\tLighting.Diffuse = (MLS_R2(GBuffer.Roughness)).xxx; Lighting.Specular = 0;\n")
		TEXT("#endif\n")
		TEXT("#if MLS_MICROSHADOW\n")
		TEXT("#if SUPPORTS_ANISOTROPIC_MATERIALS\n")
		TEXT("\tbool MLS_HasAnisotropyFallback = HasAnisotropy(GBuffer.SelectiveOutputMask);\n")
		TEXT("#else\n")
		TEXT("\tbool MLS_HasAnisotropyFallback = false;\n")
		TEXT("#endif\n")
		TEXT("\tBRANCH\n")
		TEXT("\tif (GBuffer.ShadingModelID == SHADINGMODELID_DEFAULT_LIT && !MLS_HasAnisotropyFallback && AreaLight.NoL > 0.0f && !IsRectLight(AreaLight))\n")
		TEXT("\t{\n")
		TEXT("\t\tfloat MLS_MS_D = MLS_EvaluateMicroShadow(GBuffer.GBufferAO, GBuffer.Roughness, AreaLight.NoL, Context.NoV);\n")
		TEXT("#if MLS_DEBUG_VIEW == 3\n")
		TEXT("\t\tLighting.Diffuse = saturate(GBuffer.GBufferAO).xxx; Lighting.Specular = 0;\n")
		TEXT("#elif MLS_DEBUG_VIEW == 4\n")
		TEXT("\t\tLighting.Diffuse = MLS_MS_D.xxx; Lighting.Specular = 0;\n")
		TEXT("#elif MLS_DEBUG_VIEW == 5\n")
		TEXT("\t\tLighting.Diffuse = saturate(AreaLight.NoL).xxx; Lighting.Specular = 0;\n")
		TEXT("#else\n")
		TEXT("\t\tLighting.Diffuse *= lerp(1.0, MLS_MS_D, MLS_MICROSHADOW_DIFFUSE_STRENGTH);\n")
		TEXT("#endif\n")
		TEXT("\t}\n")
		TEXT("#endif\n\t");

	Source.InsertAt(Pos, FString(Block));
	return 1;
}

int32 FMultiLobeShaderPatcher::PatchEnergyMix(FString& Source)
{
	// Per-lobe energy conservation (reviewer §7). FBxDFEnergyTerms is a #define
	// alias over two different structs, so we never touch its fields — we mix
	// the applied SCALARS per lobe instead (type-safe for both variants).
	FFuncDef Fn;
	if (!FindNextFunctionDef(Source, TEXT("DefaultLitBxDF"), 0, Fn))
	{
		return 0;
	}

	auto ReplaceInBody = [&Source, &Fn](const TCHAR* From, const FString& To) -> bool
	{
		const int32 Pos = Source.Find(From, ESearchCase::CaseSensitive, ESearchDir::FromStart, Fn.BodyOpen);
		if (Pos == INDEX_NONE || Pos > Fn.BodyClose)
		{
			return false;
		}
		Source.RemoveAt(Pos, FCString::Strlen(From));
		Source.InsertAt(Pos, To);
		Fn.BodyClose += To.Len() - FCString::Strlen(From);
		return true;
	};

	int32 Num = 0;

	Num += ReplaceInBody(
		TEXT("FBxDFEnergyTerms EnergyTerms = ComputeGGXSpecEnergyTerms(GBuffer.Roughness, Context.NoV, GBuffer.SpecularColor);"),
		FString(
			TEXT("FBxDFEnergyTerms EnergyTerms = ComputeGGXSpecEnergyTerms(GBuffer.Roughness, Context.NoV, GBuffer.SpecularColor);\n")
			TEXT("#if MLS_ENABLED && MLS_ENERGY_MIX\n")
			TEXT("\t\tFBxDFEnergyTerms MLS_EnergyTerms2 = ComputeGGXSpecEnergyTerms(MLS_R2(GBuffer.Roughness), Context.NoV, GBuffer.SpecularColor);\n")
			TEXT("\t\tbool MLS_IsDualLobeDefaultLit = GBuffer.ShadingModelID == SHADINGMODELID_DEFAULT_LIT && !bHasAnisotropy;\n")
			TEXT("\t\tfloat MLS_EW = MLS_IsDualLobeDefaultLit ? MLS_LOBE2_W(GBuffer.Roughness) : 0.0;\n")
			TEXT("#endif"))) ? 1 : 0;

	Num += ReplaceInBody(
		TEXT("Lighting.Diffuse *= ComputeEnergyPreservation(EnergyTerms);"),
		FString(
			TEXT("\n#if MLS_ENABLED && MLS_ENERGY_MIX\n")
			TEXT("\t\tLighting.Diffuse *= lerp(ComputeEnergyPreservation(EnergyTerms), ComputeEnergyPreservation(MLS_EnergyTerms2), MLS_EW);\n")
			TEXT("#else\n")
			TEXT("\t\tLighting.Diffuse *= ComputeEnergyPreservation(EnergyTerms);\n")
			TEXT("#endif"))) ? 1 : 0;

	Num += ReplaceInBody(
		TEXT("Lighting.Specular *= ComputeEnergyConservation(EnergyTerms);"),
		FString(
			TEXT("\n#if MLS_ENABLED && MLS_ENERGY_MIX\n")
			TEXT("\t\t// Non-Rect isotropic Default Lit and Rect adapters own per-lobe conservation.\n")
			TEXT("\t\tif (!MLS_IsDualLobeDefaultLit)\n")
			TEXT("\t\t{\n")
			TEXT("\t\t\tLighting.Specular *= ComputeEnergyConservation(EnergyTerms);\n")
			TEXT("\t\t}\n")
			TEXT("#else\n")
			TEXT("\t\tLighting.Specular *= ComputeEnergyConservation(EnergyTerms);\n")
			TEXT("#endif"))) ? 1 : 0;

	return Num;
}

bool FMultiLobeShaderPatcher::PatchLumenResolve(const FString& OverlayDir, FString& OutWarning)
{
	const FString FilePath = OverlayDir / TEXT("Private/Lumen/LumenReflectionResolve.usf");
	FString Src;
	if (!FFileHelper::LoadFileToString(Src, *FilePath))
	{
		OutWarning = TEXT("LumenReflectionResolve.usf not found — reconstruction stays base-roughness.");
		return false;
	}
	if (Src.Contains(TEXT("MLS_ReconR")))
	{
		return true; // already patched
	}

	int32 Num = 0;
	{
		// Mixture numerator (review v2 \u00a75 tail): RayData.PDF is now the mixture PDF,
		// so the BRDF weight D(H)/PDF must use the mixture NDF as well.
		static const TCHAR* From = TEXT("SampleNDF = D_GGX(a2, H.z);");
		const int32 Pos = Src.Find(From, ESearchCase::CaseSensitive);
		if (Pos != INDEX_NONE)
		{
			static const TCHAR* Add =
				TEXT("\n#if MLS_ENABLED && MLS_LUMEN_DUALBLUR\n")
				TEXT("\t\t{\n")
				TEXT("\t\t\tfloat MLS_R1v = pow(max(a2, 1e-8), 0.25);\n")
				TEXT("\t\t\tfloat MLS_Wl = MLS_LUMEN_BLUR_WEIGHT * saturate(MLS_R1v * MLS_CORE_FADE);\n")
				TEXT("\t\t\tif (MLS_Wl > 1e-3)\n")
				TEXT("\t\t\t{\n")
				TEXT("\t\t\t\tfloat MLS_R2v = MLS_R2(MLS_R1v);\n")
				TEXT("\t\t\t\tfloat MLS_A2w = MLS_R2v * MLS_R2v * MLS_R2v * MLS_R2v;\n")
				TEXT("\t\t\t\tSampleNDF = lerp(SampleNDF, D_GGX(MLS_A2w, H.z), MLS_Wl);\n")
				TEXT("\t\t\t}\n")
				TEXT("\t\t}\n")
				TEXT("#endif");
			const int32 LineEnd = Pos + FCString::Strlen(From);
			Src.InsertAt(LineEnd, FString(Add));
			++Num;
		}
	}
	{
		static const TCHAR* From = TEXT("saturate(Material.TopLayerRoughness * 8.0f)");
		static const TCHAR* To   = TEXT("saturate(MLS_ReconR(Material.TopLayerRoughness) * 8.0f)");
		const int32 Pos = Src.Find(From, ESearchCase::CaseSensitive);
		if (Pos != INDEX_NONE) { Src.RemoveAt(Pos, FCString::Strlen(From)); Src.InsertAt(Pos, To); ++Num; }
	}
	{
		static const TCHAR* From = TEXT("Material.TopLayerRoughness * SpatialReconstructionRoughnessScale");
		static const TCHAR* To   = TEXT("MLS_ReconR(Material.TopLayerRoughness) * SpatialReconstructionRoughnessScale");
		const int32 Pos = Src.Find(From, ESearchCase::CaseSensitive);
		if (Pos != INDEX_NONE) { Src.RemoveAt(Pos, FCString::Strlen(From)); Src.InsertAt(Pos, To); ++Num; }
	}

	if (Num != 3)
	{
		OutWarning = FString::Printf(
			TEXT("Lumen resolve exact-count failure: expected 3 anchors, found %d; file left untouched."), Num);
		return false;
	}

	// Config include after the last #include preceding the first use.
	const int32 UsePos = Src.Find(TEXT("MLS_"), ESearchCase::CaseSensitive);
	int32 IncPos = INDEX_NONE;
	{
		int32 From2 = 0;
		while (true)
		{
			const int32 P = Src.Find(TEXT("#include"), ESearchCase::CaseSensitive, ESearchDir::FromStart, From2);
			if (P == INDEX_NONE || P > UsePos) { break; }
			IncPos = P; From2 = P + 8;
		}
	}
	if (IncPos != INDEX_NONE)
	{
		int32 LineEnd = IncPos;
		while (LineEnd < Src.Len() && Src[LineEnd] != '\n') ++LineEnd;
		Src.InsertAt(LineEnd, FString::Printf(TEXT("\n%s %s"), MLS_ConfigInclude, MLS_PatchMarker));
	}
	else
	{
		Src.InsertAt(0, FString::Printf(TEXT("%s %s\n"), MLS_ConfigInclude, MLS_PatchMarker));
	}

	if (!FFileHelper::SaveStringToFile(Src, *FilePath))
	{
		OutWarning = TEXT("Failed to save patched LumenReflectionResolve.usf");
		return false;
	}
	UE_LOG(LogMultiLobeSpec, Log, TEXT("Lumen resolve: %d site(s) mixture-aware (expected 3: NDF numerator + kernel + effective roughness)."), Num);
	return true;
}

bool FMultiLobeShaderPatcher::WriteConeEnvBRDFRuntimeFile(const FString& OverlayDir, FString& OutError)
{
	const FString Runtime =
		TEXT("// Generated by MultiLobeSpec patcher. CARD-09 runtime only; no Monte Carlo.\n")
		TEXT("#ifndef MLS_CONE_ENVBRDF_RUNTIME_INCLUDED\n")
		TEXT("#define MLS_CONE_ENVBRDF_RUNTIME_INCLUDED 1\n")
		TEXT("#include \"/Engine/Private/MultiLobeSpecConfig.ush\"\n")
		TEXT("#if MLS_ENABLED && MLS_CONE_ENVBRDF\n")
		TEXT("#include \"/Engine/Private/MLS_ConeEnvBRDF.ush\"\n")
		TEXT("float2 MLS_ConeEnv_UnpackRG16(uint Packed)\n")
		TEXT("{\n")
		TEXT("\treturn float2(Packed & 65535u, Packed >> 16u) * (1.0 / 65535.0);\n")
		TEXT("}\n")
		TEXT("float2 MLS_ConeEnv_Load(int NoVIndex, int RoughnessIndex, int ConeIndex)\n")
		TEXT("{\n")
		TEXT("\tint PlaneIndex = RoughnessIndex * MLS_CONE_ENV_NOV_SIZE + NoVIndex;\n")
		TEXT("\tuint4 Packed4 = 0u;\n")
		TEXT("\tswitch (ConeIndex)\n")
		TEXT("\t{\n")
		TEXT("\tcase 0: Packed4 = MLS_CONE_ENVBRDF_PLANE_C0[PlaneIndex >> 2]; break;\n")
		TEXT("\tcase 1: Packed4 = MLS_CONE_ENVBRDF_PLANE_C1[PlaneIndex >> 2]; break;\n")
		TEXT("\tcase 2: Packed4 = MLS_CONE_ENVBRDF_PLANE_C2[PlaneIndex >> 2]; break;\n")
		TEXT("\tcase 3: Packed4 = MLS_CONE_ENVBRDF_PLANE_C3[PlaneIndex >> 2]; break;\n")
		TEXT("\tcase 4: Packed4 = MLS_CONE_ENVBRDF_PLANE_C4[PlaneIndex >> 2]; break;\n")
		TEXT("\tcase 5: Packed4 = MLS_CONE_ENVBRDF_PLANE_C5[PlaneIndex >> 2]; break;\n")
		TEXT("\tcase 6: Packed4 = MLS_CONE_ENVBRDF_PLANE_C6[PlaneIndex >> 2]; break;\n")
		TEXT("\tdefault: Packed4 = MLS_CONE_ENVBRDF_PLANE_C7[PlaneIndex >> 2]; break;\n")
		TEXT("\t}\n")
		TEXT("\tuint Packed = Packed4[PlaneIndex & 3];\n")
		TEXT("\treturn MLS_ConeEnv_UnpackRG16(Packed);\n")
		TEXT("}\n")
		TEXT("float2 MLS_ConeEnv_Trilinear(float3 GridPosition)\n")
		TEXT("{\n")
		TEXT("\tGridPosition = clamp(GridPosition, 0.0, float3(MLS_CONE_ENV_NOV_SIZE - 1, MLS_CONE_ENV_ROUGHNESS_SIZE - 1, MLS_CONE_ENV_ADJUSTED_COS_SIZE - 1));\n")
		TEXT("\tint3 I0 = (int3)floor(GridPosition);\n")
		TEXT("\tint3 I1 = min(I0 + 1, int3(MLS_CONE_ENV_NOV_SIZE - 1, MLS_CONE_ENV_ROUGHNESS_SIZE - 1, MLS_CONE_ENV_ADJUSTED_COS_SIZE - 1));\n")
		TEXT("\tfloat3 F = saturate(GridPosition - (float3)I0);\n")
		TEXT("\tfloat2 C000 = MLS_ConeEnv_Load(I0.x, I0.y, I0.z);\n")
		TEXT("\tfloat2 C100 = MLS_ConeEnv_Load(I1.x, I0.y, I0.z);\n")
		TEXT("\tfloat2 C010 = MLS_ConeEnv_Load(I0.x, I1.y, I0.z);\n")
		TEXT("\tfloat2 C110 = MLS_ConeEnv_Load(I1.x, I1.y, I0.z);\n")
		TEXT("\tfloat2 C001 = MLS_ConeEnv_Load(I0.x, I0.y, I1.z);\n")
		TEXT("\tfloat2 C101 = MLS_ConeEnv_Load(I1.x, I0.y, I1.z);\n")
		TEXT("\tfloat2 C011 = MLS_ConeEnv_Load(I0.x, I1.y, I1.z);\n")
		TEXT("\tfloat2 C111 = MLS_ConeEnv_Load(I1.x, I1.y, I1.z);\n")
		TEXT("\tfloat2 C00 = lerp(C000, C100, F.x);\n")
		TEXT("\tfloat2 C10 = lerp(C010, C110, F.x);\n")
		TEXT("\tfloat2 C01 = lerp(C001, C101, F.x);\n")
		TEXT("\tfloat2 C11 = lerp(C011, C111, F.x);\n")
		TEXT("\treturn lerp(lerp(C00, C10, F.y), lerp(C01, C11, F.y), F.z);\n")
		TEXT("}\n")
		TEXT("float MLS_AdjustSpecularCone(float CosCone, float Roughness, float NoV)\n")
		TEXT("{\n")
		TEXT("\tfloat Gloss = saturate((1.0 - Roughness) * 1.5);\n")
		TEXT("\tfloat Gloss2 = Gloss * Gloss;\n")
		TEXT("\tfloat Gloss4 = Gloss2 * Gloss2;\n")
		TEXT("\tfloat Gloss8 = Gloss4 * Gloss4;\n")
		TEXT("\tfloat OneMinusNoV2 = (1.0 - NoV) * (1.0 - NoV);\n")
		TEXT("\tfloat OneMinusNoV4 = OneMinusNoV2 * OneMinusNoV2;\n")
		TEXT("\treturn lerp(0.0, CosCone, (1.0 - Gloss8) * (1.0 - OneMinusNoV4));\n")
		TEXT("}\n")
		TEXT("float2 MLS_ConeEnv_SampleAB(float NoV, float Roughness, float AdjustedCosCone)\n")
		TEXT("{\n")
		TEXT("\tfloat3 GridPosition = float3(\n")
		TEXT("\t\tsaturate(NoV) * MLS_CONE_ENV_NOV_SIZE - 0.5,\n")
		TEXT("\t\tsaturate(Roughness) * MLS_CONE_ENV_ROUGHNESS_SIZE - 0.5,\n")
		TEXT("\t\tsaturate(AdjustedCosCone) * (MLS_CONE_ENV_ADJUSTED_COS_SIZE - 1));\n")
		TEXT("\treturn MLS_ConeEnv_Trilinear(GridPosition);\n")
		TEXT("}\n")
		TEXT("float3 MLS_ConeEnv_Response(float3 F0, float F90, float Roughness, float NoV, float Visibility)\n")
		TEXT("{\n")
		TEXT("\tfloat RawCosCone = sqrt(saturate(1.0 - Visibility));\n")
		TEXT("\tfloat AdjustedCosCone = MLS_AdjustSpecularCone(RawCosCone, Roughness, NoV);\n")
		TEXT("\tfloat2 AB = MLS_ConeEnv_SampleAB(NoV, Roughness, AdjustedCosCone);\n")
		TEXT("\treturn F0 * AB.x + F90 * AB.y;\n")
		TEXT("}\n")
		TEXT("float3 MLS_ConeEnv_SingleScatter(float3 F0, float Roughness, float NoV, float Visibility)\n")
		TEXT("{\n")
		TEXT("\treturn MLS_ConeEnv_Response(F0, saturate(50.0 * F0.g), Roughness, NoV, Visibility);\n")
		TEXT("}\n")
		TEXT("float3 MLS_ConeEnv_EnergyAware(float3 F0, float Roughness, float NoV, float Visibility, float3 MultiScatterE)\n")
		TEXT("{\n")
		TEXT("\tif (Visibility >= 1.0) return MultiScatterE;\n")
		TEXT("\t// Match ComputeGGXSpecEnergyTerms/F0RGBToMicroOcclusion: EC uses max(F0), not legacy EnvBRDF's green channel.\n")
		TEXT("\tfloat F90 = saturate(50.0 * max(F0.r, max(F0.g, F0.b)));\n")
		TEXT("\tfloat3 ConeGF = MLS_ConeEnv_Response(F0, F90, Roughness, NoV, Visibility);\n")
		TEXT("\tfloat2 OpenAB = MLS_ConeEnv_SampleAB(NoV, Roughness, 0.0);\n")
		TEXT("\tfloat3 OpenGF = F0 * OpenAB.x + F90 * OpenAB.y;\n")
		TEXT("\treturn MultiScatterE * saturate(ConeGF / max(OpenGF, 1e-5));\n")
		TEXT("}\n")
		TEXT("#endif // MLS_ENABLED && MLS_CONE_ENVBRDF\n")
		TEXT("#endif // MLS_CONE_ENVBRDF_RUNTIME_INCLUDED\n");

	const FString Path = OverlayDir / TEXT("Private") / MLS_ConeEnvRuntimeFileName;
	if (!FFileHelper::SaveStringToFile(Runtime, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		OutError = FString::Printf(TEXT("Failed to write Cone EnvBRDF runtime include: %s"), *Path);
		return false;
	}
	return true;
}

bool FMultiLobeShaderPatcher::PatchPairedIBL(const FString& OverlayDir, FString& OutWarning)
{
	// Verified against real 5.7 ReflectionEnvironmentPixelShader.usf. Three edits,
	// ALL mandatory (material/geometric AO split + paired gather + paired response); if any anchor is
	// missing the file is left untouched and the feature stays unavailable.
	const FString FilePath = OverlayDir / TEXT("Private/ReflectionEnvironmentPixelShader.usf");
	FString Src;
	if (!FFileHelper::LoadFileToString(Src, *FilePath))
	{
		OutWarning = TEXT("ReflectionEnvironmentPixelShader.usf not found — paired IBL unavailable.");
		return false;
	}
	if (Src.Contains(TEXT("MLS_PAIRED_IBL")))
	{
		return true; // already patched
	}

	// ---- Edit A: per-lobe gather (unique legacy gather line; clear-coat top-layer gather differs) ----
	static const TCHAR* GatherAnchor =
		TEXT("Color.rgb += View.PreExposure * GatherRadiance(Color.a, TranslatedWorldPosition, R, GBuffer.Roughness, BentNormal, IndirectIrradiance, GBuffer.ShadingModelID, NumCulledReflectionCaptures, CaptureDataStartIndex);");
	static const TCHAR* AOAnchor = TEXT("float AO = GBuffer.GBufferAO * AmbientOcclusion;");
	const int32 GatherCount = MLS_CountExactOccurrences(Src, GatherAnchor);
	const int32 AOCount = MLS_CountExactOccurrences(Src, AOAnchor);
	const int32 GatherPos = Src.Find(GatherAnchor, ESearchCase::CaseSensitive);
	// ---- Edit B: response block (unique EnvBRDF line; expand to its #if/#endif) ----
	static const TCHAR* RespAnchor = TEXT("Color.rgb *= EnvBRDF( SpecularColor, GBuffer.Roughness, NoV );");
	const int32 RespCount = MLS_CountExactOccurrences(Src, RespAnchor);
	const int32 RespPos = Src.Find(RespAnchor, ESearchCase::CaseSensitive);

	if (GatherCount != 1 || AOCount != 1 || RespCount != 1 || GatherPos == INDEX_NONE || RespPos == INDEX_NONE)
	{
		OutWarning = FString::Printf(
			TEXT("Paired/cone IBL exact-count failure: expected AO/Gather/Response 1/1/1, found %d/%d/%d; file left untouched."),
			AOCount, GatherCount, RespCount);
		return false;
	}

	// Expand Edit B to the enclosing USE_ENERGY_CONSERVATION block.
	const int32 IfPos = Src.Find(TEXT("#if USE_ENERGY_CONSERVATION"), ESearchCase::CaseSensitive, ESearchDir::FromEnd, RespPos);
	int32 EndifPos = Src.Find(TEXT("#endif"), ESearchCase::CaseSensitive, ESearchDir::FromStart, RespPos);
	if (IfPos == INDEX_NONE || EndifPos == INDEX_NONE || IfPos > RespPos)
	{
		OutWarning = TEXT("Paired IBL: USE_ENERGY_CONSERVATION block around the response not found — feature unavailable.");
		return false;
	}
	int32 EndifEnd = EndifPos;
	while (EndifEnd < Src.Len() && Src[EndifEnd] != '\n') ++EndifEnd;

	const FString OrigResp = Src.Mid(IfPos, EndifEnd - IfPos);
	const FString NewResp = FString::Printf(
		TEXT("#if MLS_ENABLED && MLS_PAIRED_IBL\n")
		TEXT("\t\tif (MLS_IBL_Eligible)\n")
		TEXT("\t\t{\n")
		TEXT("\t\t\t// Lumen/SSR stays authored-roughness/single-lobe: lobe identity is unavailable.\n")
		TEXT("\t\t\t#if USE_ENERGY_CONSERVATION\n")
		TEXT("\t\t\tfloat3 MLS_StockB1 = EnergyTerms.E;\n")
		TEXT("\t\t\tfloat3 MLS_StockB2 = ComputeGGXSpecEnergyTerms(MLS_R2(GBuffer.Roughness), NoV, GBuffer.SpecularColor).E;\n")
		TEXT("\t\t\t#if MLS_CONE_ENVBRDF\n")
		TEXT("\t\t\tfloat3 MLS_B1 = MLS_ConeEnv_EnergyAware(SpecularColor, GBuffer.Roughness, NoV, MLS_MaterialVisibility, MLS_StockB1);\n")
		TEXT("\t\t\tfloat3 MLS_B2 = MLS_ConeEnv_EnergyAware(SpecularColor, MLS_R2(GBuffer.Roughness), NoV, MLS_MaterialVisibility, MLS_StockB2);\n")
		TEXT("\t\t\t#else\n")
		TEXT("\t\t\tfloat3 MLS_B1 = MLS_StockB1;\n")
		TEXT("\t\t\tfloat3 MLS_B2 = MLS_StockB2;\n")
		TEXT("\t\t\t#endif\n")
		TEXT("\t\t\t#else\n")
		TEXT("\t\t\tfloat3 MLS_StockB1 = EnvBRDF_MLS_Orig(SpecularColor, GBuffer.Roughness, NoV);\n")
		TEXT("\t\t\t#if MLS_CONE_ENVBRDF\n")
		TEXT("\t\t\tfloat3 MLS_B1 = MLS_ConeEnv_SingleScatter(SpecularColor, GBuffer.Roughness, NoV, MLS_MaterialVisibility);\n")
		TEXT("\t\t\tfloat3 MLS_B2 = MLS_ConeEnv_SingleScatter(SpecularColor, MLS_R2(GBuffer.Roughness), NoV, MLS_MaterialVisibility);\n")
		TEXT("\t\t\t#else\n")
		TEXT("\t\t\tfloat3 MLS_B1 = MLS_StockB1;\n")
		TEXT("\t\t\tfloat3 MLS_B2 = EnvBRDF_MLS_Orig(SpecularColor, MLS_R2(GBuffer.Roughness), NoV);\n")
		TEXT("\t\t\t#endif\n")
		TEXT("\t\t\t#endif\n")
		TEXT("\t\t\tColor.rgb = MLS_UnpairedReflection * MLS_StockB1\n")
		TEXT("\t\t\t          + lerp(MLS_L1 * MLS_B1, MLS_L2 * MLS_B2, MLS_IBLW);\n")
		TEXT("\t\t}\n")
		TEXT("\t\telse\n")
		TEXT("\t\t{\n")
		TEXT("%s\n")
		TEXT("\t\t}\n")
		TEXT("#else\n")
		TEXT("%s\n")
		TEXT("#endif"),
		*OrigResp,
		*OrigResp);
	Src.RemoveAt(IfPos, EndifEnd - IfPos);
	Src.InsertAt(IfPos, NewResp);

	// Edit A (indices before Edit B region, still valid: GatherPos < IfPos).
	const FString OrigGather(GatherAnchor);
	const FString NewGather = FString::Printf(
		TEXT("#if MLS_ENABLED && MLS_PAIRED_IBL\n")
		TEXT("\t// Exact radiance/response pairing is limited to isotropic legacy DefaultLit.\n")
		TEXT("\tfloat MLS_IBLW = MLS_IBL_Eligible ? MLS_ENV_W(GBuffer.Roughness) : 0.0;\n")
		TEXT("\tfloat3 MLS_UnpairedReflection = Color.rgb;\n")
		TEXT("\tfloat3 MLS_Dir1 = R;\n")
		TEXT("\tfloat3 MLS_Dir2 = R;\n")
		TEXT("\tif (MLS_IBL_Eligible)\n")
		TEXT("\t{\n")
		TEXT("\t\tfloat3 MLS_MirrorR = 2.0 * dot(V, N) * N - V;\n")
		TEXT("\t\tMLS_Dir1 = GetOffSpecularPeakReflectionDir(N, MLS_MirrorR, GBuffer.Roughness);\n")
		TEXT("\t\tMLS_Dir2 = GetOffSpecularPeakReflectionDir(N, MLS_MirrorR, MLS_R2(GBuffer.Roughness));\n")
		TEXT("\t}\n")
		TEXT("\tfloat3 MLS_L1 = View.PreExposure * GatherRadiance(Color.a, TranslatedWorldPosition, MLS_Dir1, GBuffer.Roughness, BentNormal, IndirectIrradiance, GBuffer.ShadingModelID, NumCulledReflectionCaptures, CaptureDataStartIndex);\n")
		TEXT("\tfloat3 MLS_L2 = MLS_L1;\n")
		TEXT("\tBRANCH\n")
		TEXT("\tif (MLS_IBLW > 1e-4)\n")
		TEXT("\t{\n")
		TEXT("\t\tMLS_L2 = View.PreExposure * GatherRadiance(Color.a, TranslatedWorldPosition, MLS_Dir2, MLS_R2(GBuffer.Roughness), BentNormal, IndirectIrradiance, GBuffer.ShadingModelID, NumCulledReflectionCaptures, CaptureDataStartIndex);\n")
		TEXT("\t}\n")
		TEXT("\tColor.rgb += MLS_L1;\n")
		TEXT("#else\n")
		TEXT("\t%s\n")
		TEXT("#endif"),
		*OrigGather);
	{
		const int32 Pos2 = Src.Find(GatherAnchor, ESearchCase::CaseSensitive);
		Src.RemoveAt(Pos2, OrigGather.Len());
		Src.InsertAt(Pos2, NewGather);
	}

	// Split raw material visibility from Screen/DFAO geometry before the cone
	// response. This prevents applying GBufferAO once through stock GTSO and a
	// second time through the cone-integrated EnvBRDF.
	{
		const FString OrigAO(AOAnchor);
		const FString NewAO = FString::Printf(
			TEXT("#if MLS_ENABLED && MLS_PAIRED_IBL\n")
			TEXT("\tbool MLS_IBL_Eligible = GBuffer.ShadingModelID == SHADINGMODELID_DEFAULT_LIT;\n")
			TEXT("\t#if SUPPORTS_ANISOTROPIC_MATERIALS\n")
			TEXT("\tMLS_IBL_Eligible = MLS_IBL_Eligible && abs(GBuffer.Anisotropy) <= 1e-4;\n")
			TEXT("\t#endif\n")
			TEXT("\t#if MLS_CONE_ENVBRDF\n")
			TEXT("\tfloat MLS_MaterialVisibility = saturate(GBuffer.GBufferAO);\n")
			TEXT("\tfloat AO = (MLS_IBL_Eligible ? 1.0 : GBuffer.GBufferAO) * AmbientOcclusion;\n")
			TEXT("\t#else\n")
			TEXT("\t%s\n")
			TEXT("\t#endif\n")
			TEXT("#else\n")
			TEXT("\t%s\n")
			TEXT("#endif"),
			*OrigAO,
			*OrigAO);
		const int32 AOPos = Src.Find(AOAnchor, ESearchCase::CaseSensitive);
		Src.RemoveAt(AOPos, OrigAO.Len());
		Src.InsertAt(AOPos, NewAO);
	}

	// Config include after the last #include preceding first use.
	{
		const int32 UsePos = Src.Find(TEXT("MLS_PAIRED_IBL"), ESearchCase::CaseSensitive);
		int32 IncPos = INDEX_NONE;
		int32 From2 = 0;
		while (true)
		{
			const int32 P = Src.Find(TEXT("#include"), ESearchCase::CaseSensitive, ESearchDir::FromStart, From2);
			if (P == INDEX_NONE || P > UsePos) { break; }
			IncPos = P; From2 = P + 8;
		}
		if (IncPos != INDEX_NONE)
		{
			int32 LineEnd = IncPos;
			while (LineEnd < Src.Len() && Src[LineEnd] != '\n') ++LineEnd;
			Src.InsertAt(LineEnd, FString::Printf(TEXT("\n%s %s"), MLS_ConeEnvRuntimeInclude, MLS_PatchMarker));
		}
	}

	if (!FFileHelper::SaveStringToFile(Src, *FilePath))
	{
		OutWarning = TEXT("Failed to save patched ReflectionEnvironmentPixelShader.usf");
		return false;
	}
	UE_LOG(LogMultiLobeSpec, Log, TEXT("Paired/Cone IBL: AO/gather/response exact anchors patched 1/1/1; Lumen/SSR response remains single-lobe."));
	return true;
}

int32 FMultiLobeShaderPatcher::PatchRectAdapter(FString& Source)
{
	// Specialized Rect LTC wrapper (review v2 \u00a74): never route the same inout
	// through both lobe evaluations. Per-lobe results, per-lobe energy, and a
	// luminance-weighted mean direction of the mixture.
	static const TCHAR* FnName = TEXT("RectGGXApproxLTC");
	TArray<FFuncDef> Defs;
	{
		int32 From = 0; FFuncDef D;
		while (FindNextFunctionDef(Source, FnName, From, D)) { Defs.Add(D); From = D.BodyClose + 1; }
	}
	int32 Num = 0;
	const FString CloneName = FString(FnName) + MLS_CloneSuffix;

	for (int32 i = Defs.Num() - 1; i >= 0; --i)
	{
		const FFuncDef& D = Defs[i];
		const FString ParamList = Source.Mid(D.ParamsOpen + 1, D.ParamsClose - D.ParamsOpen - 1);
		TArray<FString> P;
		ParseParamNames(ParamList, P);
		if (!P.Contains(TEXT("Roughness")) || !P.Contains(TEXT("OutMeanLightWorldDirection")))
		{
			continue; // thin forwarders stay untouched
		}
		const FString F0Expr = P.Contains(TEXT("SpecularColor")) ? FString(TEXT("SpecularColor")) : FString(TEXT("F0"));

		auto Args = [&P](const TCHAR* RWrap, const TCHAR* DirName) -> FString
		{
			FString A;
			for (int32 k = 0; k < P.Num(); ++k)
			{
				if (k) A += TEXT(", ");
				if (P[k] == TEXT("Roughness"))
				{
					A += FCString::Strlen(RWrap) ? FString::Printf(TEXT("%s(Roughness)"), RWrap) : FString(TEXT("Roughness"));
				}
				else if (P[k] == TEXT("OutMeanLightWorldDirection"))
				{
					A += DirName;
				}
				else
				{
					A += P[k];
				}
			}
			return A;
		};

		const FString OriginalFull = Source.Mid(D.Start, D.BodyClose - D.Start + 1);
		const FString Signature    = Source.Mid(D.Start, D.BodyOpen - D.Start);
		FString Clone = OriginalFull;
		{
			const int32 Off = D.NamePos - D.Start;
			Clone.RemoveAt(Off, FCString::Strlen(FnName));
			Clone.InsertAt(Off, CloneName);
		}

		FString W;
		W += TEXT("{\n#if MLS_ENABLED\n");
		W += TEXT("\tfloat3 MLS_Dir1 = OutMeanLightWorldDirection;\n");
		W += TEXT("\tfloat3 MLS_Dir2 = OutMeanLightWorldDirection;\n");
		W += FString::Printf(TEXT("\tfloat3 MLS_S1 = %s( %s );\n"), *CloneName, *Args(TEXT("MLS_R1"), TEXT("MLS_Dir1")));
		W += FString::Printf(TEXT("\tfloat3 MLS_S2 = %s( %s );\n"), *CloneName, *Args(TEXT("MLS_R2"), TEXT("MLS_Dir2")));
		W += TEXT("\tfloat MLS_Wr = MLS_LOBE2_W(Roughness);\n");
		W += TEXT("#if MLS_ENERGY_MIX\n");
		W += TEXT("\tfloat MLS_RectNoV = saturate(abs(dot(N, V)) + 1e-5);\n");
		W += FString::Printf(TEXT("\tMLS_S1 *= ComputeEnergyConservation(ComputeGGXSpecEnergyTerms(MLS_R1(Roughness), MLS_RectNoV, %s));\n"), *F0Expr);
		W += FString::Printf(TEXT("\tMLS_S2 *= ComputeEnergyConservation(ComputeGGXSpecEnergyTerms(MLS_R2(Roughness), MLS_RectNoV, %s));\n"), *F0Expr);
		W += TEXT("#endif\n");
		W += TEXT("\tfloat3 MLS_DirMix = (1.0 - MLS_Wr) * max(Luminance(MLS_S1), 1e-6) * MLS_Dir1 + MLS_Wr * max(Luminance(MLS_S2), 1e-6) * MLS_Dir2;\n");
		W += TEXT("\tOutMeanLightWorldDirection = dot(MLS_DirMix, MLS_DirMix) > 1e-12 ? normalize(MLS_DirMix) : MLS_Dir1;\n");
		W += TEXT("\treturn lerp(MLS_S1, MLS_S2, MLS_Wr);\n");
		W += TEXT("#else\n");
		W += FString::Printf(TEXT("\treturn %s( %s );\n"), *CloneName, *Args(TEXT(""), TEXT("OutMeanLightWorldDirection")));
		W += TEXT("#endif\n}");

		Source.RemoveAt(D.Start, D.BodyClose - D.Start + 1);
		Source.InsertAt(D.Start, Clone + TEXT("\n\n") + Signature + W);
		++Num;
	}
	return Num;
}

bool FMultiLobeShaderPatcher::PatchIndirectAO(const FString& OverlayDir, FString& OutWarning)
{
	// Split material visibility from geometry/screen AO. RGB material visibility
	// is applied to the diffuse contribution, never to the final scene-color multiply.
	const FString FilePath = OverlayDir / TEXT("Private/DiffuseIndirectComposite.usf");
	FString Src;
	if (!FFileHelper::LoadFileToString(Src, *FilePath))
	{
		OutWarning = TEXT("DiffuseIndirectComposite.usf not found — indirect AO stays raw.");
		return false;
	}
	if (Src.Contains(TEXT("MLS_NONLUMEN_RGB_INTERREFLECTION")))
	{
		return true;
	}
	static const TCHAR* AOAnchor =
		TEXT("FinalAmbientOcclusion = lerp(1.0f, Material.MaterialAO * DynamicAmbientOcclusion, AOMask * AmbientOcclusionStaticFraction);");
	static const TCHAR* DiffuseAnchor =
		TEXT("IndirectLighting.Diffuse = (DiffuseIndirectLighting * DiffuseColor + BackfaceDiffuseIndirectLighting) * Occlusion.DiffuseOcclusion * EnergyPreservationFactor + ShortRangeGI * DiffuseColor;");
	const int32 AOPos = Src.Find(AOAnchor, ESearchCase::CaseSensitive);
	const int32 DiffusePos = Src.Find(DiffuseAnchor, ESearchCase::CaseSensitive);
	if (AOPos == INDEX_NONE || DiffusePos == INDEX_NONE
		|| MLS_CountExactOccurrences(Src, AOAnchor) != 1
		|| MLS_CountExactOccurrences(Src, DiffuseAnchor) != 1)
	{
		OutWarning = TEXT("Non-Lumen indirect visibility exact-count failure: expected scalar AO and diffuse contribution anchors once each.");
		return false;
	}
	const FString AOReplacement = FString::Printf(
		TEXT("#if MLS_ENABLED && MLS_INDIRECT_VISIBILITY_MODE != 1\n")
		TEXT("\t\t// Only geometry/screen AO may affect the final scene-color multiply.\n")
		TEXT("\t\tFinalAmbientOcclusion = lerp(1.0f, DynamicAmbientOcclusion, AOMask * AmbientOcclusionStaticFraction);\n")
		TEXT("#else\n")
		TEXT("\t\t%s\n")
		TEXT("#endif"),
		AOAnchor);
	Src.RemoveAt(AOPos, FCString::Strlen(AOAnchor));
	Src.InsertAt(AOPos, AOReplacement);

	const int32 AdjustedDiffusePos = Src.Find(DiffuseAnchor, ESearchCase::CaseSensitive);
	const FString DiffuseReplacement = FString::Printf(
		TEXT("%s\n")
		TEXT("#if MLS_ENABLED && MLS_INDIRECT_VISIBILITY_MODE == 2 && DIM_APPLY_DIFFUSE_INDIRECT != DIM_APPLY_DIFFUSE_INDIRECT_SCREEN_PROBE_GATHER\n")
		TEXT("\t\t\t\tIndirectLighting.Diffuse *= MLS_InterreflectionVisibility(Material.MaterialAO, GBuffer.DiffuseColor); // MLS_NONLUMEN_RGB_INTERREFLECTION\n")
		TEXT("#endif"),
		DiffuseAnchor);
	Src.RemoveAt(AdjustedDiffusePos, FCString::Strlen(DiffuseAnchor));
	Src.InsertAt(AdjustedDiffusePos, DiffuseReplacement);
	{
		const int32 UsePos = Src.Find(TEXT("MLS_"), ESearchCase::CaseSensitive);
		int32 IncPos = INDEX_NONE, From2 = 0;
		while (true)
		{
			const int32 P = Src.Find(TEXT("#include"), ESearchCase::CaseSensitive, ESearchDir::FromStart, From2);
			if (P == INDEX_NONE || P > UsePos) { break; }
			IncPos = P; From2 = P + 8;
		}
		if (IncPos != INDEX_NONE)
		{
			int32 LineEnd = IncPos;
			while (LineEnd < Src.Len() && Src[LineEnd] != '\n') ++LineEnd;
			Src.InsertAt(LineEnd, FString::Printf(TEXT("\n%s %s"), MLS_ConfigInclude, MLS_PatchMarker));
		}
	}
	if (!FFileHelper::SaveStringToFile(Src, *FilePath))
	{
		OutWarning = TEXT("Failed to save patched DiffuseIndirectComposite.usf");
		return false;
	}
	UE_LOG(LogMultiLobeSpec, Log, TEXT("Non-Lumen indirect: material RGB visibility separated from scalar geometry AO (2/2 anchors)."));
	return true;
}

bool FMultiLobeShaderPatcher::PatchLumenIndirectAO(const FString& OverlayDir, FString& OutWarning)
{
	// Byte-exact anchors from UE 5.7 LumenScreenProbeGather.usf. DirectOnly removes
	// material visibility; RGB mode applies the per-pixel albedo correction while
	// retaining screen/short-range/geometric occlusion.
	const FString FilePath = OverlayDir / TEXT("Private/Lumen/LumenScreenProbeGather.usf");
	FString Src;
	if (!FFileHelper::LoadFileToString(Src, *FilePath))
	{
		OutWarning = TEXT("LumenScreenProbeGather.usf not found — Lumen keeps darkening indirect with material AO.");
		return false;
	}
	if (Src.Contains(TEXT("MLS_LUMEN_RGB_INTERREFLECTION")))
	{
		return true;
	}

	int32 Num = 0;

	// A) per-ray occlusion bit: declaration must survive, value forced to false.
	{
		static const TCHAR* From = TEXT("bool bIsBentNormalOccluded = ApplyMaterialAO > 0 ? ((Material.DiffuseIndirectSampleOcclusion & (1u << PixelRayIndex)) != 0) : false;");
		const int32 Pos = Src.Find(From, ESearchCase::CaseSensitive);
		if (Pos != INDEX_NONE)
		{
			const FString To = FString::Printf(
				TEXT("#if MLS_ENABLED && MLS_INDIRECT_VISIBILITY_MODE == 0\n")
				TEXT("\t\t\t\t\t\tbool bIsBentNormalOccluded = false; // material AO = micro-visibility only\n")
				TEXT("#else\n")
				TEXT("\t\t\t\t\t\t%s\n")
				TEXT("#endif"), From);
			Src.RemoveAt(Pos, FCString::Strlen(From));
			Src.InsertAt(Pos, To);
			++Num;
		}
	}
	// B) Directional sample-mask permutation already contains raw material AO;
	// RGB mode converts raw -> interreflection by a ratio correction.
	{
		static const TCHAR* From = TEXT("DiffuseLighting *= AOMultiBounce(min(Material.DiffuseAlbedo, MaxAOMultibounceAlbedo), Material.MaterialAO) * (Material.MaterialAO > 0.0 ? rcp(Material.MaterialAO) : 0.0);");
		const int32 Pos = Src.Find(From, ESearchCase::CaseSensitive);
		if (Pos != INDEX_NONE)
		{
			const FString To = FString::Printf(
				TEXT("#if MLS_ENABLED && MLS_INDIRECT_VISIBILITY_MODE == 0\n")
				TEXT("\t\t\t\t// Direct-only diagnostic: no material visibility in indirect diffuse.\n")
				TEXT("#elif MLS_ENABLED && MLS_INDIRECT_VISIBILITY_MODE == 2\n")
				TEXT("\t\t\t\tDiffuseLighting *= MLS_InterreflectionVisibility(Material.MaterialAO, Material.GBufferData.DiffuseColor) * (Material.MaterialAO > 0.0 ? rcp(Material.MaterialAO) : 0.0); // MLS_LUMEN_RGB_INTERREFLECTION\n")
				TEXT("#else\n")
				TEXT("\t\t\t\t%s\n")
				TEXT("#endif"), From);
			Src.RemoveAt(Pos, FCString::Strlen(From));
			Src.InsertAt(Pos, To);
			++Num;
		}
	}
	// C) Standard project permutation: material RGB visibility is applied once.
	{
		static const TCHAR* From = TEXT("DiffuseLighting *= AOMultiBounce(min(Material.DiffuseAlbedo, MaxAOMultibounceAlbedo), Material.MaterialAO);");
		const int32 Pos = Src.Find(From, ESearchCase::CaseSensitive);
		if (Pos != INDEX_NONE)
		{
			const FString To = FString::Printf(
				TEXT("#if MLS_ENABLED && MLS_INDIRECT_VISIBILITY_MODE == 0\n")
				TEXT("\t\t\t// Direct-only diagnostic: no material visibility in indirect diffuse.\n")
				TEXT("#elif MLS_ENABLED && MLS_INDIRECT_VISIBILITY_MODE == 2\n")
				TEXT("\t\t\tDiffuseLighting *= MLS_InterreflectionVisibility(Material.MaterialAO, Material.GBufferData.DiffuseColor); // MLS_LUMEN_RGB_INTERREFLECTION\n")
				TEXT("#else\n")
				TEXT("\t\t\t%s\n")
				TEXT("#endif"), From);
			Src.RemoveAt(Pos, FCString::Strlen(From));
			Src.InsertAt(Pos, To);
			++Num;
		}
	}

	if (Num != 3)
	{
		OutWarning = FString::Printf(
			TEXT("Lumen indirect AO exact-count failure: expected 3 anchors, found %d; file left untouched."), Num);
		return false;
	}

	{
		const int32 UsePos = Src.Find(TEXT("MLS_"), ESearchCase::CaseSensitive);
		int32 IncPos = INDEX_NONE, From2 = 0;
		while (true)
		{
			const int32 P = Src.Find(TEXT("#include"), ESearchCase::CaseSensitive, ESearchDir::FromStart, From2);
			if (P == INDEX_NONE || P > UsePos) { break; }
			IncPos = P; From2 = P + 8;
		}
		if (IncPos != INDEX_NONE)
		{
			int32 LineEnd = IncPos;
			while (LineEnd < Src.Len() && Src[LineEnd] != '\n') ++LineEnd;
			Src.InsertAt(LineEnd, FString::Printf(TEXT("\n%s %s"), MLS_ConfigInclude, MLS_PatchMarker));
		}
	}

	if (!FFileHelper::SaveStringToFile(Src, *FilePath))
	{
		OutWarning = TEXT("Failed to save patched LumenScreenProbeGather.usf");
		return false;
	}
	UE_LOG(LogMultiLobeSpec, Log, TEXT("Lumen indirect diffuse: %d/3 material-visibility sites support DirectOnly/Raw/RGB modes."), Num);
	return true;
}

bool FMultiLobeShaderPatcher::PatchSkyLightIndirectAO(const FString& OverlayDir, FString& OutWarning)
{
	const FString FilePath = OverlayDir / TEXT("Private/SkyLightingDiffuseShared.ush");
	FString Source;
	if (!FFileHelper::LoadFileToString(Source, *FilePath))
	{
		OutWarning = TEXT("SkyLightingDiffuseShared.ush not found.");
		return false;
	}
	if (Source.Contains(TEXT("MLS_SKYLIGHT_RGB_INTERREFLECTION")))
	{
		return true;
	}

	auto ReplaceExact = [&Source](const TCHAR* From, const TCHAR* To) -> bool
	{
		if (MLS_CountExactOccurrences(Source, From) != 1)
		{
			return false;
		}
		Source.ReplaceInline(From, To, ESearchCase::CaseSensitive);
		return true;
	};

	int32 Count = 0;
	Count += ReplaceExact(
		TEXT("float  SkyDiffuseLookUpMul;"),
		TEXT("float3 SkyDiffuseLookUpMul;")) ? 1 : 0;
	Count += ReplaceExact(
		TEXT("FSkyLightVisibilityData GetSkyLightVisibilityData(float3 SkyLightingNormal, const float3 WorldNormal, const float GBufferAO, float ScreenAO, const float3 BentNormal)"),
		TEXT("FSkyLightVisibilityData GetSkyLightVisibilityData(float3 SkyLightingNormal, const float3 WorldNormal, const float GBufferAO, float ScreenAO, const float3 BentNormal, const float3 DiffuseAlbedo)")) ? 1 : 0;
	Count += ReplaceExact(
		TEXT("// Combine with other AO sources"),
		TEXT("// Separate material RGB visibility from Screen/DFAO geometry visibility.\n")
		TEXT("float3 MLS_MaterialVisibility = GBufferAO.xxx;\n")
		TEXT("#if MLS_ENABLED && MLS_INDIRECT_VISIBILITY_MODE == 0\n")
		TEXT("\tMLS_MaterialVisibility = 1.0.xxx;\n")
		TEXT("#elif MLS_ENABLED && MLS_INDIRECT_VISIBILITY_MODE == 2\n")
		TEXT("\tMLS_MaterialVisibility = MLS_InterreflectionVisibility(GBufferAO, DiffuseAlbedo); // MLS_SKYLIGHT_RGB_INTERREFLECTION\n")
		TEXT("#endif\n")
		TEXT("float3 MLS_FinalVisibility = 1.0.xxx;\n\n")
		TEXT("// Combine with other AO sources")) ? 1 : 0;
	Count += ReplaceExact(
		TEXT("SkyVisibility = min(SkyVisibility, min(GBufferAO, ScreenAO));"),
		TEXT("MLS_FinalVisibility = min(MLS_MaterialVisibility, min(SkyVisibility, ScreenAO).xxx);")) ? 1 : 0;
	Count += ReplaceExact(
		TEXT("SkyVisibility = SkyVisibility * min(GBufferAO, ScreenAO);"),
		TEXT("MLS_FinalVisibility = MLS_MaterialVisibility * SkyVisibility * ScreenAO;")) ? 1 : 0;
	Count += ReplaceExact(
		TEXT("SkyVisData.SkyDiffuseLookUpMul = SkyVisibility * DotProductFactor;"),
		TEXT("SkyVisData.SkyDiffuseLookUpMul = MLS_FinalVisibility * DotProductFactor;")) ? 1 : 0;
	Count += ReplaceExact(
		TEXT("SkyVisData.SkyDiffuseLookUpAdd = (1 - SkyVisibility) * OcclusionTintAndMinOcclusion.xyz;"),
		TEXT("SkyVisData.SkyDiffuseLookUpAdd = (1.0.xxx - MLS_FinalVisibility) * OcclusionTintAndMinOcclusion.xyz;")) ? 1 : 0;
	Count += ReplaceExact(
		TEXT("GetSkyLightVisibilityData(SkyLightingNormal, GBuffer.WorldNormal, GBuffer.GBufferAO, AmbientOcclusion, BentNormal);"),
		TEXT("GetSkyLightVisibilityData(SkyLightingNormal, GBuffer.WorldNormal, GBuffer.GBufferAO, AmbientOcclusion, BentNormal, GBuffer.DiffuseColor);")) ? 1 : 0;

	if (Count != 8)
	{
		OutWarning = FString::Printf(TEXT("Skylight indirect visibility exact-count failure: expected 8 anchors, found %d."), Count);
		return false;
	}
	EnsureConfigInclude(Source);
	if (!FFileHelper::SaveStringToFile(Source, *FilePath))
	{
		OutWarning = TEXT("Failed to save patched SkyLightingDiffuseShared.ush");
		return false;
	}
	UE_LOG(LogMultiLobeSpec, Log, TEXT("Skylight indirect: RGB material visibility separated from Screen/DFAO geometry visibility (8/8 anchors)."));
	return true;
}

bool FMultiLobeShaderPatcher::PatchRawMaterialVisibilityTransport(
	const FString& OverlayDir,
	FString& OutError)
{
	const FString FilePath = OverlayDir / TEXT("Private/BasePassPixelShader.usf");
	FString Source;
	if (!FFileHelper::LoadFileToString(Source, *FilePath))
	{
		OutError = TEXT("Unable to load BasePassPixelShader.usf for raw MaterialAO transport.");
		return false;
	}
	if (Source.Contains(TEXT("MLS_RAW_MATERIAL_VISIBILITY_TRANSPORT")))
	{
		return true;
	}

	const FString RawAnchor = TEXT("float MaterialAO = GetMaterialAmbientOcclusion(PixelMaterialInputs);");
	const FString StoreAnchor = TEXT("GBuffer.GBufferAO = AOMultiBounce( Luminance( GBuffer.SpecularColor ), ShadingOcclusion.SpecOcclusion ).g;");
	const int32 RawCount = MLS_CountExactOccurrences(Source, RawAnchor);
	const int32 StoreCount = MLS_CountExactOccurrences(Source, StoreAnchor);
	if (RawCount != 1 || StoreCount != 1)
	{
		OutError = FString::Printf(
			TEXT("Raw MaterialAO transport exact-count failure: raw source expected 1/found %d, GBufferAO store expected 1/found %d; file left untouched."),
			RawCount,
			StoreCount);
		return false;
	}

	const FString Replacement = FString::Printf(
		TEXT("#if MLS_ENABLED && MLS_RAW_MATERIAL_VISIBILITY_TRANSPORT\n")
		TEXT("\t#if GBUFFER_HAS_DIFFUSE_SAMPLE_OCCLUSION || ALLOW_STATIC_LIGHTING || SUBSTRATE_ENABLED\n")
		TEXT("\t\t#error MultiLobeSpec direct/indirect material visibility requires lossless legacy MaterialAO transport\n")
		TEXT("\t#endif\n")
		TEXT("\tGBuffer.GBufferAO = MaterialAO; // MLS_RAW_MATERIAL_VISIBILITY_TRANSPORT %s\n")
		TEXT("#else\n")
		TEXT("\t%s\n")
		TEXT("#endif"),
		MLS_PatchMarker,
		*StoreAnchor);
	Source.ReplaceInline(*StoreAnchor, *Replacement, ESearchCase::CaseSensitive);
	EnsureConfigInclude(Source);
	if (!FFileHelper::SaveStringToFile(Source, *FilePath))
	{
		OutError = TEXT("Failed to save BasePassPixelShader.usf raw MaterialAO transport patch.");
		return false;
	}
	UE_LOG(LogMultiLobeSpec, Log,
		TEXT("Raw MaterialAO transport: 1/1 source and 1/1 GBufferAO store patched; incompatible compile permutations hard-error."));
	return true;
}

int32 FMultiLobeShaderPatcher::ReplaceIdentifierCalls(FString& Source, const FString& From, const FString& To)
{
	TArray<int32> Hits;
	int32 Pos = 0;
	while (true)
	{
		Pos = Source.Find(From, ESearchCase::CaseSensitive, ESearchDir::FromStart, Pos);
		if (Pos == INDEX_NONE)
		{
			break;
		}
		const int32 After = Pos + From.Len();

		bool bBoundaryL = (Pos == 0) || !(FChar::IsAlnum(Source[Pos - 1]) || Source[Pos - 1] == '_');
		bool bBoundaryR = (After >= Source.Len()) || !(FChar::IsAlnum(Source[After]) || Source[After] == '_');

		int32 P = After;
		while (P < Source.Len() && FChar::IsWhitespace(Source[P])) ++P;
		const bool bIsCall = (P < Source.Len() && Source[P] == '(');

		if (bBoundaryL && bBoundaryR && bIsCall)
		{
			Hits.Add(Pos);
		}
		Pos = After;
	}

	for (int32 i = Hits.Num() - 1; i >= 0; --i)
	{
		Source.RemoveAt(Hits[i], From.Len());
		Source.InsertAt(Hits[i], To);
	}
	return Hits.Num();
}

void FMultiLobeShaderPatcher::EnsureConfigInclude(FString& Source)
{
	if (Source.Contains(MLS_ConfigInclude))
	{
		return;
	}

	const FString Injection = FString::Printf(TEXT("\n%s %s\n"), MLS_ConfigInclude, MLS_PatchMarker);

	const int32 PragmaIdx = Source.Find(TEXT("#pragma once"), ESearchCase::CaseSensitive);
	if (PragmaIdx != INDEX_NONE)
	{
		int32 LineEnd = PragmaIdx;
		while (LineEnd < Source.Len() && Source[LineEnd] != '\n') ++LineEnd;
		Source.InsertAt(LineEnd, Injection);
	}
	else
	{
		Source.InsertAt(0, Injection);
	}
}

// ---------------------------------------------------------------------------

bool FMultiLobeShaderPatcher::WriteAgXFile(const FString& OverlayDir, FString& OutError)
{
	// Self-contained AgX (Base + Punchy look), no dependency on engine color
	// matrices. Minimal-AgX inset/outset (Troy Sobotka) transposed for HLSL
	// row-major mul(M, v). Space adapters are define-switched because the
	// exact color space at the FilmToneMap call site differs between engine
	// versions — see README "Калибровка AgX".
	static const TCHAR* AgX =
		TEXT("// Generated by MultiLobeSpec. AgX tonemapper (Base / Punchy). // MLS_PATCHED\n")
		TEXT("#pragma once\n")
		TEXT("#include \"/Engine/Private/MultiLobeSpecConfig.ush\"\n")
		TEXT("\n")
		TEXT("#ifndef MLS_TONEMAP\n")
		TEXT("#define MLS_TONEMAP 0\n")
		TEXT("#endif\n")
		TEXT("\n")
		TEXT("static const float3x3 MLS_AP1_2_sRGB_MAT = {\n")
		TEXT("\t 1.70505099, -0.62179212, -0.08325887,\n")
		TEXT("\t-0.13025642,  1.14080474, -0.01054832,\n")
		TEXT("\t-0.02400336, -0.12896898,  1.15297234 };\n")
		TEXT("\n")
		TEXT("static const float3x3 MLS_sRGB_2_AP1_MAT = {\n")
		TEXT("\t 0.61319000, 0.33951000, 0.04737000,\n")
		TEXT("\t 0.07021000, 0.91634000, 0.01345000,\n")
		TEXT("\t 0.02062000, 0.10957000, 0.86961000 };\n")
		TEXT("\n")
		TEXT("float3 MLS_AgXContrast(float3 x)\n")
		TEXT("{\n")
		TEXT("\tfloat3 x2 = x * x;\n")
		TEXT("\tfloat3 x4 = x2 * x2;\n")
		TEXT("\treturn + 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4\n")
		TEXT("\t       - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232;\n")
		TEXT("}\n")
		TEXT("\n")
		TEXT("float3 MLS_AgXBase(float3 Val)\n")
		TEXT("{\n")
		TEXT("\tconst float3x3 AgXInset = {\n")
		TEXT("\t\t0.842479062253094,  0.0423282422610123, 0.0423756549057051,\n")
		TEXT("\t\t0.0784335999999992, 0.878468636469772,  0.0784336,\n")
		TEXT("\t\t0.0792237451477643, 0.0791661274605434, 0.879142973793104 };\n")
		TEXT("\tconst float MinEV = -12.47393;\n")
		TEXT("\tconst float MaxEV =   4.026069;\n")
		TEXT("\n")
		TEXT("\tVal = mul(AgXInset, Val);\n")
		TEXT("\tVal = clamp(log2(max(Val, 1e-10)), MinEV, MaxEV);\n")
		TEXT("\tVal = (Val - MinEV) / (MaxEV - MinEV);\n")
		TEXT("\treturn MLS_AgXContrast(Val);\n")
		TEXT("}\n")
		TEXT("\n")
		TEXT("float3 MLS_AgXLookPunchy(float3 Val)\n")
		TEXT("{\n")
		TEXT("\t// Blender's AgX Punchy: power 1.35, saturation 1.4.\n")
		TEXT("\t// ASC CDL: saturation uses luma of the SOP-transformed value,\n")
		TEXT("\t// so luma is computed AFTER pow (community-flagged fix).\n")
		TEXT("\tVal = pow(max(Val, 0.0), 1.35);\n")
		TEXT("\tconst float3 LumaW = float3(0.2126, 0.7152, 0.0722);\n")
		TEXT("\tfloat Luma = dot(Val, LumaW);\n")
		TEXT("\treturn Luma + 1.4 * (Val - Luma);\n")
		TEXT("}\n")
		TEXT("\n")
		TEXT("float3 MLS_AgXOutset(float3 Val)\n")
		TEXT("{\n")
		TEXT("\tconst float3x3 AgXOutsetMat = {\n")
		TEXT("\t\t 1.19687900512017,   -0.0528968517574562, -0.0529716355144438,\n")
		TEXT("\t\t-0.0980208811401368,  1.15190312990417,   -0.0980434501171241,\n")
		TEXT("\t\t-0.0990297440797205, -0.0989611768448433,  1.15107367264116 };\n")
		TEXT("\tVal = mul(AgXOutsetMat, Val);\n")
		TEXT("#if MLS_AGX_OUTPUT_LINEARIZE\n")
		TEXT("\t// AgX chain lands ~2.2-encoded; the engine tonemap slot expects LINEAR\n")
		TEXT("\t// (display encoding happens later in the pipeline).\n")
		TEXT("\tVal = pow(max(Val, 0.0), 2.2);\n")
		TEXT("#endif\n")
		TEXT("\treturn Val;\n")
		TEXT("}\n")
		TEXT("\n")
		TEXT("float3 MLS_GamutCompress(float3 C)\n")
		TEXT("{\n")
		TEXT("\t// Lumen/AP1 can deliver colors outside the sRGB gamut (negative\n")
		TEXT("\t// channels after the primaries conversion). Crushing them per-channel\n")
		TEXT("\t// produces oversaturated blowouts (green blobs in reflections), so\n")
		TEXT("\t// instead desaturate toward luma just enough to reach the gamut edge.\n")
		TEXT("\tconst float3 LumaW = float3(0.2126, 0.7152, 0.0722);\n")
		TEXT("\tfloat L = dot(C, LumaW);\n")
		TEXT("\tfloat Mn = min(C.r, min(C.g, C.b));\n")
		TEXT("\tif (Mn < 0.0 && L > 1e-6)\n")
		TEXT("\t{\n")
		TEXT("\t\tfloat S = L / (L - Mn);\n")
		TEXT("\t\tC = L + (C - L) * S;\n")
		TEXT("\t}\n")
		TEXT("\treturn max(C, 0.0);\n")
		TEXT("}\n")
		TEXT("\n")
		TEXT("float3 MLS_ApplyAgX(float3 WorkingColor)\n")
		TEXT("{\n")
		TEXT("#if MLS_AGX_INPUT_AP1\n")
		TEXT("\tWorkingColor = mul(MLS_AP1_2_sRGB_MAT, WorkingColor);\n")
		TEXT("#endif\n")
		TEXT("\tWorkingColor = MLS_GamutCompress(WorkingColor);\n")
		TEXT("\tfloat3 V = MLS_AgXBase(WorkingColor);\n")
		TEXT("#if MLS_TONEMAP == 2\n")
		TEXT("\tV = MLS_AgXLookPunchy(V);\n")
		TEXT("#endif\n")
		TEXT("\tV = MLS_AgXOutset(V);\n")
		TEXT("#if MLS_AGX_OUTPUT_AP1\n")
		TEXT("\tV = mul(MLS_sRGB_2_AP1_MAT, V);\n")
		TEXT("#endif\n")
		TEXT("\treturn V;\n")
		TEXT("}\n")
		TEXT("\n")
		TEXT("// --- GT tonemapper (Uchimura, CEDEC 2017: 'HDR Theory and Practice', Gran Turismo) ---\n")
		TEXT("float MLS_UchimuraChannel(float x, float Pk, float Ak, float Mk, float Lk, float Ck, float Bk)\n")
		TEXT("{\n")
		TEXT("\tfloat l0 = ((Pk - Mk) * Lk) / Ak;\n")
		TEXT("\tfloat S0 = Mk + l0;\n")
		TEXT("\tfloat S1 = Mk + Ak * l0;\n")
		TEXT("\tfloat C2 = (Ak * Pk) / max(Pk - S1, 1e-6);\n")
		TEXT("\tfloat CP = -C2 / Pk;\n")
		TEXT("\tfloat w0 = 1.0 - smoothstep(0.0, Mk, x);\n")
		TEXT("\tfloat w2 = step(S0, x);\n")
		TEXT("\tfloat w1 = 1.0 - w0 - w2;\n")
		TEXT("\tfloat T = Mk * pow(max(x / Mk, 0.0), Ck) + Bk; // toe (black tightness Ck, pedestal Bk)\n")
		TEXT("\tfloat S = Pk - (Pk - S1) * exp(CP * (x - S0));  // shoulder\n")
		TEXT("\tfloat Ln = Mk + Ak * (x - Mk);                  // linear section\n")
		TEXT("\treturn T * w0 + Ln * w1 + S * w2;\n")
		TEXT("}\n")
		TEXT("\n")
		TEXT("float3 MLS_ApplyGT(float3 WorkingColor)\n")
		TEXT("{\n")
		TEXT("#if MLS_AGX_INPUT_AP1\n")
		TEXT("\tWorkingColor = mul(MLS_AP1_2_sRGB_MAT, WorkingColor);\n")
		TEXT("#endif\n")
		TEXT("\tWorkingColor = MLS_GamutCompress(WorkingColor);\n")
		TEXT("#if MLS_TONEMAP == 4\n")
		TEXT("\t// GT High Contrast: steeper linear section, tighter toe. Tweak here (regenerated on overlay rebuild).\n")
		TEXT("\tconst float GTp = 1.0, GTa = 1.20, GTm = 0.18, GTl = 0.35, GTc = 1.45, GTb = 0.0;\n")
		TEXT("#else\n")
		TEXT("\t// GT reference parameters (Uchimura defaults).\n")
		TEXT("\tconst float GTp = 1.0, GTa = 1.00, GTm = 0.22, GTl = 0.40, GTc = 1.33, GTb = 0.0;\n")
		TEXT("#endif\n")
		TEXT("\tfloat3 V;\n")
		TEXT("\tV.x = MLS_UchimuraChannel(WorkingColor.x, GTp, GTa, GTm, GTl, GTc, GTb);\n")
		TEXT("\tV.y = MLS_UchimuraChannel(WorkingColor.y, GTp, GTa, GTm, GTl, GTc, GTb);\n")
		TEXT("\tV.z = MLS_UchimuraChannel(WorkingColor.z, GTp, GTa, GTm, GTl, GTc, GTb);\n")
		TEXT("\t// GT outputs display-linear directly — no 2.2 step (unlike the AgX chain).\n")
		TEXT("#if MLS_AGX_OUTPUT_AP1\n")
		TEXT("\tV = mul(MLS_sRGB_2_AP1_MAT, V);\n")
		TEXT("#endif\n")
		TEXT("\treturn V;\n")
		TEXT("}\n")
		TEXT("\n")
		TEXT("half3 MLS_FilmToneMap(half3 WorkingColor)\n")
		TEXT("{\n")
		TEXT("#if MLS_TONEMAP == 0\n")
		TEXT("\treturn FilmToneMap(WorkingColor);\n")
		TEXT("#elif MLS_TONEMAP == 1 || MLS_TONEMAP == 2\n")
		TEXT("\treturn (half3)MLS_ApplyAgX((float3)WorkingColor);\n")
		TEXT("#else\n")
		TEXT("\treturn (half3)MLS_ApplyGT((float3)WorkingColor);\n")
		TEXT("#endif\n")
		TEXT("}\n");

	const FString Path = OverlayDir / TEXT("Private") / MLS_AgXFileName;
	if (!FFileHelper::SaveStringToFile(FString(AgX), *Path))
	{
		OutError = FString::Printf(TEXT("Failed to write '%s'"), *Path);
		return false;
	}
	return true;
}

bool FMultiLobeShaderPatcher::PatchTonemapper(const FString& OverlayDir, FString& OutWarning)
{
	const FString FilePath = OverlayDir / TEXT("Private/PostProcessCombineLUTs.usf");
	FString Src;
	if (!FFileHelper::LoadFileToString(Src, *FilePath))
	{
		OutWarning = TEXT("PostProcessCombineLUTs.usf not found — tonemapper patch skipped.");
		return false;
	}
	if (Src.Contains(MLS_AgXInclude))
	{
		return true; // already patched
	}

	// Our include must come AFTER TonemapCommon.ush so FilmToneMap is declared
	// for the fallback path.
	const int32 TCIdx = Src.Find(TEXT("TonemapCommon.ush"), ESearchCase::IgnoreCase);
	if (TCIdx == INDEX_NONE)
	{
		OutWarning = TEXT("'TonemapCommon.ush' include not found in PostProcessCombineLUTs.usf — tonemapper patch skipped.");
		return false;
	}
	int32 LineEnd = TCIdx;
	while (LineEnd < Src.Len() && Src[LineEnd] != '\n') ++LineEnd;
	Src.InsertAt(LineEnd, FString::Printf(TEXT("\n%s %s"), MLS_AgXInclude, MLS_PatchMarker));

	const int32 NumCalls = ReplaceIdentifierCalls(Src, TEXT("FilmToneMap"), TEXT("MLS_FilmToneMap"));
	if (NumCalls == 0)
	{
		OutWarning = TEXT("No FilmToneMap call sites found in PostProcessCombineLUTs.usf — tonemapper patch skipped.");
		return false;
	}

	// The engine divides the final device color by 1.05 as an ACES-specific
	// compensation. Left in place, custom tonemappers come out ~5% darker
	// ("grey layer" reports). Disable it whenever MLS tonemapping is active.
	{
		const int32 CompIdx = Src.Find(TEXT("OutDeviceColor / 1.05"), ESearchCase::CaseSensitive);
		if (CompIdx != INDEX_NONE)
		{
			int32 LineStart = CompIdx;
			while (LineStart > 0 && Src[LineStart - 1] != '\n') --LineStart;
			int32 LineEnd2 = CompIdx;
			while (LineEnd2 < Src.Len() && Src[LineEnd2] != '\n') ++LineEnd2;

			const FString OrigLine = Src.Mid(LineStart, LineEnd2 - LineStart);
			const FString NewBlock = FString::Printf(
				TEXT("#if MLS_TONEMAP == 0\n%s\n#else\n\tOutColor.rgb = OutDeviceColor;\n#endif"),
				*OrigLine);

			Src.RemoveAt(LineStart, LineEnd2 - LineStart);
			Src.InsertAt(LineStart, NewBlock);
		}
		else
		{
			UE_LOG(LogMultiLobeSpec, Warning,
				TEXT("'OutDeviceColor / 1.05' line not found — custom tonemap output will be ~5%% darker than reference."));
		}
	}

	if (!FFileHelper::SaveStringToFile(Src, *FilePath))
	{
		OutWarning = FString::Printf(TEXT("Failed to save patched '%s'"), *FilePath);
		return false;
	}

	UE_LOG(LogMultiLobeSpec, Log, TEXT("Tonemapper patched: %d FilmToneMap call site(s) rerouted."), NumCalls);
	return true;
}

// ---------------------------------------------------------------------------

bool FMultiLobeShaderPatcher::WriteLumenDualBlurFile(const FString& OverlayDir, FString& OutError)
{
	// Verified against real 5.7 LumenReflections.usf (raw uniform E, float2 Alpha
	// overload, tangent-space V/H). v0.6.1 adds the MIXTURE PDF (review v2 \u00a75):
	// p_mix = (1-W) p1 + W p2, computed as a density ratio at the sampled H with
	// self-contained VNDF math (G1 * D; VoH/NoV factors cancel in the ratio).
	static const TCHAR* Code =
		TEXT("// Generated by MultiLobeSpec. Lumen dual-blur sampling wrapper. // MLS_PATCHED\n")
		TEXT("#pragma once\n")
		TEXT("#include \"/Engine/Private/MultiLobeSpecConfig.ush\"\n")
		TEXT("\n")
		TEXT("#ifndef MLS_LUMEN_DUALBLUR\n")
		TEXT("#define MLS_LUMEN_DUALBLUR 0\n")
		TEXT("#endif\n")
		TEXT("\n")
		TEXT("float MLS_VNDF_G1(float A2, float NoV)\n")
		TEXT("{\n")
		TEXT("\treturn 2.0 * NoV / max(NoV + sqrt(A2 + (1.0 - A2) * NoV * NoV), 1e-8);\n")
		TEXT("}\n")
		TEXT("float MLS_VNDF_D(float A2, float NoH)\n")
		TEXT("{\n")
		TEXT("\tfloat T = NoH * NoH * (A2 - 1.0) + 1.0;\n")
		TEXT("\treturn A2 / max(T * T, 1e-8); // 1/PI cancels in the density ratio\n")
		TEXT("}\n")
		TEXT("\n")
		TEXT("// Primary overload used by 5.7 Lumen ray generation: Alpha = roughness^2 per axis.\n")
		TEXT("float4 MLS_ImportanceSampleVisibleGGX(float2 E, float2 Alpha, float3 V)\n")
		TEXT("{\n")
		TEXT("#if MLS_ENABLED && MLS_LUMEN_DUALBLUR\n")
		TEXT("\tfloat R1v = sqrt(max(max(Alpha.x, Alpha.y), 1e-8));\n")
		TEXT("\tfloat W = MLS_LUMEN_BLUR_WEIGHT * saturate(R1v * MLS_CORE_FADE);\n")
		TEXT("\tfloat2 Alpha1 = Alpha;\n")
		TEXT("\tfloat R2v = MLS_R2(R1v);\n")
		TEXT("\tfloat2 Alpha2 = min(Alpha * ((R2v * R2v) / max(R1v * R1v, 1e-8)), 1.0);\n")
		TEXT("\tbool bWide = false;\n")
		TEXT("\tif (W > 1e-3)\n")
		TEXT("\t{\n")
		TEXT("\t\tif (E.x < W) { E.x = E.x / W; Alpha = Alpha2; bWide = true; }\n")
		TEXT("\t\telse { E.x = (E.x - W) / (1.0 - W); }\n")
		TEXT("\t}\n")
		TEXT("\tfloat4 S = ImportanceSampleVisibleGGX(E, Alpha, V);\n")
		TEXT("\tif (W > 1e-3)\n")
		TEXT("\t{\n")
		TEXT("\t\t// Isotropic mixture PDF (call site passes Alpha.xx; anisotropic mixing\n")
		TEXT("\t\t// would need per-axis densities — documented limitation).\n")
		TEXT("\t\tfloat NoV = max(V.z, 1e-4);\n")
		TEXT("\t\tfloat NoH = saturate(S.z);\n")
		TEXT("\t\tfloat A2_1 = Alpha1.x * Alpha1.x;\n")
		TEXT("\t\tfloat A2_2 = Alpha2.x * Alpha2.x;\n")
		TEXT("\t\tfloat P1 = MLS_VNDF_G1(A2_1, NoV) * MLS_VNDF_D(A2_1, NoH);\n")
		TEXT("\t\tfloat P2 = MLS_VNDF_G1(A2_2, NoV) * MLS_VNDF_D(A2_2, NoH);\n")
		TEXT("\t\tfloat PSel = bWide ? P2 : P1;\n")
		TEXT("\t\tfloat PMix = (1.0 - W) * P1 + W * P2;\n")
		TEXT("\t\tS.w = S.w * PMix / max(PSel, 1e-8);\n")
		TEXT("\t}\n")
		TEXT("\treturn S;\n")
		TEXT("#else\n")
		TEXT("\treturn ImportanceSampleVisibleGGX(E, Alpha, V);\n")
		TEXT("#endif\n")
		TEXT("}\n")
		TEXT("\n")
		TEXT("// Scalar overload kept for other call sites / engine drift (a2 = alpha^2 = roughness^4).\n")
		TEXT("float4 MLS_ImportanceSampleVisibleGGX(float2 E, float a2, float3 V)\n")
		TEXT("{\n")
		TEXT("#if MLS_ENABLED && MLS_LUMEN_DUALBLUR\n")
		TEXT("\tfloat R1v = pow(max(a2, 1e-8), 0.25);\n")
		TEXT("\tfloat W = MLS_LUMEN_BLUR_WEIGHT * saturate(R1v * MLS_CORE_FADE);\n")
		TEXT("\tfloat A2_1 = a2;\n")
		TEXT("\tfloat R2v = MLS_R2(R1v);\n")
		TEXT("\tfloat A2_2 = R2v * R2v * R2v * R2v;\n")
		TEXT("\tbool bWide = false;\n")
		TEXT("\tif (W > 1e-3)\n")
		TEXT("\t{\n")
		TEXT("\t\tif (E.x < W) { E.x = E.x / W; a2 = A2_2; bWide = true; }\n")
		TEXT("\t\telse { E.x = (E.x - W) / (1.0 - W); }\n")
		TEXT("\t}\n")
		TEXT("\tfloat4 S = ImportanceSampleVisibleGGX(E, a2, V);\n")
		TEXT("\tif (W > 1e-3)\n")
		TEXT("\t{\n")
		TEXT("\t\tfloat NoV = max(V.z, 1e-4);\n")
		TEXT("\t\tfloat NoH = saturate(S.z);\n")
		TEXT("\t\tfloat P1 = MLS_VNDF_G1(A2_1, NoV) * MLS_VNDF_D(A2_1, NoH);\n")
		TEXT("\t\tfloat P2 = MLS_VNDF_G1(A2_2, NoV) * MLS_VNDF_D(A2_2, NoH);\n")
		TEXT("\t\tfloat PSel = bWide ? P2 : P1;\n")
		TEXT("\t\tfloat PMix = (1.0 - W) * P1 + W * P2;\n")
		TEXT("\t\tS.w = S.w * PMix / max(PSel, 1e-8);\n")
		TEXT("\t}\n")
		TEXT("\treturn S;\n")
		TEXT("#else\n")
		TEXT("\treturn ImportanceSampleVisibleGGX(E, a2, V);\n")
		TEXT("#endif\n")
		TEXT("}\n");

	const FString Path = OverlayDir / TEXT("Private") / MLS_LumenFileName;
	if (!FFileHelper::SaveStringToFile(FString(Code), *Path))
	{
		OutError = FString::Printf(TEXT("Failed to write '%s'"), *Path);
		return false;
	}
	return true;
}

bool FMultiLobeShaderPatcher::PatchLumenDualBlur(const FString& OverlayDir, FString& OutWarning)
{
	static const TCHAR* Candidates[] =
	{
		TEXT("Private/Lumen/LumenReflections.usf"),
		TEXT("Private/Lumen/LumenReflectionCommon.ush"),
		TEXT("Private/Lumen/LumenReflectionTracing.usf"),
	};

	int32 TotalCalls = 0;
	for (const TCHAR* RelPath : Candidates)
	{
		const FString FilePath = OverlayDir / RelPath;
		FString Src;
		if (!FFileHelper::LoadFileToString(Src, *FilePath))
		{
			continue;
		}
		if (Src.Contains(MLS_LumenInclude))
		{
			continue; // already patched
		}

		const int32 Num = ReplaceIdentifierCalls(Src, TEXT("ImportanceSampleVisibleGGX"), TEXT("MLS_ImportanceSampleVisibleGGX"));
		if (Num == 0)
		{
			continue;
		}

		// Insert our include after the nearest '#include' line preceding the first rerouted call.
		const int32 CallPos = Src.Find(TEXT("MLS_ImportanceSampleVisibleGGX"), ESearchCase::CaseSensitive);
		int32 IncPos = INDEX_NONE;
		{
			int32 From = 0;
			while (true)
			{
				const int32 P = Src.Find(TEXT("#include"), ESearchCase::CaseSensitive, ESearchDir::FromStart, From);
				if (P == INDEX_NONE || P > CallPos)
				{
					break;
				}
				IncPos = P;
				From = P + 8;
			}
		}
		if (IncPos != INDEX_NONE)
		{
			int32 LineEnd = IncPos;
			while (LineEnd < Src.Len() && Src[LineEnd] != '\n') ++LineEnd;
			Src.InsertAt(LineEnd, FString::Printf(TEXT("\n%s %s"), MLS_LumenInclude, MLS_PatchMarker));
		}
		else
		{
			Src.InsertAt(0, FString::Printf(TEXT("%s %s\n"), MLS_LumenInclude, MLS_PatchMarker));
		}

		if (!FFileHelper::SaveStringToFile(Src, *FilePath))
		{
			OutWarning = FString::Printf(TEXT("Failed to save patched '%s'"), *FilePath);
			return false;
		}
		UE_LOG(LogMultiLobeSpec, Log, TEXT("Lumen dual-blur: rerouted %d GGX sample call(s) in %s"), Num, RelPath);
		TotalCalls += Num;
	}

	if (TotalCalls != 1)
	{
		OutWarning = FString::Printf(
			TEXT("Lumen dual-blur exact-count failure: expected 1 ImportanceSampleVisibleGGX call, found %d."),
			TotalCalls);
		return false;
	}
	return true;
}

bool FMultiLobeShaderPatcher::WriteConfigFile(const FString& OverlayDir, const FMLSShaderConfig& Cfg, FString& OutError)
{
	const float EnvW = Cfg.bPatchEnvBRDF ? Cfg.EnvLobe2Weight : 0.0f;

	const FString Config = FString::Printf(
		TEXT("// Generated by MultiLobeSpec plugin. Do not edit — rewritten on every preset change.\n")
		TEXT("#ifndef MLS_CONFIG_INCLUDED\n")
		TEXT("#define MLS_CONFIG_INCLUDED 1\n")
		TEXT("\n")
		TEXT("#define MLS_ENABLED %d\n")
		TEXT("#define MLS_LOBE2_WEIGHT %.4f\n")
		TEXT("#define MLS_ENV_WEIGHT %.4f\n")
		TEXT("#define MLS_CORE_FADE %.4f\n")
		TEXT("\n")
		TEXT("// Effective weights fade out on near-mirror roughness so the sharp\n")
		TEXT("// specular core is preserved (fixes 'highlight got soft' from v0.1).\n")
		TEXT("#define MLS_LOBE2_W(r) (MLS_LOBE2_WEIGHT * saturate((r) * MLS_CORE_FADE))\n")
		TEXT("#define MLS_ENV_W(r)   (MLS_ENV_WEIGHT   * saturate((r) * MLS_CORE_FADE))\n")
		TEXT("\n")
		TEXT("// Per-lobe energy pairing: compensation lives INSIDE each specular lobe\n")
		TEXT("// (SpecularGGX/Rect wrappers); global specular conservation is skipped for MLS.\n")
		TEXT("#define MLS_ENERGY_MIX 1\n")
		TEXT("\n")
		TEXT("// Lobe 1 = authored roughness; Lobe 2 = widened \"haze\" lobe.\n")
		TEXT("#define MLS_R1(r) (r)\n")
		TEXT("#define MLS_R2(r) saturate((r) * %.4f + %.4f)\n")
		TEXT("\n")
		TEXT("// ---- Diffuse model: 0 = Lambert (engine default), 1 = Chan (GGX-consistent) ----\n")
		TEXT("#define MLS_DIFFUSE_ROUGH %d\n")
		TEXT("#if MLS_ENABLED && MLS_DIFFUSE_ROUGH\n")
		TEXT("// Verbatim engine expression (5.7 MATERIAL_ROUGHDIFFUSE branch): LOCAL pre-morph\n")
		TEXT("// NoV/VoH/NoH (SphereMaxNoH artefact avoidance) + engine area-light weight.\n")
		TEXT("#define MLS_DefaultDiffuse(GB, AL) Diffuse_GGX_Rough((GB).DiffuseColor, (GB).Roughness, NoV, (AL).NoL, VoH, NoH, GetAreaLightDiffuseMicroReflWeight(AL))\n")
		TEXT("#else\n")
		TEXT("#define MLS_DefaultDiffuse(GB, AL) Diffuse_Lambert((GB).DiffuseColor)\n")
		TEXT("#endif\n")
		TEXT("\n")
		TEXT("// ---- Direct material micro-shadowing. AO is cosine-weighted surface visibility. ----\n")
		TEXT("#define MLS_MICROSHADOW %d\n")
		TEXT("#define MLS_MICROSHADOW_DIFFUSE_STRENGTH %.4f\n")
		TEXT("#define MLS_MICROSHADOW_SPECULAR_STRENGTH %.4f\n")
		TEXT("#define MLS_MICROSHADOW_MODE %d\n")
		TEXT("// Preserve raw MaterialAO whenever direct micro-shadowing, indirect material visibility, or cone-aware IBL consumes it.\n")
		TEXT("#define MLS_RAW_MATERIAL_VISIBILITY_TRANSPORT %d\n")
		TEXT("#if MLS_MICROSHADOW_MODE == 4\n")
		TEXT("#include \"/Engine/Private/MLS_MicroShadowLUT.ush\"\n")
		TEXT("#ifndef MLS_VNDF_LUT_ROUGHNESS_BANK_COUNT\n")
		TEXT("#define MLS_VNDF_LUT_ROUGHNESS_BANK_COUNT 1\n")
		TEXT("#define MLS_VNDF_LUT_TEXELS_PER_BANK (MLS_VNDF_LUT_VISIBILITY_SIZE * MLS_VNDF_LUT_NOL_SIZE * MLS_VNDF_LUT_NOV_SIZE)\n")
		TEXT("#endif\n")
		TEXT("float4 MLS_VNDF_UnpackRGBA8(uint Packed)\n")
		TEXT("{\n")
		TEXT("\treturn float4(Packed & 255u, (Packed >> 8u) & 255u, (Packed >> 16u) & 255u, (Packed >> 24u) & 255u) * (1.0 / 255.0);\n")
		TEXT("}\n")
		TEXT("float4 MLS_VNDF_LoadRGBA(int VisibilityIndex, int NoLIndex, int NoVIndex, int BankIndex)\n")
		TEXT("{\n")
		TEXT("\tint LinearIndex = BankIndex * MLS_VNDF_LUT_TEXELS_PER_BANK + (NoVIndex * MLS_VNDF_LUT_NOL_SIZE + NoLIndex) * MLS_VNDF_LUT_VISIBILITY_SIZE + VisibilityIndex;\n")
		TEXT("\treturn MLS_VNDF_UnpackRGBA8(MLS_VNDF_LUT_PACKED[LinearIndex]);\n")
		TEXT("}\n")
		TEXT("float4 MLS_VNDF_TrilinearRGBA(float3 GridPosition, int BankIndex)\n")
		TEXT("{\n")
		TEXT("\tint3 I0 = (int3)floor(GridPosition);\n")
		TEXT("\tint3 I1 = min(I0 + 1, int3(MLS_VNDF_LUT_VISIBILITY_SIZE - 1, MLS_VNDF_LUT_NOL_SIZE - 1, MLS_VNDF_LUT_NOV_SIZE - 1));\n")
		TEXT("\tfloat3 F = saturate(GridPosition - (float3)I0);\n")
		TEXT("\tfloat4 C000 = MLS_VNDF_LoadRGBA(I0.x, I0.y, I0.z, BankIndex);\n")
		TEXT("\tfloat4 C100 = MLS_VNDF_LoadRGBA(I1.x, I0.y, I0.z, BankIndex);\n")
		TEXT("\tfloat4 C010 = MLS_VNDF_LoadRGBA(I0.x, I1.y, I0.z, BankIndex);\n")
		TEXT("\tfloat4 C110 = MLS_VNDF_LoadRGBA(I1.x, I1.y, I0.z, BankIndex);\n")
		TEXT("\tfloat4 C001 = MLS_VNDF_LoadRGBA(I0.x, I0.y, I1.z, BankIndex);\n")
		TEXT("\tfloat4 C101 = MLS_VNDF_LoadRGBA(I1.x, I0.y, I1.z, BankIndex);\n")
		TEXT("\tfloat4 C011 = MLS_VNDF_LoadRGBA(I0.x, I1.y, I1.z, BankIndex);\n")
		TEXT("\tfloat4 C111 = MLS_VNDF_LoadRGBA(I1.x, I1.y, I1.z, BankIndex);\n")
		TEXT("\tfloat4 C00 = lerp(C000, C100, F.x);\n")
		TEXT("\tfloat4 C10 = lerp(C010, C110, F.x);\n")
		TEXT("\tfloat4 C01 = lerp(C001, C101, F.x);\n")
		TEXT("\tfloat4 C11 = lerp(C011, C111, F.x);\n")
		TEXT("\treturn lerp(lerp(C00, C10, F.y), lerp(C01, C11, F.y), F.z);\n")
		TEXT("}\n")
		TEXT("float4 MLS_VNDF_SampleRGBA(float Visibility, float NoL, float NoV, int BankIndex)\n")
		TEXT("{\n")
		TEXT("\tif (Visibility <= 0.0) return float4(0.0, 0.0, 0.0, 0.0);\n")
		TEXT("\tif (Visibility >= 1.0) return float4(1.0, 1.0, 1.0, 1.0);\n")
		TEXT("\tNoL = saturate(NoL);\n")
		TEXT("\tif (2.0 * Visibility + NoL < 1.0) return float4(0.0, 0.0, 0.0, 0.0);\n")
		TEXT("#if MLS_VNDF_LUT_AXIS_BOUNDARY_AWARE\n")
		TEXT("\tfloat Boundary = sqrt(saturate(1.0 - Visibility));\n")
		TEXT("\tfloat SignedBoundaryCoordinate = NoL < Boundary\n")
		TEXT("\t\t? -sqrt(saturate((Boundary - NoL) / max(Boundary, 1e-6)))\n")
		TEXT("\t\t:  sqrt(saturate((NoL - Boundary) / max(1.0 - Boundary, 1e-6)));\n")
		TEXT("\tfloat NoLT = 0.5 + 0.5 * SignedBoundaryCoordinate;\n")
		TEXT("#elif MLS_VNDF_LUT_AXIS_WARPED\n")
		TEXT("\tfloat NoLT = 1.0 - sqrt(saturate(1.0 - NoL));\n")
		TEXT("#else\n")
		TEXT("\tfloat NoLT = NoL;\n")
		TEXT("#endif\n")
		TEXT("#if MLS_VNDF_LUT_AXIS_WARPED || MLS_VNDF_LUT_AXIS_BOUNDARY_AWARE\n")
		TEXT("#if MLS_VNDF_LUT_AXIS_BOUNDARY_AWARE\n")
		TEXT("\tfloat VisibilityRoot = sqrt(saturate(Visibility));\n")
		TEXT("\tfloat VisibilityComplementRoot = sqrt(saturate(1.0 - Visibility));\n")
		TEXT("\tfloat VisibilityT = VisibilityRoot / max(VisibilityRoot + VisibilityComplementRoot, 1e-6);\n")
		TEXT("#else\n")
		TEXT("\tfloat VisibilityT = sqrt(saturate(Visibility));\n")
		TEXT("#endif\n")
		TEXT("\tfloat NoVT = sqrt(saturate(NoV));\n")
		TEXT("#else\n")
		TEXT("\tfloat VisibilityT = saturate(Visibility);\n")
		TEXT("\tfloat NoVT = saturate(NoV);\n")
		TEXT("#endif\n")
		TEXT("\tfloat3 GridPosition = float3(\n")
		TEXT("\t\tVisibilityT * (MLS_VNDF_LUT_VISIBILITY_SIZE - 1),\n")
		TEXT("\t\tNoLT * (MLS_VNDF_LUT_NOL_SIZE - 1),\n")
		TEXT("\t\tNoVT * (MLS_VNDF_LUT_NOV_SIZE - 1));\n")
		TEXT("\treturn MLS_VNDF_TrilinearRGBA(GridPosition, BankIndex);\n")
		TEXT("}\n")
		TEXT("float MLS_VNDF_InterpolateRoughness(float4 LUTValues, float Roughness, int BankIndex)\n")
		TEXT("{\n")
		TEXT("\tRoughness = clamp(Roughness, 0.1, 1.0);\n")
		TEXT("\tfloat4 RoughnessNodes = MLS_VNDF_LUT_ROUGHNESS_NODES;\n")
		TEXT("#if MLS_VNDF_LUT_ROUGHNESS_BANK_COUNT == 2\n")
		TEXT("\tif (BankIndex != 0) RoughnessNodes = MLS_VNDF_LUT_ROUGHNESS_NODES_HIGH;\n")
		TEXT("#endif\n")
		TEXT("\tint Channel0 = Roughness < RoughnessNodes.y ? 0 : (Roughness < RoughnessNodes.z ? 1 : 2);\n")
		TEXT("\tint Channel1 = min(Channel0 + 1, 3);\n")
		TEXT("\tfloat Node0 = RoughnessNodes[Channel0];\n")
		TEXT("\tfloat Node1 = RoughnessNodes[Channel1];\n")
		TEXT("#if MLS_VNDF_LUT_ROUGHNESS_ALPHA_INTERPOLATION\n")
		TEXT("\tfloat RoughnessT = (Roughness * Roughness - Node0 * Node0) / max(Node1 * Node1 - Node0 * Node0, 1e-6);\n")
		TEXT("#else\n")
		TEXT("\tfloat RoughnessT = (Roughness - Node0) / max(Node1 - Node0, 1e-6);\n")
		TEXT("#endif\n")
		TEXT("\treturn saturate(lerp(LUTValues[Channel0], LUTValues[Channel1], saturate(RoughnessT)));\n")
		TEXT("}\n")
		TEXT("float MLS_MicroShadow_GenericVNDF(float Visibility, float Roughness, float NoL, float NoV)\n")
		TEXT("{\n")
		TEXT("\tint BankIndex = 0;\n")
		TEXT("#if MLS_VNDF_LUT_ROUGHNESS_BANK_COUNT == 2\n")
		TEXT("\tBankIndex = Roughness > MLS_VNDF_LUT_ROUGHNESS_BANK_SPLIT ? 1 : 0;\n")
		TEXT("#endif\n")
		TEXT("\treturn MLS_VNDF_InterpolateRoughness(MLS_VNDF_SampleRGBA(Visibility, NoL, NoV, BankIndex), Roughness, BankIndex);\n")
		TEXT("}\n")
		TEXT("#endif // MLS_MICROSHADOW_MODE == 4\n")
		TEXT("float MLS_Pow5(float X)\n")
		TEXT("{\n")
		TEXT("\tfloat X2 = X * X;\n")
		TEXT("\treturn X2 * X2 * X;\n")
		TEXT("}\n")
		TEXT("float MLS_MicroShadow_ReferenceStep(float Visibility, float NoL)\n")
		TEXT("{\n")
		TEXT("\treturn (NoL > sqrt(saturate(1.0 - Visibility))) ? 1.0 : 0.0;\n")
		TEXT("}\n")
		TEXT("float MLS_MicroShadow_ActivisionWWII(float Visibility, float NoL)\n")
		TEXT("{\n")
		TEXT("\tif (Visibility <= 0.0) return 0.0;\n")
		TEXT("\tif (Visibility >= 1.0) return 1.0;\n")
		TEXT("\tfloat CosTheta = sqrt(saturate(1.0 - Visibility));\n")
		TEXT("\tfloat T = saturate(NoL / max(CosTheta, 1e-6));\n")
		TEXT("\treturn T * T;\n")
		TEXT("}\n")
		TEXT("float MLS_MicroShadow_PZAnalytical(float Visibility, float NoL)\n")
		TEXT("{\n")
		TEXT("\tif (Visibility <= 0.0) return 0.0;\n")
		TEXT("\tif (Visibility >= 1.0) return 1.0;\n")
		TEXT("\tfloat CosThetaPrime = sqrt(saturate(1.0 - MLS_Pow5(Visibility)));\n")
		TEXT("\treturn pow(saturate(NoL / max(CosThetaPrime, 1e-6)), 0.75 * rcp(Visibility));\n")
		TEXT("}\n")
		TEXT("// Legacy roughness-aware spherical-cap overlap heuristic. It is not VNDF sampling.\n")
		TEXT("#define MLS_MS_CONE_K 1.5\n")
		TEXT("float MLS_ConeConeOverlap(float CosA, float CosB, float CosBeta)\n")
		TEXT("{\n")
		TEXT("\tfloat ThetaA = acos(clamp(CosA, -1.0, 1.0));\n")
		TEXT("\tfloat ThetaB = acos(clamp(CosB, -1.0, 1.0));\n")
		TEXT("\tfloat Beta = acos(clamp(CosBeta, -1.0, 1.0));\n")
		TEXT("\tfloat FullyIn = ThetaA - ThetaB;\n")
		TEXT("\tfloat FullyOut = ThetaA + ThetaB;\n")
		TEXT("\treturn smoothstep(0.0, 1.0, saturate((FullyOut - Beta) / max(FullyOut - FullyIn, 1e-4)));\n")
		TEXT("}\n")
		TEXT("float MLS_MicroShadow_ConeOverlapExperimental(float Visibility, float Roughness, float NoL)\n")
		TEXT("{\n")
		TEXT("\tif (Visibility <= 0.0) return 0.0;\n")
		TEXT("\tif (Visibility >= 1.0) return 1.0;\n")
		TEXT("\tfloat CosThetaVis = saturate(1.0 - Visibility); // preserve the v0.12.3 experiment exactly\n")
		TEXT("\tfloat Alpha = max(Roughness * Roughness, 0.02);\n")
		TEXT("\tfloat K = MLS_MS_CONE_K * Alpha;\n")
		TEXT("\tfloat CosThetaLobe = rsqrt(1.0 + K * K); // lobe cap around L\n")
		TEXT("\treturn MLS_ConeConeOverlap(CosThetaVis, CosThetaLobe, saturate(NoL));\n")
		TEXT("}\n")
		TEXT("float MLS_EvaluateMicroShadow(float Visibility, float Roughness, float NoL, float NoV)\n")
		TEXT("{\n")
		TEXT("#if MLS_MICROSHADOW_MODE == 1\n")
		TEXT("\treturn MLS_MicroShadow_ReferenceStep(Visibility, NoL);\n")
		TEXT("#elif MLS_MICROSHADOW_MODE == 2\n")
		TEXT("\treturn MLS_MicroShadow_ActivisionWWII(Visibility, NoL);\n")
		TEXT("#elif MLS_MICROSHADOW_MODE == 3\n")
		TEXT("\treturn MLS_MicroShadow_PZAnalytical(Visibility, NoL);\n")
		TEXT("#elif MLS_MICROSHADOW_MODE == 4\n")
		TEXT("\treturn MLS_MicroShadow_GenericVNDF(Visibility, Roughness, NoL, NoV);\n")
		TEXT("#elif MLS_MICROSHADOW_MODE == 5\n")
		TEXT("\treturn MLS_MicroShadow_ConeOverlapExperimental(Visibility, Roughness, NoL);\n")
		TEXT("#else\n")
		TEXT("\treturn 1.0;\n")
		TEXT("#endif\n")
		TEXT("}\n")
		TEXT("\n")
		TEXT("// ---- Indirect material visibility: 0 Direct Only, 1 Raw Scalar AO, 2 RGB interreflection ----\n")
		TEXT("#define MLS_INDIRECT_VISIBILITY_MODE %d\n")
		TEXT("float3 MLS_InterreflectionVisibility(float Visibility, float3 DiffuseAlbedo)\n")
		TEXT("{\n")
		TEXT("\treturn saturate(Visibility / max(1.0 - DiffuseAlbedo * (1.0 - Visibility), 1e-4));\n")
		TEXT("}\n")
		TEXT("\n")
		TEXT("// ---- Lumen dual-blur: stochastic wide-lobe rays at reflection ray generation ----\n")
		TEXT("#define MLS_LUMEN_DUALBLUR %d\n")
		TEXT("#define MLS_LUMEN_BLUR_WEIGHT %.4f\n")
		TEXT("\n")
		TEXT("// Mixture-mean roughness for Lumen reconstruction (active only with dual-blur).\n")
		TEXT("// Reconstruction heuristic (NOT a mixture roughness): alpha-space mean, driven by\n")
		TEXT("// the SAME weight the ray generation uses (incl. near-mirror fade).\n")
		TEXT("#define MLS_ReconR(r) (sqrt(lerp((r) * (r), MLS_R2(r) * MLS_R2(r), MLS_LUMEN_BLUR_WEIGHT * saturate((r) * MLS_CORE_FADE) * MLS_LUMEN_DUALBLUR)))\n")
		TEXT("\n")
		TEXT("// ---- Paired IBL (v0.7): per-lobe radiance x response at the composite call-site ----\n")
		TEXT("#define MLS_PAIRED_IBL %d\n")
		TEXT("// CARD-09: cone-aware split-sum response for paired captures/skylight.\n")
		TEXT("#define MLS_CONE_ENVBRDF %d\n")
		TEXT("// Legacy mixed-radiance TWS mode is superseded by paired IBL — permanently off.\n")
		TEXT("#define MLS_TWO_SAMPLE_IBL 0\n")
		TEXT("#define MLS_IBL_W(r) (MLS_ENV_W(r) * MLS_TWO_SAMPLE_IBL)\n")
		TEXT("#define MLS_DEBUG_VIEW %d\n")
		TEXT("\n")
		TEXT("// ---- Tonemapper: 0 = engine ACES, 1 = AgX Base, 2 = AgX Punchy ----\n")
		TEXT("#define MLS_TONEMAP %d\n")
		TEXT("// Space adapters (see README 'Калибровка AgX'):\n")
		TEXT("#define MLS_AGX_INPUT_AP1 1\n")
		TEXT("#define MLS_AGX_OUTPUT_AP1 1\n")
		TEXT("#define MLS_AGX_OUTPUT_LINEARIZE 1\n")
		TEXT("\n")
		TEXT("#endif // MLS_CONFIG_INCLUDED\n"),
		Cfg.bEnabled ? 1 : 0,
		Cfg.Lobe2Weight,
		EnvW,
		Cfg.CoreFade,
		Cfg.Lobe2Scale,
		Cfg.Lobe2Offset,
		Cfg.DiffuseModel,
		Cfg.bMicroShadow ? 1 : 0,
		Cfg.MicroShadowDiffuseStrength,
		Cfg.MicroShadowSpecularStrength,
		Cfg.MicroShadowMode,
		Cfg.NeedsRawMaterialVisibilityTransport() ? 1 : 0,
		Cfg.IndirectMaterialVisibilityMode,
		Cfg.bLumenDualBlur ? 1 : 0,
		Cfg.LumenBlurWeight,
		Cfg.bTwoSampleIBL ? 1 : 0,
		Cfg.bConeAwareIndirectSpecular ? 1 : 0,
		Cfg.DebugView,
		Cfg.TonemapMode);

	const FString ConfigPath = OverlayDir / TEXT("Private") / MLS_ConfigFileName;
	if (!FFileHelper::SaveStringToFile(Config, *ConfigPath))
	{
		OutError = FString::Printf(TEXT("Failed to write '%s'"), *ConfigPath);
		return false;
	}
	return true;
}

bool FMultiLobeShaderPatcher::WriteCapabilityManifest(
	const FString& OverlayDir,
	const FMLSShaderConfig& Cfg,
	FString& OutError)
{
	auto ReadCVarInt = [](const TCHAR* Name, int32& OutValue)
	{
		const IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(Name);
		if (Variable == nullptr)
		{
			return false;
		}
		OutValue = Variable->GetInt();
		return true;
	};

	int32 Substrate = -1;
	int32 AllowStaticLighting = -1;
	int32 DiffuseSampleOcclusion = -1;
	const bool bHasSubstrate = ReadCVarInt(TEXT("r.Substrate"), Substrate);
	const bool bHasStaticLighting = ReadCVarInt(TEXT("r.AllowStaticLighting"), AllowStaticLighting);
	const bool bHasDiffuseSampleOcclusion = ReadCVarInt(TEXT("r.GBufferDiffuseSampleOcclusion"), DiffuseSampleOcclusion);
	const bool bOverlayOnlyTransportPrerequisites =
		bHasSubstrate && Substrate == 0
		&& bHasStaticLighting && AllowStaticLighting == 0
		&& bHasDiffuseSampleOcclusion && DiffuseSampleOcclusion == 0;

	FString LUTInclude;
	FString LUTManifest;
	FString LUTValidation;
	const bool bLUTArtifactsPresent = MLS_GetVNDFArtifactPaths(LUTInclude, LUTManifest)
		&& FPaths::FileExists(LUTInclude)
		&& FPaths::FileExists(LUTManifest);
	const bool bLUTValidationReceiptPresent = MLS_GetVNDFValidationPath(LUTValidation)
		&& FPaths::FileExists(LUTValidation);
	FString LUTAdmissionReason;
	const bool bLUTNumericallyAdmitted = bLUTArtifactsPresent
		&& bLUTValidationReceiptPresent
		&& MLS_ValidateVNDFAdmission(LUTInclude, LUTManifest, LUTValidation, LUTAdmissionReason);
	FString ConeEnvInclude;
	FString ConeEnvManifest;
	const bool bConeEnvArtifactsPresent = MLS_GetConeEnvArtifactPaths(ConeEnvInclude, ConeEnvManifest)
		&& FPaths::FileExists(ConeEnvInclude)
		&& FPaths::FileExists(ConeEnvManifest);
	// P29/P30/P31 showed that every tested overlay-only static-array layout makes
	// UE 5.7 SM6 compilation consume multi-GB per worker and fail the bounded-time
	// compile gate. Keep the exact implementation/artifact staged, but never
	// advertise this storage policy as runtime-available.
	constexpr bool bConeEnvStaticStorageCompileAdmitted = false;
	const bool bGenericRequested = Cfg.bMicroShadow && Cfg.MicroShadowMode == 4;
	const bool bRawTransportRequested = Cfg.NeedsRawMaterialVisibilityTransport();
	const bool bRawTransportPatched = bRawTransportRequested && bOverlayOnlyTransportPrerequisites;
	const bool bGenericDirectAvailable = bGenericRequested && bLUTNumericallyAdmitted && bRawTransportPatched;
	const bool bRGBIndirectRequested = Cfg.bEnabled && Cfg.IndirectMaterialVisibilityMode == 2;
	const bool bConeEnvAvailable = Cfg.bEnabled
		&& Cfg.bTwoSampleIBL
		&& Cfg.bConeAwareIndirectSpecular
		&& bConeEnvArtifactsPresent
		&& bRawTransportPatched
		&& bConeEnvStaticStorageCompileAdmitted;

	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("schemaVersion"), 1);
	Root->SetStringField(TEXT("engineVersion"), FEngineVersion::Current().ToString());
	Root->SetNumberField(TEXT("patchVersion"), PatchVersion);
	Root->SetStringField(TEXT("overlayBuildId"), GetOverlayBuildId(Cfg));
	Root->SetBoolField(TEXT("GenericVNDFLUT_ArtifactsPresent"), bLUTArtifactsPresent);
	Root->SetBoolField(TEXT("GenericVNDFLUT_ValidationReceiptPresent"), bLUTValidationReceiptPresent);
	Root->SetBoolField(TEXT("GenericVNDFLUT_RuntimeSamplerImplemented"), true);
	Root->SetBoolField(TEXT("GenericVNDFLUT_NumericallyAdmitted"), bLUTNumericallyAdmitted);
	Root->SetStringField(TEXT("GenericVNDFLUT_AdmissionReason"), LUTAdmissionReason);
	Root->SetStringField(TEXT("GenericVNDFLUT_Representation"), TEXT("V2 SingleBankPiecewise, two overlapping RGBA8 banks, one selected trilinear fetch"));
	Root->SetStringField(TEXT("GenericVNDFDirect_AzimuthPolicy"), TEXT("Isotropic4D Phi0 approximation; arbitrary relative azimuth is measured but not numerically admitted"));
	Root->SetBoolField(TEXT("LegacyRawMaterialVisibility_OverlayPrerequisitesMet"), bOverlayOnlyTransportPrerequisites);
	Root->SetBoolField(TEXT("LegacyRawMaterialVisibility_TransportRequested"), bRawTransportRequested);
	Root->SetBoolField(TEXT("LegacyRawMaterialVisibility_TransportPatched"),
		bRawTransportPatched);

	Root->SetBoolField(TEXT("GenericVNDFDirect_DefaultLit"), bGenericDirectAvailable);
	Root->SetBoolField(TEXT("GenericVNDFDirect_MegaLights"), false);
	Root->SetBoolField(TEXT("GenericVNDF_RectLight"), false);
	Root->SetStringField(TEXT("GenericVNDF_AnisotropyFallback"), TEXT("Off (original anisotropic SpecularGGX and diffuse; no Generic VNDF term)"));
	Root->SetBoolField(TEXT("IndirectDiffuse_LumenRGBInterreflection"), bRGBIndirectRequested);
	Root->SetBoolField(TEXT("IndirectDiffuse_SkylightRGBInterreflection"), bRGBIndirectRequested);
	Root->SetBoolField(TEXT("IndirectDiffuse_NonLumenRGBInterreflection"), bRGBIndirectRequested);
	Root->SetBoolField(TEXT("IndirectSpecular_ConeEnvBRDF"), bConeEnvAvailable);
	Root->SetBoolField(TEXT("IndirectSpecular_ConeEnvBRDF_ArtifactsPresent"), bConeEnvArtifactsPresent);
	Root->SetBoolField(TEXT("IndirectSpecular_ConeEnvBRDF_NumericallyAdmitted"), false);
	Root->SetBoolField(TEXT("IndirectSpecular_ConeEnvBRDF_StaticStorageCompileAdmitted"), bConeEnvStaticStorageCompileAdmitted);
	Root->SetStringField(TEXT("IndirectSpecular_ConeEnvBRDF_StoragePolicy"),
		TEXT("Static overlay data is staging-only; production activation requires an explicitly bound texture/renderer path"));
	Root->SetStringField(TEXT("IndirectSpecular_ConeEnvBRDF_Representation"), TEXT("32x32x8 NoV x Roughness x AdjustedCosCone, packed RG16_UNORM, manual trilinear"));
	Root->SetBoolField(TEXT("IndirectSpecular_CapturesSkylight_PerLobePairing"), bConeEnvAvailable);
	Root->SetBoolField(TEXT("IndirectSpecular_SingleScatterEstimatorContractImplemented"), true);
	Root->SetBoolField(TEXT("IndirectSpecular_SingleScatterConeIntegralExactToEstimator"), false);
	Root->SetStringField(TEXT("IndirectSpecular_EnergyConservationPolicy"),
		TEXT("Approximate: cone/open single-scatter ratio scales UE multi-scatter EnergyTerms.E; exact cone-integrated multi-scatter is unavailable"));
	Root->SetBoolField(TEXT("DualLobe_PerLobeMicroShadow"), bGenericDirectAvailable);
	Root->SetBoolField(TEXT("Lumen_PerLobeConePairing"), false);
	Root->SetBoolField(TEXT("Transactional_ExactRequestedAnchorCounts"), true);
	Root->SetBoolField(TEXT("Transactional_ContentAddressedOverlay"), true);
	Root->SetBoolField(TEXT("Transactional_PreRemapSmokeCompile"), false);

	const TSharedRef<FJsonObject> Prerequisites = MakeShared<FJsonObject>();
	Prerequisites->SetNumberField(TEXT("r.Substrate"), Substrate);
	Prerequisites->SetNumberField(TEXT("r.AllowStaticLighting"), AllowStaticLighting);
	Prerequisites->SetNumberField(TEXT("r.GBufferDiffuseSampleOcclusion"), DiffuseSampleOcclusion);
	Prerequisites->SetStringField(TEXT("requiredContract"),
		TEXT("r.Substrate=0; r.AllowStaticLighting=0; r.GBufferDiffuseSampleOcclusion=0"));
	Root->SetObjectField(TEXT("rawMaterialVisibilityPrerequisites"), Prerequisites);

	TArray<TSharedPtr<FJsonValue>> Blockers;
	Blockers.Add(MakeShared<FJsonValueString>(TEXT("Lumen/SSR per-lobe indirect-specular radiance pairing is unsupported without pipeline extension.")));
	Blockers.Add(MakeShared<FJsonValueString>(TEXT("CARD-09 interpolation/reference error distributions are recorded, but numerical admission thresholds are not specified by v0.14 and remain to be ratified.")));
	Blockers.Add(MakeShared<FJsonValueString>(TEXT("CARD-09 overlay static storage failed the UE 5.7 SM6 compile-cost gate in scalar, uint4, and eight-plane layouts; production activation requires renderer-bound Texture3D/SRV work.")));
	Blockers.Add(MakeShared<FJsonValueString>(TEXT("Pre-remap shader smoke compile is pending.")));
	Root->SetArrayField(TEXT("blockers"), Blockers);

	FString Json;
	const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Json);
	if (!FJsonSerializer::Serialize(Root, Writer))
	{
		OutError = TEXT("Failed to serialize MLS capability manifest.");
		return false;
	}
	Json += TEXT("\n");
	const FString Path = OverlayDir / TEXT("MLS_CAPABILITIES.json");
	if (!FFileHelper::SaveStringToFile(Json, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		OutError = FString::Printf(TEXT("Failed to write capability manifest: %s"), *Path);
		return false;
	}
	return true;
}

bool FMultiLobeShaderPatcher::BuildOverlay(const FString& EngineShaderDir, const FString& OverlayDir,
                                           const FMLSShaderConfig& Cfg, FString& OutError)
{
	if (Cfg.bConeAwareIndirectSpecular)
	{
		OutError = TEXT("Cone-aware indirect specular is staged but unavailable: the overlay static LUT failed the UE 5.7 SM6 compile-cost gate. A renderer-bound Texture3D/SRV path is required before activation.");
		return false;
	}

	bool bFreshCopy = false;
	if (!EnsureOverlayCopy(EngineShaderDir, OverlayDir, Cfg, bFreshCopy, OutError))
	{
		return false;
	}

	if (bFreshCopy)
	{
		if (!MLS_CopyVNDFArtifacts(
			OverlayDir,
			Cfg.MicroShadowMode == 4,
			OutError))
		{
			return false;
		}
		if (!MLS_CopyConeEnvArtifacts(
			OverlayDir,
			Cfg.bConeAwareIndirectSpecular,
			OutError))
		{
			return false;
		}
		if (Cfg.NeedsRawMaterialVisibilityTransport()
			&& !PatchRawMaterialVisibilityTransport(OverlayDir, OutError))
		{
			return false;
		}

		int32 NumDefaultLitSpecular = 0;
		int32 NumEnvBRDF = 0;
		int32 NumEnvBRDFApprox = 0;
		int32 NumDiffuse = 0;
		int32 NumMicroShadow = 0;
		int32 NumEnergyMix = 0;

		for (const TCHAR* RelPath : MLS_CandidateFiles)
		{
			const FString FilePath = OverlayDir / RelPath;
			FString Src;
			if (!FFileHelper::LoadFileToString(Src, *FilePath))
			{
				continue;
			}
			if (Src.Contains(MLS_PatchMarker))
			{
				continue;
			}

			const int32 A = PatchDefaultLitDualSpecular(Src);
			const int32 B = PatchFunctionDualLobe(Src, TEXT("EnvBRDF"),       TEXT("MLS_ENV_W(Roughness)"));
			const int32 C = PatchFunctionDualLobe(Src, TEXT("EnvBRDFApprox"), TEXT("MLS_ENV_W(Roughness)"));
			const int32 Dd = PatchDiffuseCallSite(Src);
			const int32 Ms = PatchMicroShadow(Src);
			const int32 En = PatchEnergyMix(Src);

			if (A + B + C + Dd + Ms + En > 0)
			{
				EnsureConfigInclude(Src);
				if (!FFileHelper::SaveStringToFile(Src, *FilePath))
				{
					OutError = FString::Printf(TEXT("Failed to save patched '%s'"), *FilePath);
					return false;
				}
				UE_LOG(LogMultiLobeSpec, Log,
					TEXT("Patched %s: SpecularGGX x%d, EnvBRDF x%d, EnvBRDFApprox x%d, DiffuseCallSite x%d, MicroShadow x%d, EnergyMix x%d/3"),
					RelPath, A, B, C, Dd, Ms, En);
			}

			NumDefaultLitSpecular += A;
			NumEnvBRDF += B;
			NumEnvBRDFApprox += C;
			NumDiffuse += Dd;
			NumMicroShadow += Ms;
			NumEnergyMix += En;
		}

		if (Cfg.bEnabled && NumDefaultLitSpecular != 1)
		{
			OutError = FString::Printf(TEXT("DefaultLit dual-specular exact-count failure: expected 1, found %d."), NumDefaultLitSpecular);
			IFileManager::Get().Delete(*(OverlayDir / MLS_StampFileName));
			return false;
		}
		if (Cfg.bEnabled && Cfg.bPatchEnvBRDF && NumEnvBRDF != 2)
		{
			OutError = FString::Printf(TEXT("EnvBRDF exact-count failure: expected 2, found %d."), NumEnvBRDF);
			return false;
		}
		if (Cfg.bEnabled && Cfg.bPatchEnvBRDFApprox && NumEnvBRDFApprox != 2)
		{
			OutError = FString::Printf(TEXT("EnvBRDFApprox exact-count failure: expected 2, found %d."), NumEnvBRDFApprox);
			return false;
		}
		if (Cfg.bEnabled && NumDiffuse != 1)
		{
			OutError = FString::Printf(TEXT("DefaultLit diffuse exact-count failure: expected 1, found %d."), NumDiffuse);
			return false;
		}
		if (Cfg.bMicroShadow && NumMicroShadow != 1)
		{
			OutError = FString::Printf(TEXT("DefaultLit micro-shadow exact-count failure: expected 1, found %d."), NumMicroShadow);
			return false;
		}
		if (Cfg.bEnabled && NumEnergyMix != 3)
		{
			OutError = FString::Printf(TEXT("DefaultLit energy exact-count failure: expected 3, found %d."), NumEnergyMix);
			return false;
		}

		FString AgXError;
		if (!WriteAgXFile(OverlayDir, AgXError))
		{
			OutError = AgXError;
			return false;
		}
		FString TMWarning;
		if (Cfg.TonemapMode != 0 && !PatchTonemapper(OverlayDir, TMWarning))
		{
			OutError = TMWarning.IsEmpty() ? TEXT("Tonemapper patch failed.") : TMWarning;
			return false;
		}

		FString LumenError;
		if (!WriteLumenDualBlurFile(OverlayDir, LumenError))
		{
			OutError = LumenError;
			return false;
		}
		FString LumenWarning;
		if (Cfg.bLumenDualBlur && !PatchLumenDualBlur(OverlayDir, LumenWarning))
		{
			OutError = LumenWarning.IsEmpty() ? TEXT("Lumen dual-blur patch failed.") : LumenWarning;
			return false;
		}
		FString ResolveWarning;
		if (Cfg.bLumenDualBlur && !PatchLumenResolve(OverlayDir, ResolveWarning))
		{
			OutError = ResolveWarning.IsEmpty() ? TEXT("Lumen resolve patch failed.") : ResolveWarning;
			return false;
		}
		if (!WriteConeEnvBRDFRuntimeFile(OverlayDir, OutError))
		{
			return false;
		}
		FString PairedWarning;
		if (Cfg.bTwoSampleIBL && !PatchPairedIBL(OverlayDir, PairedWarning))
		{
			OutError = PairedWarning.IsEmpty() ? TEXT("Paired IBL patch failed.") : PairedWarning;
			return false;
		}
		FString IndirectWarning;
		if (Cfg.bEnabled && !PatchIndirectAO(OverlayDir, IndirectWarning))
		{
			OutError = IndirectWarning.IsEmpty() ? TEXT("Indirect AO patch failed.") : IndirectWarning;
			return false;
		}
		FString LumenAOWarning;
		if (Cfg.bEnabled && !PatchLumenIndirectAO(OverlayDir, LumenAOWarning))
		{
			OutError = LumenAOWarning.IsEmpty() ? TEXT("Lumen indirect AO patch failed.") : LumenAOWarning;
			return false;
		}
		FString SkyAOWarning;
		if (Cfg.bEnabled && !PatchSkyLightIndirectAO(OverlayDir, SkyAOWarning))
		{
			OutError = SkyAOWarning.IsEmpty() ? TEXT("Skylight indirect AO patch failed.") : SkyAOWarning;
			return false;
		}

		// The paired call-site already invokes GatherRadiance once per lobe with
		// that lobe's roughness/direction. Do not also dual-wrap the inner TWS
		// composite: doing both creates a mixture-of-mixtures and breaks pairing.
		int32 NumRect = 0;
		// Rect LTC: specialized adapter (honest mean direction + per-lobe energy).
		{
			const FString RectPath = OverlayDir / TEXT("Private/RectLight.ush");
			FString RectSrc;
			if (FFileHelper::LoadFileToString(RectSrc, *RectPath) && !RectSrc.Contains(TEXT("RectGGXApproxLTC_MLS_Orig")))
			{
				NumRect = PatchRectAdapter(RectSrc);
				if (NumRect > 0)
				{
					EnsureConfigInclude(RectSrc);
					// Per-lobe energy in the adapter needs the energy-terms header in every
					// including TU (#pragma once makes the double include safe).
					{
						const int32 CfgPos = RectSrc.Find(MLS_ConfigInclude, ESearchCase::CaseSensitive);
						if (CfgPos != INDEX_NONE)
						{
							int32 LineEnd = CfgPos;
							while (LineEnd < RectSrc.Len() && RectSrc[LineEnd] != '\n') ++LineEnd;
							RectSrc.InsertAt(LineEnd, FString::Printf(TEXT("\n#include \"/Engine/Private/ShadingEnergyConservation.ush\" %s"), MLS_PatchMarker));
						}
					}
					if (!FFileHelper::SaveStringToFile(RectSrc, *RectPath))
					{
						OutError = TEXT("Failed to save patched RectLight.ush");
						return false;
					}
					UE_LOG(LogMultiLobeSpec, Log, TEXT("Rect LTC adapter: %d inner overload(s) — honest mean direction, per-lobe energy."), NumRect);
				}
			}
		}
		if (Cfg.bEnabled && NumRect != 2)
		{
			OutError = FString::Printf(TEXT("Rect LTC exact-count failure: expected 2, found %d."), NumRect);
			return false;
		}

		// Config first, THEN the stamp — a stamped overlay is fully patched AND configured.
		{
			FString CfgError;
			if (!WriteConfigFile(OverlayDir, Cfg, CfgError))
			{
				OutError = CfgError;
				return false;
			}
		}
		if (!WriteCapabilityManifest(OverlayDir, Cfg, OutError))
		{
			return false;
		}
		// Commit the stamp LAST — only a fully patched overlay is ever considered valid.
		const FString Stamp = GetOverlayBuildId(Cfg);
		if (!FFileHelper::SaveStringToFile(Stamp, *(OverlayDir / MLS_StampFileName)))
		{
			OutError = TEXT("Failed to commit overlay stamp.");
			return false;
		}
	}

	// A content-addressed overlay never changes in place. Existing valid targets
	// already contain the exact config whose digest names their directory.
	return true;
}
