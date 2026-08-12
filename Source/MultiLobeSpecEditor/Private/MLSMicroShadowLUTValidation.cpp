#include "MLSMicroShadowLUTGenerator.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Math/RandomStream.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#include <limits>

namespace
{
	// UE unity builds concatenate module .cpp files; keep every internal symbol file-prefixed.
	double Percentile(TArray<double> Values, double Fraction)
	{
		if (Values.IsEmpty())
		{
			return 0.0;
		}
		Values.Sort();
		const int32 Index = FMath::Clamp(
			FMath::CeilToInt(FMath::Clamp(Fraction, 0.0, 1.0) * static_cast<double>(Values.Num() - 1)),
			0,
			Values.Num() - 1);
		return Values[Index];
	}

	bool SaveReportAtomic(const FString& Path, const FString& Contents, FString& OutError)
	{
		IFileManager& FileManager = IFileManager::Get();
		if (!FileManager.MakeDirectory(*FPaths::GetPath(Path), true))
		{
			OutError = FString::Printf(TEXT("Unable to create report directory: %s"), *FPaths::GetPath(Path));
			return false;
		}
		const FString Temporary = Path + TEXT(".tmp");
		FileManager.Delete(*Temporary, false, true, true);
		if (!FFileHelper::SaveStringToFile(Contents, *Temporary, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
			|| !FileManager.Move(*Path, *Temporary, true, true, false, true))
		{
			FileManager.Delete(*Temporary, false, true, true);
			OutError = FString::Printf(TEXT("Unable to atomically write report: %s"), *Path);
			return false;
		}
		return true;
	}
}

bool FMLSMicroShadowLUTValidation::Validate(
	const FMLSMicroShadowLUTData& Data,
	int32 RandomPointCount,
	int32 ReferenceSampleCount,
	uint32 Seed,
	FMLSMicroShadowErrorMetrics& OutMetrics,
	FString& OutError)
{
	OutMetrics = {};
	FString SettingsError;
	if (!Data.Settings.Validate(SettingsError)
		|| Data.FloatTexels.Num() != Data.Settings.PackedTexelCount()
		|| Data.PackedRGBA8.Num() != Data.Settings.PackedTexelCount())
	{
		OutError = SettingsError.IsEmpty() ? TEXT("LUT data is incomplete.") : SettingsError;
		return false;
	}
	if (RandomPointCount <= 0 || ReferenceSampleCount <= 0)
	{
		OutError = TEXT("Validation point and reference sample counts must be positive.");
		return false;
	}

	FRandomStream Random(static_cast<int32>(Seed));
	FMLSMicroShadowSequenceSettings Sequence;
	Sequence.SampleCount = ReferenceSampleCount;
	Sequence.Seed = Seed ^ 0xa511e9b3u;
	TArray<double> Errors;
	TArray<double> QuantizationErrors;
	TArray<double> NearBoundaryErrors;
	Errors.Reserve(RandomPointCount);
	QuantizationErrors.Reserve(RandomPointCount);
	OutMetrics.Samples.Reserve(RandomPointCount);

	for (int32 Index = 0; Index < RandomPointCount; ++Index)
	{
		FMLSMicroShadowErrorMetrics::FSample Sample;
		Sample.bNearBoundary = (Index % 5) == 0;
		if (Sample.bNearBoundary)
		{
			Sample.Visibility = FMath::Lerp(0.01, 0.20, static_cast<double>(Random.GetFraction()));
			Sample.Roughness = FMath::Lerp(0.10, 0.20, static_cast<double>(Random.GetFraction()));
			Sample.NoL = FMath::Lerp(0.75, 1.00, static_cast<double>(Random.GetFraction()));
		}
		else
		{
			Sample.Visibility = Random.GetFraction();
			Sample.Roughness = FMath::Lerp(0.10, 1.00, static_cast<double>(Random.GetFraction()));
			Sample.NoL = Random.GetFraction();
		}
		Sample.NoV = Random.GetFraction();
		Sample.RelativeAzimuthRadians = Data.Settings.RelativeAzimuthRadians;

		FMLSMicroShadowReferenceInput Input;
		Input.Visibility = Sample.Visibility;
		Input.Roughness = Sample.Roughness;
		Input.NoL = Sample.NoL;
		Input.NoV = Sample.NoV;
		Input.RelativeAzimuthRadians = Sample.RelativeAzimuthRadians;
		FMLSMicroShadowIntegralResult ReferenceResult;
		if (!FMLSMicroShadowLUTGenerator::IntegrateReferenceChecked(Input, Sequence, ReferenceResult, OutError))
		{
			return false;
		}

		Sample.Reference = ReferenceResult.Value;
		Sample.FloatLUT = FMLSMicroShadowLUTGenerator::SampleFloatLUT(
			Data, Sample.Visibility, Sample.Roughness, Sample.NoL, Sample.NoV);
		Sample.PackedLUT = FMLSMicroShadowLUTGenerator::SamplePackedLUT(
			Data, Sample.Visibility, Sample.Roughness, Sample.NoL, Sample.NoV);
		Sample.AbsoluteError = FMath::Abs(Sample.PackedLUT - Sample.Reference);
		Sample.QuantizationError = FMath::Abs(Sample.PackedLUT - Sample.FloatLUT);
		Errors.Add(Sample.AbsoluteError);
		QuantizationErrors.Add(Sample.QuantizationError);
		if (Sample.bNearBoundary)
		{
			NearBoundaryErrors.Add(Sample.AbsoluteError);
		}
		OutMetrics.Samples.Add(Sample);
	}

	OutMetrics.SampleCount = Errors.Num();
	OutMetrics.ReferenceSamplesPerPoint = ReferenceSampleCount;
	OutMetrics.NearBoundarySampleCount = NearBoundaryErrors.Num();
	for (double Error : Errors)
	{
		OutMetrics.MeanAbsoluteError += Error;
		OutMetrics.MaxAbsoluteError = FMath::Max(OutMetrics.MaxAbsoluteError, Error);
	}
	for (double Error : QuantizationErrors)
	{
		OutMetrics.QuantizationMeanAbsoluteError += Error;
		OutMetrics.QuantizationMaxAbsoluteError = FMath::Max(OutMetrics.QuantizationMaxAbsoluteError, Error);
	}
	OutMetrics.MeanAbsoluteError /= static_cast<double>(Errors.Num());
	OutMetrics.QuantizationMeanAbsoluteError /= static_cast<double>(QuantizationErrors.Num());
	OutMetrics.P95AbsoluteError = Percentile(Errors, 0.95);
	OutMetrics.P99AbsoluteError = Percentile(Errors, 0.99);
	OutMetrics.NearBoundaryP95AbsoluteError = Percentile(NearBoundaryErrors, 0.95);
	for (double Error : NearBoundaryErrors)
	{
		OutMetrics.NearBoundaryMaxAbsoluteError = FMath::Max(OutMetrics.NearBoundaryMaxAbsoluteError, Error);
	}

	// Diagnostic 5D sweep: the canonical LUT remains phi=0 by contract. These
	// values are reported but do not silently redefine the 4D acceptance gate.
	static constexpr double Azimuths[] =
	{
		0.0,
		0.7853981633974483096,
		1.5707963267948966192,
		2.3561944901923449288,
		3.1415926535897932385
	};
	for (double Azimuth : Azimuths)
	{
		for (int32 Index = 0; Index < 32; ++Index)
		{
			FMLSMicroShadowReferenceInput Input;
			Input.Visibility = Random.GetFraction();
			Input.Roughness = FMath::Lerp(0.10, 1.00, static_cast<double>(Random.GetFraction()));
			Input.NoL = Random.GetFraction();
			Input.NoV = Random.GetFraction();
			Input.RelativeAzimuthRadians = Azimuth;
			const double Reference = FMLSMicroShadowLUTGenerator::IntegrateReference(Input, Sequence).Value;
			const double Approximation = FMLSMicroShadowLUTGenerator::SamplePackedLUT(
				Data, Input.Visibility, Input.Roughness, Input.NoL, Input.NoV);
			const double Error = FMath::Abs(Approximation - Reference);
			OutMetrics.AzimuthMeanAbsoluteError += Error;
			OutMetrics.AzimuthMaxAbsoluteError = FMath::Max(OutMetrics.AzimuthMaxAbsoluteError, Error);
			++OutMetrics.AzimuthSampleCount;
		}
	}
	OutMetrics.AzimuthMeanAbsoluteError /= static_cast<double>(OutMetrics.AzimuthSampleCount);

	OutMetrics.bPassed =
		OutMetrics.MeanAbsoluteError <= 0.012
		&& OutMetrics.P95AbsoluteError <= 0.035
		&& OutMetrics.P99AbsoluteError <= 0.070
		&& OutMetrics.MaxAbsoluteError <= 0.12
		&& OutMetrics.NearBoundaryP95AbsoluteError <= 0.06
		&& OutMetrics.NearBoundaryMaxAbsoluteError <= 0.15
		&& OutMetrics.QuantizationMeanAbsoluteError <= 0.003;
	return true;
}

bool FMLSMicroShadowLUTValidation::WriteReport(
	const FMLSMicroShadowLUTData& Data,
	const FMLSMicroShadowErrorMetrics& Metrics,
	const FMLSMicroShadowArtifactPaths& Paths,
	FString& OutError)
{
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("algorithm"), TEXT("GenericVNDFBasedMicroShadowing"));
	Root->SetStringField(TEXT("dataSHA256"), Data.DataSHA256);
	Root->SetStringField(TEXT("axisLayout"), FMLSMicroShadowLUTGenerator::AxisLayoutName(Data.Settings.AxisLayout));
	Root->SetStringField(TEXT("roughnessInterpolation"), FMLSMicroShadowLUTGenerator::RoughnessInterpolationName(Data.Settings.RoughnessInterpolation));
	Root->SetNumberField(TEXT("validationSamples"), Metrics.SampleCount);
	Root->SetNumberField(TEXT("referenceSamplesPerPoint"), Metrics.ReferenceSamplesPerPoint);
	Root->SetNumberField(TEXT("meanAbsoluteError"), Metrics.MeanAbsoluteError);
	Root->SetNumberField(TEXT("p95AbsoluteError"), Metrics.P95AbsoluteError);
	Root->SetNumberField(TEXT("p99AbsoluteError"), Metrics.P99AbsoluteError);
	Root->SetNumberField(TEXT("maxAbsoluteError"), Metrics.MaxAbsoluteError);
	Root->SetNumberField(TEXT("quantizationMeanAbsoluteError"), Metrics.QuantizationMeanAbsoluteError);
	Root->SetNumberField(TEXT("quantizationMaxAbsoluteError"), Metrics.QuantizationMaxAbsoluteError);
	Root->SetNumberField(TEXT("nearBoundarySamples"), Metrics.NearBoundarySampleCount);
	Root->SetNumberField(TEXT("nearBoundaryP95AbsoluteError"), Metrics.NearBoundaryP95AbsoluteError);
	Root->SetNumberField(TEXT("nearBoundaryMaxAbsoluteError"), Metrics.NearBoundaryMaxAbsoluteError);
	Root->SetNumberField(TEXT("azimuthSamples"), Metrics.AzimuthSampleCount);
	Root->SetNumberField(TEXT("azimuthMeanAbsoluteError"), Metrics.AzimuthMeanAbsoluteError);
	Root->SetNumberField(TEXT("azimuthMaxAbsoluteError"), Metrics.AzimuthMaxAbsoluteError);
	Root->SetBoolField(TEXT("passed"), Metrics.bPassed);
	TSharedRef<FJsonObject> Thresholds = MakeShared<FJsonObject>();
	Thresholds->SetNumberField(TEXT("meanAbsoluteError"), 0.012);
	Thresholds->SetNumberField(TEXT("p95AbsoluteError"), 0.035);
	Thresholds->SetNumberField(TEXT("p99AbsoluteError"), 0.070);
	Thresholds->SetNumberField(TEXT("maxAbsoluteError"), 0.12);
	Thresholds->SetNumberField(TEXT("nearBoundaryP95AbsoluteError"), 0.06);
	Thresholds->SetNumberField(TEXT("nearBoundaryMaxAbsoluteError"), 0.15);
	Thresholds->SetNumberField(TEXT("quantizationMeanAbsoluteError"), 0.003);
	Root->SetObjectField(TEXT("acceptanceThresholds"), Thresholds);

