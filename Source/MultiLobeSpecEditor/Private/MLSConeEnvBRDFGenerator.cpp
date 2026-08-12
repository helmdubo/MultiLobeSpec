#include "MLSConeEnvBRDFGenerator.h"

#include "MLSMicroShadowLUTGenerator.h"

#include "Async/ParallelFor.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#endif

namespace
{
	constexpr double MLSTwoPi = 6.283185307179586476925286766559;
	constexpr double MLSInvUint32 = 1.0 / 4294967296.0;

	uint32 ReverseBits32(uint32 Bits)
	{
		Bits = (Bits << 16u) | (Bits >> 16u);
		Bits = ((Bits & 0x00ff00ffu) << 8u) | ((Bits & 0xff00ff00u) >> 8u);
		Bits = ((Bits & 0x0f0f0f0fu) << 4u) | ((Bits & 0xf0f0f0f0u) >> 4u);
		Bits = ((Bits & 0x33333333u) << 2u) | ((Bits & 0xccccccccu) >> 2u);
		Bits = ((Bits & 0x55555555u) << 1u) | ((Bits & 0xaaaaaaaau) >> 1u);
		return Bits;
	}

	int32 ConeEnvTexelIndex(
		const FMLSConeEnvBRDFSettings& Settings,
		int32 NoVIndex,
		int32 RoughnessIndex,
		int32 ConeIndex)
	{
		return (ConeIndex * Settings.RoughnessSize + RoughnessIndex) * Settings.NoVSize + NoVIndex;
	}

