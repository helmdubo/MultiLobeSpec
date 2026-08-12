#pragma once

#include "CoreMinimal.h"

/** Inputs to the article-compatible reference integral. */
struct FMLSMicroShadowReferenceInput
{
	double Visibility = 1.0;
	double Roughness = 0.5;
	double NoL = 1.0;
	double NoV = 1.0;
	double RelativeAzimuthRadians = 0.0;
};

/** Fixed deterministic sequence contract used by canonical LUT generation. */
struct FMLSMicroShadowSequenceSettings
{
	int32 SampleCount = 8192;
	uint32 Seed = 1337u;
};

struct FMLSMicroShadowIntegralResult
{
	double Value = 0.0;
	double ConeSum = 0.0;
	double HemisphereSum = 0.0;
	int32 PositiveHemisphereSamples = 0;
	int32 PositiveConeSamples = 0;
};

enum class EMLSMicroShadowAxisLayout : uint8
{
	Linear,
	Warped,
	BoundaryAware
};

enum class EMLSMicroShadowRoughnessInterpolation : uint8
{
	PerceptualRoughness,
	Alpha
};

/** Complete, versioned contract for a packed four-channel LUT. */
struct FMLSMicroShadowLUTSettings
{
	int32 VisibilitySize = 33;
	int32 NoLSize = 49;
	int32 NoVSize = 12;
	/**
	 * V1: four RGBA channels.
	 * V2: seven physical nodes stored as two overlapping RGBA banks:
	 * [0,1,2,3] and [3,4,5,6]. Runtime selects exactly one bank.
	 */
	TArray<double> RoughnessNodes = { 0.1, 0.18, 0.3, 0.45, 0.6, 0.75, 1.0 };
	EMLSMicroShadowAxisLayout AxisLayout = EMLSMicroShadowAxisLayout::BoundaryAware;
	EMLSMicroShadowRoughnessInterpolation RoughnessInterpolation =
		EMLSMicroShadowRoughnessInterpolation::PerceptualRoughness;
	FMLSMicroShadowSequenceSettings Sequence;
	double RelativeAzimuthRadians = 0.0;

	int32 NumTexels() const { return VisibilitySize * NoLSize * NoVSize; }
	int32 RoughnessBankCount() const { return RoughnessNodes.Num() == 7 ? 2 : 1; }
	int32 PackedTexelCount() const { return NumTexels() * RoughnessBankCount(); }
	int32 RoughnessNodeIndex(int32 BankIndex, int32 Channel) const { return BankIndex * 3 + Channel; }
	bool Validate(FString& OutError) const;
};

/** Float reference cells plus the exact RGBA8 payload emitted to HLSL. */
struct FMLSMicroShadowLUTData
{
	FMLSMicroShadowLUTSettings Settings;
	TArray<FVector4f> FloatTexels;
	TArray<uint32> PackedRGBA8;
	FString DataSHA256;
};

struct FMLSMicroShadowArtifactPaths
{
	FString ShaderInclude;
	FString Manifest;
	FString ValidationJson;
	FString ValidationCsv;
};

struct FMLSMicroShadowErrorMetrics
{
	int32 SampleCount = 0;
	int32 ReferenceSamplesPerPoint = 0;
	double MeanAbsoluteError = 0.0;
	double P95AbsoluteError = 0.0;
	double P99AbsoluteError = 0.0;
	double MaxAbsoluteError = 0.0;
	double QuantizationMeanAbsoluteError = 0.0;
	double QuantizationMaxAbsoluteError = 0.0;
	int32 NearBoundarySampleCount = 0;
	double NearBoundaryP95AbsoluteError = 0.0;
	double NearBoundaryMaxAbsoluteError = 0.0;
	int32 AzimuthSampleCount = 0;
	double AzimuthMeanAbsoluteError = 0.0;
	double AzimuthMaxAbsoluteError = 0.0;
	bool bPassed = false;