	FString Json;
	const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Json);
	if (!FJsonSerializer::Serialize(Root, Writer))
	{
		OutError = TEXT("Unable to serialize LUT validation report.");
		return false;
	}
	Json += TEXT("\n");

	FString Csv = TEXT("visibility,roughness,NoL,NoV,relativeAzimuthRadians,reference,floatLUT,packedLUT,absoluteError,quantizationError,nearBoundary\n");
	for (const FMLSMicroShadowErrorMetrics::FSample& Sample : Metrics.Samples)
	{
		Csv += FString::Printf(
			TEXT("%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%d\n"),
			Sample.Visibility,
			Sample.Roughness,
			Sample.NoL,
			Sample.NoV,
			Sample.RelativeAzimuthRadians,
			Sample.Reference,
			Sample.FloatLUT,
			Sample.PackedLUT,
			Sample.AbsoluteError,
			Sample.QuantizationError,
			Sample.bNearBoundary ? 1 : 0);
	}

	if (!SaveReportAtomic(Paths.ValidationJson, Json, OutError))
	{
		return false;
	}
	return SaveReportAtomic(Paths.ValidationCsv, Csv, OutError);
}

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

namespace
{
	// This second anonymous block shares the same unity translation unit as the generators.
	constexpr double MLSValidationTwoPi = 6.283185307179586476925286766559;