	bool SaveStringAtomic(const FString& Path, const FString& Value, FString& OutError)
	{
		IFileManager& FileManager = IFileManager::Get();
		if (!FileManager.MakeDirectory(*FPaths::GetPath(Path), true))
		{
			OutError = FString::Printf(TEXT("Unable to create artifact directory: %s"), *FPaths::GetPath(Path));
			return false;
		}
		const FString Temporary = Path + TEXT(".tmp");
		if (!FFileHelper::SaveStringToFile(Value, *Temporary, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			OutError = FString::Printf(TEXT("Unable to write temporary artifact: %s"), *Temporary);
			return false;
		}
		if (!FileManager.Move(*Path, *Temporary, true, true, false, true))
		{
			FileManager.Delete(*Temporary, false, true, true);
			OutError = FString::Printf(TEXT("Unable to commit artifact: %s"), *Path);
			return false;
		}
		return true;
	}
}

bool FMLSConeEnvBRDFSettings::Validate(FString& OutError) const
{
	if (NoVSize != 32 || RoughnessSize != 32 || AdjustedCosConeSize != 8)
	{
		OutError = TEXT("CARD-09 canonical representation is exactly 32x32x8.");
		return false;
	}
	if (SampleCount <= 0)
	{
		OutError = TEXT("Cone EnvBRDF sample count must be positive.");
		return false;
	}
	return true;
}

bool FMLSConeEnvBRDFGenerator::Generate(
	const FMLSConeEnvBRDFSettings& Settings,
	FMLSConeEnvBRDFData& OutData,
	FString& OutError)
{
	if (!Settings.Validate(OutError))
	{
		return false;
	}

	OutData = {};
	OutData.Settings = Settings;
	OutData.FloatAB.SetNumZeroed(Settings.NumTexels());
	OutData.PackedRG16.SetNumUninitialized(Settings.NumTexels());

	TArray<FVector2d> Sequence;
	Sequence.SetNumUninitialized(Settings.SampleCount);
	for (int32 SampleIndex = 0; SampleIndex < Settings.SampleCount; ++SampleIndex)
	{
		// Literal UE 5.7 SystemTextures.cpp sequence: E1=i/N, E2=ReverseBits(i)/2^32.
		Sequence[SampleIndex] = FVector2d(
			static_cast<double>(SampleIndex) / static_cast<double>(Settings.SampleCount),
			static_cast<double>(ReverseBits32(static_cast<uint32>(SampleIndex))) * MLSInvUint32);
	}

	const int32 GroupCount = Settings.NoVSize * Settings.RoughnessSize;
	ParallelFor(TEXT("MLS Cone EnvBRDF bake"), GroupCount, 1, [&](int32 GroupIndex)
	{
		const int32 NoVIndex = GroupIndex % Settings.NoVSize;
		const int32 RoughnessIndex = GroupIndex / Settings.NoVSize;
		// Match UE's hardware-filtered PreIntegratedGF texel-center convention.
		const double NoV = (static_cast<double>(NoVIndex) + 0.5) / static_cast<double>(Settings.NoVSize);
		const double Roughness = (static_cast<double>(RoughnessIndex) + 0.5) / static_cast<double>(Settings.RoughnessSize);
		const double Alpha = Roughness * Roughness;
		const double AlphaSquared = Alpha * Alpha;
		const FVector3d ViewDirection(FMath::Sqrt(FMath::Max(1.0 - NoV * NoV, 0.0)), 0.0, NoV);

		double SumA[8] = {};
		double SumB[8] = {};
		for (const FVector2d& Xi : Sequence)
		{
			const double Phi = MLSTwoPi * Xi.X;
			const double CosTheta = FMath::Sqrt(
				(1.0 - Xi.Y) / FMath::Max(1.0 + (AlphaSquared - 1.0) * Xi.Y, 1.0e-20));
			const double SinTheta = FMath::Sqrt(FMath::Max(1.0 - CosTheta * CosTheta, 0.0));
			const FVector3d H(SinTheta * FMath::Cos(Phi), SinTheta * FMath::Sin(Phi), CosTheta);
			const double VoH = FMath::Max(FVector3d::DotProduct(ViewDirection, H), 0.0);
			const FVector3d L = 2.0 * VoH * H - ViewDirection;
			const double NoL = FMath::Max(L.Z, 0.0);
			const double NoH = FMath::Max(H.Z, 0.0);
			if (NoL <= 0.0 || NoH <= 0.0)
			{
				continue;
			}

			// Literal UE 5.7 SystemTextures.cpp PreintegratedGF estimator.
			const double VisSmithV = NoL * (NoV * (1.0 - Alpha) + Alpha);
			const double VisSmithL = NoV * (NoL * (1.0 - Alpha) + Alpha);
			const double Vis = 0.5 / FMath::Max(VisSmithV + VisSmithL, 1.0e-20);
			const double Weight = NoL * Vis * (4.0 * VoH / NoH);
			double Fc = 1.0 - VoH;
			Fc *= FMath::Square(Fc * Fc); // (1 - VoH)^5
			const double A = Weight * (1.0 - Fc);
			const double B = Weight * Fc;

			// Axis is AdjustedCosCone: 0 = open, 1 = closed. Strict boundary.
			for (int32 ConeIndex = 0; ConeIndex + 1 < Settings.AdjustedCosConeSize; ++ConeIndex)
			{
				const double AdjustedCosCone = static_cast<double>(ConeIndex)
					/ static_cast<double>(Settings.AdjustedCosConeSize - 1);
				if (NoL > AdjustedCosCone)
				{
					SumA[ConeIndex] += A;
					SumB[ConeIndex] += B;
				}
			}
		}

		const double InvSampleCount = 1.0 / static_cast<double>(Settings.SampleCount);
		for (int32 ConeIndex = 0; ConeIndex < Settings.AdjustedCosConeSize; ++ConeIndex)
		{
			const int32 LinearIndex = ConeEnvTexelIndex(Settings, NoVIndex, RoughnessIndex, ConeIndex);
			OutData.FloatAB[LinearIndex] = FVector2f(
				static_cast<float>(FMath::Clamp(SumA[ConeIndex] * InvSampleCount, 0.0, 1.0)),
				static_cast<float>(FMath::Clamp(SumB[ConeIndex] * InvSampleCount, 0.0, 1.0)));
		}
	}, EParallelForFlags::Unbalanced);

	for (int32 Index = 0; Index < Settings.NumTexels(); ++Index)
	{
		const uint32 A = static_cast<uint32>(FMath::Clamp(FMath::RoundToInt(OutData.FloatAB[Index].X * 65535.0f), 0, 65535));
		const uint32 B = static_cast<uint32>(FMath::Clamp(FMath::RoundToInt(OutData.FloatAB[Index].Y * 65535.0f), 0, 65535));
		OutData.PackedRG16[Index] = A | (B << 16u);
	}
	OutData.DataSHA256 = FMLSMicroShadowLUTGenerator::ComputePackedDataSHA256(OutData.PackedRG16);
	return true;
}

bool FMLSConeEnvBRDFGenerator::WriteArtifacts(
	const FMLSConeEnvBRDFData& Data,
	const FMLSConeEnvBRDFArtifactPaths& Paths,
	FString& OutError)
{
	if (Data.PackedRG16.Num() != Data.Settings.NumTexels())
	{
		OutError = TEXT("Cone EnvBRDF packed payload size does not match the canonical dimensions.");
		return false;
	}

	FString Shader;
	Shader += TEXT("// Generated by MultiLobeSpec Cone EnvBRDF Generator. Do not edit.\n");
	Shader += TEXT("#ifndef MLS_CONE_ENVBRDF_GENERATED\n#define MLS_CONE_ENVBRDF_GENERATED 1\n");
	Shader += FString::Printf(TEXT("#define MLS_CONE_ENV_NOV_SIZE %d\n"), Data.Settings.NoVSize);
	Shader += FString::Printf(TEXT("#define MLS_CONE_ENV_ROUGHNESS_SIZE %d\n"), Data.Settings.RoughnessSize);
	Shader += FString::Printf(TEXT("#define MLS_CONE_ENV_ADJUSTED_COS_SIZE %d\n"), Data.Settings.AdjustedCosConeSize);
	Shader += FString::Printf(TEXT("#define MLS_CONE_ENV_TEXEL_COUNT %d\n"), Data.PackedRG16.Num());
	check(Data.PackedRG16.Num() % 4 == 0);
	const int32 WordsPerConePlane = Data.Settings.NoVSize * Data.Settings.RoughnessSize;
	check(WordsPerConePlane % 4 == 0);
	for (int32 ConeIndex = 0; ConeIndex < Data.Settings.AdjustedCosConeSize; ++ConeIndex)
	{
		Shader += FString::Printf(
			TEXT("static const uint4 MLS_CONE_ENVBRDF_PLANE_C%d[%d] =\n{\n"),
			ConeIndex,
			WordsPerConePlane / 4);
		const int32 PlaneStart = ConeIndex * WordsPerConePlane;
		for (int32 PlaneOffset = 0; PlaneOffset < WordsPerConePlane; PlaneOffset += 4)
		{
			const int32 Index = PlaneStart + PlaneOffset;
			Shader += FString::Printf(
				TEXT("\tuint4(0x%08xu, 0x%08xu, 0x%08xu, 0x%08xu)%s\n"),
				Data.PackedRG16[Index],
				Data.PackedRG16[Index + 1],
				Data.PackedRG16[Index + 2],
				Data.PackedRG16[Index + 3],
				PlaneOffset + 4 == WordsPerConePlane ? TEXT("") : TEXT(","));
		}
		Shader += TEXT("};\n");
	}
	Shader += TEXT("#endif // MLS_CONE_ENVBRDF_GENERATED\n");

	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("algorithm"), TEXT("ConeAwareEnvBRDF"));
	Root->SetStringField(TEXT("referenceEstimator"), TEXT("UE5.7_SystemTextures_PreintegratedGF"));
	TArray<TSharedPtr<FJsonValue>> Dimensions;
	Dimensions.Add(MakeShared<FJsonValueNumber>(Data.Settings.NoVSize));
	Dimensions.Add(MakeShared<FJsonValueNumber>(Data.Settings.RoughnessSize));
	Dimensions.Add(MakeShared<FJsonValueNumber>(Data.Settings.AdjustedCosConeSize));
	Root->SetArrayField(TEXT("dimensions"), Dimensions);
	TArray<TSharedPtr<FJsonValue>> AxisOrder;
	AxisOrder.Add(MakeShared<FJsonValueString>(TEXT("NoV")));
	AxisOrder.Add(MakeShared<FJsonValueString>(TEXT("Roughness")));
	AxisOrder.Add(MakeShared<FJsonValueString>(TEXT("AdjustedCosCone")));
	Root->SetArrayField(TEXT("axisOrder"), AxisOrder);
	Root->SetStringField(TEXT("NoVNodes"), TEXT("center"));
	Root->SetStringField(TEXT("roughnessNodes"), TEXT("center"));
	TArray<TSharedPtr<FJsonValue>> ConeNodes;
	for (int32 ConeIndex = 0; ConeIndex < Data.Settings.AdjustedCosConeSize; ++ConeIndex)
	{
		ConeNodes.Add(MakeShared<FJsonValueNumber>(
			static_cast<double>(ConeIndex) / static_cast<double>(Data.Settings.AdjustedCosConeSize - 1)));
	}
	Root->SetArrayField(TEXT("adjustedCosConeNodes"), ConeNodes);
	Root->SetStringField(TEXT("importanceSampling"), TEXT("GGX_NDF_H"));
	Root->SetStringField(TEXT("coneAcceptance"), TEXT("NoL_gt_AdjustedCosCone_Strict"));
	Root->SetStringField(TEXT("normalization"), TEXT("DivideByCommonTotalSampleCount"));
	Root->SetStringField(TEXT("output"), TEXT("AB_ScaleBiasAgainstF0"));
	Root->SetStringField(TEXT("quantization"), TEXT("RG16_UNORM"));
	Root->SetStringField(TEXT("packing"), TEXT("A_Low16_B_High16"));
	Root->SetStringField(TEXT("storageGrouping"), TEXT("EightConePlanes_uint4_FourConsecutivePackedWords"));
	Root->SetArrayField(TEXT("linearIndexOrder"), AxisOrder);
	Root->SetNumberField(TEXT("samplesPerCell"), Data.Settings.SampleCount);
	Root->SetStringField(TEXT("sequence"), TEXT("UE5.7_Hammersley_iOverN_ReverseBits"));
	Root->SetNumberField(TEXT("generatorVersion"), GeneratorVersion);
	Root->SetStringField(TEXT("dataSHA256"), Data.DataSHA256);

