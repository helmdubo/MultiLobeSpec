#pragma once

#include "CoreMinimal.h"

struct FMLSConeEnvBRDFSettings
{
	int32 NoVSize = 32;
	int32 RoughnessSize = 32;
	int32 AdjustedCosConeSize = 8;
	int32 SampleCount = 8192;

	int32 NumTexels() const { return NoVSize * RoughnessSize * AdjustedCosConeSize; }
	bool Validate(FString& OutError) const;
};

struct FMLSConeEnvBRDFData
{
	FMLSConeEnvBRDFSettings Settings;
	TArray<FVector2f> FloatAB;
	TArray<uint32> PackedRG16;
	FString DataSHA256;
};

struct FMLSConeEnvBRDFArtifactPaths
{
	FString ShaderInclude;
	FString Manifest;
};

/**
 * Offline CoD:WWII-style cone-aware split-sum EnvBRDF generator.
 *
 * The estimator deliberately mirrors UE 5.7 SystemTextures.cpp's
 * PreintegratedGF bake. The only additional operation is strict rejection of
 * reflected directions with NoL <= AdjustedCosCone. Runtime never performs MC.
 */
class FMLSConeEnvBRDFGenerator
{
public:
	static constexpr uint32 GeneratorVersion = 3u;

	static bool Generate(
		const FMLSConeEnvBRDFSettings& Settings,
		FMLSConeEnvBRDFData& OutData,
		FString& OutError);

	static bool WriteArtifacts(
		const FMLSConeEnvBRDFData& Data,
		const FMLSConeEnvBRDFArtifactPaths& Paths,
		FString& OutError);

	static bool GetCanonicalArtifactPaths(
		FMLSConeEnvBRDFArtifactPaths& OutPaths,
		FString& OutError);
};