	FVector3d ValidationSafeNormal(const FVector3d& Value, const FVector3d& Fallback)
	{
		const double LengthSquared = Value.SquaredLength();
		return LengthSquared > 1.0e-24 ? Value / FMath::Sqrt(LengthSquared) : Fallback;
	}

	// Independent Heitz 2018 sampler used only as a validation oracle.
	FVector3d SampleGGXVNDFHeitz(const FVector3d& ViewDirection, double Alpha, const FVector2d& Xi)
	{
		if (Alpha <= 1.0e-12)
		{
			return FVector3d(0.0, 0.0, 1.0);
		}

		const FVector3d ViewHemisphere = ValidationSafeNormal(
			FVector3d(Alpha * ViewDirection.X, Alpha * ViewDirection.Y, ViewDirection.Z),
			FVector3d(0.0, 0.0, 1.0));
		const double Lensq = ViewHemisphere.X * ViewHemisphere.X + ViewHemisphere.Y * ViewHemisphere.Y;
		const FVector3d Tangent1 = Lensq > 0.0
			? FVector3d(-ViewHemisphere.Y, ViewHemisphere.X, 0.0) / FMath::Sqrt(Lensq)
			: FVector3d(1.0, 0.0, 0.0);
		const FVector3d Tangent2 = FVector3d::CrossProduct(ViewHemisphere, Tangent1);
		const double Radius = FMath::Sqrt(FMath::Clamp(Xi.X, 0.0, 1.0));
		const double Phi = MLSValidationTwoPi * FMath::Clamp(Xi.Y, 0.0, 1.0);
		const double T1 = Radius * FMath::Cos(Phi);
		double T2 = Radius * FMath::Sin(Phi);
		const double S = 0.5 * (1.0 + ViewHemisphere.Z);
		T2 = (1.0 - S) * FMath::Sqrt(FMath::Max(0.0, 1.0 - T1 * T1)) + S * T2;
		const double T3 = FMath::Sqrt(FMath::Max(0.0, 1.0 - T1 * T1 - T2 * T2));
		const FVector3d NormalHemisphere = T1 * Tangent1 + T2 * Tangent2 + T3 * ViewHemisphere;
		return ValidationSafeNormal(
			FVector3d(Alpha * NormalHemisphere.X, Alpha * NormalHemisphere.Y, FMath::Max(0.0, NormalHemisphere.Z)),
			FVector3d(0.0, 0.0, 1.0));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMLSMicroShadowReferenceInvariantTest,
	"MultiLobeSpec.MicroShadow.Reference.Invariants",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMLSMicroShadowReferenceInvariantTest::RunTest(const FString& Parameters)
{
	FMLSMicroShadowSequenceSettings Sequence;
	Sequence.SampleCount = 4096;
	Sequence.Seed = 1337u;

	FMLSMicroShadowReferenceInput Input;
	Input.Roughness = 0.45;
	Input.NoL = 0.8;
	Input.NoV = 0.6;
	Input.Visibility = 0.0;
	TestEqual(TEXT("Visibility=0 endpoint"), FMLSMicroShadowLUTGenerator::IntegrateReference(Input, Sequence).Value, 0.0);
	Input.Visibility = 1.0;
	TestEqual(TEXT("Visibility=1 endpoint"), FMLSMicroShadowLUTGenerator::IntegrateReference(Input, Sequence).Value, 1.0);
	TestEqual(TEXT("Strict boundary is excluded"), FMLSMicroShadowLUTGenerator::StepPositive(0.0), 0.0);
	TestEqual(TEXT("Negative zero boundary is excluded"), FMLSMicroShadowLUTGenerator::StepPositive(-0.0), 0.0);
	TestEqual(TEXT("Smallest positive value is included"), FMLSMicroShadowLUTGenerator::StepPositive(std::numeric_limits<double>::denorm_min()), 1.0);
	TestEqual(TEXT("Lambda at surface normal"), FMLSMicroShadowLUTGenerator::LambdaGGX(1.0, 0.49), 0.0);
	const double Weight = FMLSMicroShadowLUTGenerator::SmithHeightCorrelatedWeight(0.3, 0.7, 0.49);
	TestTrue(TEXT("Height-correlated Smith weight is bounded"), Weight > 0.0 && Weight <= 1.0);

	FString InputError;
	FMLSMicroShadowReferenceInput InvalidInput = Input;
	InvalidInput.NoV = std::numeric_limits<double>::quiet_NaN();
	TestFalse(TEXT("NaN input is rejected"), FMLSMicroShadowLUTGenerator::ValidateReferenceInput(InvalidInput, Sequence, InputError));
	TestFalse(TEXT("Invalid input returns a diagnostic"), InputError.IsEmpty());

	for (double Visibility : { 0.05, 0.2, 0.5, 0.9 })
	{
		Input.Visibility = Visibility;
		const double Value = FMLSMicroShadowLUTGenerator::IntegrateReference(Input, Sequence).Value;
		TestTrue(FString::Printf(TEXT("Bounded result at V=%.3f"), Visibility), Value >= 0.0 && Value <= 1.0);
	}

	Input.Visibility = 0.3;
	const double First = FMLSMicroShadowLUTGenerator::IntegrateReference(Input, Sequence).Value;
	const double Second = FMLSMicroShadowLUTGenerator::IntegrateReference(Input, Sequence).Value;
	TestEqual(TEXT("Deterministic fixed sequence"), First, Second);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMLSMicroShadowSmallLUTContractTest,
	"MultiLobeSpec.MicroShadow.LUT.SmallContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMLSMicroShadowSmallLUTContractTest::RunTest(const FString& Parameters)
{
	FMLSMicroShadowLUTSettings Settings;
	Settings.VisibilitySize = 3;
	Settings.NoLSize = 5;
	Settings.NoVSize = 2;
	Settings.Sequence.SampleCount = 128;
	FMLSMicroShadowLUTData First;
	FMLSMicroShadowLUTData Second;
	FString Error;
	if (!TestTrue(TEXT("Small LUT generates"), FMLSMicroShadowLUTGenerator::Generate(Settings, First, Error)))
	{
		AddError(Error);
		return false;
	}
	if (!TestTrue(TEXT("Repeated small LUT generates"), FMLSMicroShadowLUTGenerator::Generate(Settings, Second, Error)))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("Expected texel count"), First.PackedRGBA8.Num(), Settings.PackedTexelCount());
	TestEqual(TEXT("SHA-256 is 64 lowercase hex characters"), First.DataSHA256.Len(), 64);
	TestTrue(TEXT("Deterministic packed data"), First.PackedRGBA8 == Second.PackedRGBA8);
	TestEqual(TEXT("Deterministic payload digest"), First.DataSHA256, Second.DataSHA256);
	TestEqual(TEXT("SHA-256 empty-vector golden"),
		FMLSMicroShadowLUTGenerator::ComputePackedDataSHA256({}),
		FString(TEXT("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855")));
	TArray<uint32> SHABytes = { 0x03020100u };
	TestEqual(TEXT("SHA-256 little-endian word golden"),
		FMLSMicroShadowLUTGenerator::ComputePackedDataSHA256(SHABytes),
		FString(TEXT("054edec1d0211f624fed0cbca9d4f9400b0e491c43742af2c5b0abebf0c990d8")));
	TestEqual(TEXT("Runtime Visibility=0 endpoint is exact"),
		FMLSMicroShadowLUTGenerator::SamplePackedLUT(First, 0.0, 0.5, 0.8, 0.6), 0.0);
	TestEqual(TEXT("Runtime Visibility=1 endpoint is exact"),
		FMLSMicroShadowLUTGenerator::SamplePackedLUT(First, 1.0, 0.5, 0.8, 0.6), 1.0);
	const double Interior = FMLSMicroShadowLUTGenerator::SamplePackedLUT(First, 0.4, 0.5, 0.8, 0.6);
	TestTrue(TEXT("Packed trilinear sample is bounded"), Interior >= 0.0 && Interior <= 1.0);
	FMLSMicroShadowLUTData Incomplete = First;
	Incomplete.PackedRGBA8.Reset();
	TestEqual(TEXT("Incomplete packed data fails closed"),
		FMLSMicroShadowLUTGenerator::SamplePackedLUT(Incomplete, 0.4, 0.5, 0.8, 0.6), 0.0);

	// Verify that the optimized grouped bake is mathematically identical to
	// the standalone reference integral at an exact warped grid point.
	FMLSMicroShadowReferenceInput GridInput;
	GridInput.Visibility = 0.5; // symmetric-ratio-square at the T=0.5 center node
	GridInput.NoL = FMath::Sqrt(1.0 - GridInput.Visibility); // exact signed-boundary center node
	GridInput.NoV = 1.0;
	const int32 GridTexel = (1 * Settings.NoLSize + 2) * Settings.VisibilitySize + 1;
	for (int32 Channel = 0; Channel < 4; ++Channel)
	{
		GridInput.Roughness = Settings.RoughnessNodes[Channel];
		const double Reference = FMLSMicroShadowLUTGenerator::IntegrateReference(GridInput, Settings.Sequence).Value;
		double Generated = 0.0;
		switch (Channel)
		{
		case 0: Generated = First.FloatTexels[GridTexel].X; break;
		case 1: Generated = First.FloatTexels[GridTexel].Y; break;
		case 2: Generated = First.FloatTexels[GridTexel].Z; break;
		default: Generated = First.FloatTexels[GridTexel].W; break;
		}
		TestTrue(FString::Printf(
			TEXT("Grouped bake matches reference channel %d (generated=%.9g reference=%.9g delta=%.9g)"),
			Channel,
			Generated,
			Reference,
			FMath::Abs(Generated - Reference)),
			FMath::Abs(Generated - Reference) <= 1.0e-6);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMLSMicroShadowSmoothLimitTest,
	"MultiLobeSpec.MicroShadow.Reference.SmoothLimit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMLSMicroShadowSmoothLimitTest::RunTest(const FString& Parameters)
{
	FMLSMicroShadowSequenceSettings Sequence;
	Sequence.SampleCount = 16384;
	FMLSMicroShadowReferenceInput Input;
	Input.Visibility = 0.36; // reference threshold NoL = 0.8
	Input.Roughness = 0.001;
	Input.NoV = 0.75;
	Input.NoL = 0.95;
	TestTrue(TEXT("Smooth limit inside visibility cone"),
		FMLSMicroShadowLUTGenerator::IntegrateReference(Input, Sequence).Value >= 0.99);
	Input.NoL = 0.65;
	TestTrue(TEXT("Smooth limit outside visibility cone"),
		FMLSMicroShadowLUTGenerator::IntegrateReference(Input, Sequence).Value <= 0.01);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMLSMicroShadowSamplerCrossCheckTest,
	"MultiLobeSpec.MicroShadow.Reference.DupuyBenyoubVsHeitz",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMLSMicroShadowSamplerCrossCheckTest::RunTest(const FString& Parameters)
{
	constexpr uint32 SampleCount = 32768u;
	const double Roughness = 0.55;
	const double Alpha = Roughness * Roughness;
	const double NoV = 0.35;
	const FVector3d ViewDirection(FMath::Sqrt(1.0 - NoV * NoV), 0.0, NoV);
	double DupuyMeanZ = 0.0;
	double DupuyMeanZ2 = 0.0;
	double HeitzMeanZ = 0.0;
	double HeitzMeanZ2 = 0.0;

	for (uint32 Index = 0; Index < SampleCount; ++Index)
	{
		const FVector2d Xi = FMLSMicroShadowLUTGenerator::Hammersley(Index, SampleCount, 1337u);
		const FVector3d Dupuy = FMLSMicroShadowLUTGenerator::SampleGGXVNDFIsotropic(ViewDirection, Alpha, Xi);
		const FVector3d Heitz = SampleGGXVNDFHeitz(ViewDirection, Alpha, Xi);
		DupuyMeanZ += Dupuy.Z;
		DupuyMeanZ2 += Dupuy.Z * Dupuy.Z;
		HeitzMeanZ += Heitz.Z;
		HeitzMeanZ2 += Heitz.Z * Heitz.Z;
	}

	DupuyMeanZ /= SampleCount;
	DupuyMeanZ2 /= SampleCount;
	HeitzMeanZ /= SampleCount;
	HeitzMeanZ2 /= SampleCount;
	TestTrue(TEXT("VNDF first moment agrees"), FMath::Abs(DupuyMeanZ - HeitzMeanZ) <= 0.005);
	TestTrue(TEXT("VNDF second moment agrees"), FMath::Abs(DupuyMeanZ2 - HeitzMeanZ2) <= 0.005);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