	struct FSample
	{
		double Visibility = 0.0;
		double Roughness = 0.0;
		double NoL = 0.0;
		double NoV = 0.0;
		double RelativeAzimuthRadians = 0.0;
		double Reference = 0.0;
		double FloatLUT = 0.0;
		double PackedLUT = 0.0;
		double AbsoluteError = 0.0;
		double QuantizationError = 0.0;
		bool bNearBoundary = false;
	};

	TArray<FSample> Samples;
};

/**
 * Deterministic CPU oracle and packed LUT generator for Generic VNDF-Based
 * Micro-Shadowing. This is editor tooling; runtime shaders only consume the
 * admitted generated include and manifest.
 */
class FMLSMicroShadowLUTGenerator
{
public:
	static constexpr uint32 GeneratorVersion = 2u;
	static constexpr uint32 SequenceVersion = 1u;

	/** Literal Dupuy-Benyoub 2023 spherical-cap sampler, isotropic alpha. */
	static FVector3d SampleGGXVNDFIsotropic(
		const FVector3d& ViewDirection,
		double Alpha,
		const FVector2d& Xi);

	static double LambdaGGX(double NoX, double Alpha);
	static double SmithHeightCorrelatedWeight(double NoV, double NoL, double Alpha);

	/** Strict article boundary: one only when X > 0, equality is zero. */
	static double StepPositive(double X) { return X > 0.0 ? 1.0 : 0.0; }

	/** Fixed-axis digitally scrambled Hammersley point. */
	static FVector2d Hammersley(uint32 Index, uint32 Count, uint32 Seed);

	static bool ValidateReferenceInput(
		const FMLSMicroShadowReferenceInput& Input,
		const FMLSMicroShadowSequenceSettings& Sequence,
		FString& OutError);

	static bool IntegrateReferenceChecked(
		const FMLSMicroShadowReferenceInput& Input,
		const FMLSMicroShadowSequenceSettings& Sequence,
		FMLSMicroShadowIntegralResult& OutResult,
		FString& OutError);

	/** Convenience wrapper retained for automation and trusted generator inputs. */
	static FMLSMicroShadowIntegralResult IntegrateReference(
		const FMLSMicroShadowReferenceInput& Input,
		const FMLSMicroShadowSequenceSettings& Sequence = {});

	static bool Generate(
		const FMLSMicroShadowLUTSettings& Settings,
		FMLSMicroShadowLUTData& OutData,
		FString& OutError);

	/** SHA-256 over explicit little-endian RGBA8 words; part of manifest V1. */
	static FString ComputePackedDataSHA256(const TArray<uint32>& PackedData);

	static double SampleFloatLUT(
		const FMLSMicroShadowLUTData& Data,
		double Visibility,
		double Roughness,
		double NoL,
		double NoV);

	static double SamplePackedLUT(
		const FMLSMicroShadowLUTData& Data,
		double Visibility,
		double Roughness,
		double NoL,
		double NoV);

	static bool WriteArtifacts(
		const FMLSMicroShadowLUTData& Data,
		const FMLSMicroShadowArtifactPaths& Paths,
		FString& OutError);

	static bool GetCanonicalArtifactPaths(
		FMLSMicroShadowArtifactPaths& OutPaths,
		FString& OutError);

	static FString AxisLayoutName(EMLSMicroShadowAxisLayout Layout);
	static FString RoughnessInterpolationName(EMLSMicroShadowRoughnessInterpolation Interpolation);

private:
	static uint32 ReverseBits32(uint32 Bits);
};

/** Quantitative validation and machine-readable report writer. */
class FMLSMicroShadowLUTValidation
{
public:
	static bool Validate(
		const FMLSMicroShadowLUTData& Data,
		int32 RandomPointCount,
		int32 ReferenceSampleCount,
		uint32 Seed,
		FMLSMicroShadowErrorMetrics& OutMetrics,
		FString& OutError);

	static bool WriteReport(
		const FMLSMicroShadowLUTData& Data,
		const FMLSMicroShadowErrorMetrics& Metrics,
		const FMLSMicroShadowArtifactPaths& Paths,
		FString& OutError);
};