	FString Manifest;
	const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Manifest);
	if (!FJsonSerializer::Serialize(Root, Writer))
	{
		OutError = TEXT("Unable to serialize Cone EnvBRDF manifest.");
		return false;
	}
	Manifest += TEXT("\n");
	return SaveStringAtomic(Paths.ShaderInclude, Shader, OutError)
		&& SaveStringAtomic(Paths.Manifest, Manifest, OutError);
}

bool FMLSConeEnvBRDFGenerator::GetCanonicalArtifactPaths(
	FMLSConeEnvBRDFArtifactPaths& OutPaths,
	FString& OutError)
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("MultiLobeSpec"));
	if (!Plugin.IsValid())
	{
		OutError = TEXT("MultiLobeSpec plugin base directory is unavailable.");
		return false;
	}
	const FString GeneratedDir = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Resources"), TEXT("Generated"));
	OutPaths.ShaderInclude = FPaths::Combine(GeneratedDir, TEXT("MLS_ConeEnvBRDF.ush"));
	OutPaths.Manifest = FPaths::Combine(GeneratedDir, TEXT("MLS_ConeEnvBRDF.manifest.json"));
	return true;
}

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMLSConeEnvBRDFContractTest,
	"MultiLobeSpec.ConeEnvBRDF.CanonicalContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMLSConeEnvBRDFContractTest::RunTest(const FString& Parameters)
{
	FMLSConeEnvBRDFSettings Settings;
	Settings.SampleCount = 128;
	FMLSConeEnvBRDFData First;
	FMLSConeEnvBRDFData Second;
	FString Error;
	TestTrue(TEXT("First canonical bake succeeds"), FMLSConeEnvBRDFGenerator::Generate(Settings, First, Error));
	TestTrue(TEXT("Second canonical bake succeeds"), FMLSConeEnvBRDFGenerator::Generate(Settings, Second, Error));
	TestEqual(TEXT("Canonical dimensions produce 8192 texels"), First.PackedRG16.Num(), 32 * 32 * 8);
	TestEqual(TEXT("Bake is deterministic"), First.DataSHA256, Second.DataSHA256);
	TestEqual(TEXT("NoV is the fastest axis"), ConeEnvTexelIndex(Settings, 31, 0, 0), 31);
	TestEqual(TEXT("Roughness is the middle axis"), ConeEnvTexelIndex(Settings, 0, 1, 0), 32);
	TestEqual(TEXT("AdjustedCosCone is the slowest axis"), ConeEnvTexelIndex(Settings, 0, 0, 1), 32 * 32);
	for (int32 RoughnessIndex : {0, 7, 15, 31})
	{
		for (int32 NoVIndex : {0, 7, 15, 31})
		{
			const int32 ClosedIndex = ConeEnvTexelIndex(Settings, NoVIndex, RoughnessIndex, 7);
			TestEqual(TEXT("Closed cone A is exactly zero"), First.FloatAB[ClosedIndex].X, 0.0f);
			TestEqual(TEXT("Closed cone B is exactly zero"), First.FloatAB[ClosedIndex].Y, 0.0f);
			for (int32 ConeIndex = 1; ConeIndex < 8; ++ConeIndex)
			{
				const FVector2f Previous = First.FloatAB[ConeEnvTexelIndex(Settings, NoVIndex, RoughnessIndex, ConeIndex - 1)];
				const FVector2f Current = First.FloatAB[ConeEnvTexelIndex(Settings, NoVIndex, RoughnessIndex, ConeIndex)];
				TestTrue(TEXT("A is non-increasing over AdjustedCosCone"), Current.X <= Previous.X);
				TestTrue(TEXT("B is non-increasing over AdjustedCosCone"), Current.Y <= Previous.Y);
			}
		}
	}
	return true;
}
#endif
