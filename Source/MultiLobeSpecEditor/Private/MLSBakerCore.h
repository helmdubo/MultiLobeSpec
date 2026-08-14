#pragma once

#include "CoreMinimal.h"

class UTexture2D;

enum class EMLSBoundaryMode : uint8 { Wrap, Clamp, Mirror };
enum class EMLSHeightReconstruction : uint8 { Auto, PeriodicFFT, Multigrid };
enum class EMLSBakePreset : uint8 { DebrisDeep, StoneBrickMedium, SandEarthShallow, Custom };

/**
 * Artist-facing physical bake parameters. Everything else (relief multiplier,
 * pixel radius, slice/step counts, sampling kernel, solver, boundary) is derived:
 * the same settings produce the same physical AO on 1K, 2K, and 4K maps.
 */
struct FMLSPhysicalBakeSettings
{
	/** Physical width of one full UV tile in centimeters (UV 0..1 == this many cm). */
	float SurfaceSizeCm = 200.0f;
	/** Target robust peak-to-valley relief height (P1..P99 of reconstructed height), cm. */
	float ReliefHeightCm = 2.0f;
	/** Physical occlusion radius, cm; defines which microrelief scale participates in visibility. */
	float OcclusionRadiusCm = 6.0f;
	/** Requested horizon samples per texel (= 2 * slices * steps); numerical quality only. */
	int32 QualitySamplesPerTexel = 320;
	/** Input convention of the source normal map (green channel direction). */
	bool bDirectXNormal = true;
	/** Write reconstructed height / gradient-error diagnostic textures next to the AO. */
	bool bWriteDebugTextures = false;
};

/** Read-only per-bake calibration report; never an input. */
struct FMLSBakeDiagnostics
{
	float CmPerTexel = 0.0f;
	float NormalImpliedHeightCm = 0.0f;
	float AppliedReliefScale = 1.0f;
	float RadiusTexels = 0.0f;
	int32 Slices = 0;
	int32 StepsPerSide = 0;
	int32 ActualSamplesPerTexel = 0;
	/** Fraction of texels whose slope was reduced by winsorization or the absolute limit. */
	float ClampedSlopeFraction = 0.0f;
	FString SolverName;
	FString BoundaryName;
	TArray<FString> Warnings;
};

/**
 * Internal derived configuration consumed by the reconstruction/GTAO passes.
 * Not exposed to the UI: values come from FMLSPhysicalBakeSettings and the
 * calibrated fixed kernel (distribution power 2.35, smoothstep falloff 0.6R..R,
 * Strength/OutputPower permanently 1 so the map stays canonical visibility).
 */
struct FMLSDerivedBakeSettings
{
	EMLSHeightReconstruction Reconstruction = EMLSHeightReconstruction::Auto;
	EMLSBoundaryMode Boundary = EMLSBoundaryMode::Wrap;
	bool bRemoveMeanSlope = true;
	int32 MultigridVCycles = 8;
	int32 MultigridPreSmooth = 3;
	int32 MultigridPostSmooth = 3;
	int32 MultigridCoarseIterations = 64;
	int32 MultigridCoarsestSize = 16;
	int32 RadiusPx = 28;
	int32 Slices = 16;
	int32 StepsPerSide = 10;
	float SampleDistributionPower = 2.35f;
	bool bDirectXNormal = true;
	bool bWriteDebugTextures = false;
};

/** Physical presets change only the physically meaningful values; Surface Size stays the user's. */
inline void MLS_ApplyPhysicalBakePreset(const EMLSBakePreset Preset, FMLSPhysicalBakeSettings& InOut)
{
	switch (Preset)
	{
	case EMLSBakePreset::DebrisDeep:
		InOut.ReliefHeightCm = 5.0f;
		InOut.OcclusionRadiusCm = 15.0f;
		InOut.QualitySamplesPerTexel = 520;
		break;
	case EMLSBakePreset::SandEarthShallow:
		InOut.ReliefHeightCm = 0.4f;
		InOut.OcclusionRadiusCm = 2.0f;
		InOut.QualitySamplesPerTexel = 192;
		break;
	case EMLSBakePreset::Custom:
		break;
	case EMLSBakePreset::StoneBrickMedium:
	default:
		InOut.ReliefHeightCm = 2.0f;
		InOut.OcclusionRadiusCm = 6.0f;
		InOut.QualitySamplesPerTexel = 320;
		break;
	}
}

/**
 * Quality -> sampling split at roughly the calibrated Slices/Steps ratio of 1.6
 * (the former Stone/Brick preset: 16 x 10 x 2 = 320 samples/texel).
 */
inline void MLS_QualityToSampling(const int32 RequestedSamples, int32& OutSlices, int32& OutStepsPerSide)
{
	const int32 Quality = FMath::Clamp(RequestedSamples, 64, 2048);
	const float RawSlices = FMath::Sqrt(0.8f * static_cast<float>(Quality));
	OutSlices = FMath::Clamp(2 * FMath::RoundToInt(RawSlices * 0.5f), 8, 32);
	OutStepsPerSide = FMath::Clamp(
		FMath::CeilToInt(static_cast<float>(Quality) / (2.0f * static_cast<float>(OutSlices))), 4, 32);
}

inline const TCHAR* MLS_BakePresetName(const EMLSBakePreset Preset)
{
	switch (Preset)
	{
	case EMLSBakePreset::DebrisDeep: return TEXT("Debris / Deep Relief");
	case EMLSBakePreset::SandEarthShallow: return TEXT("Sand / Earth / Shallow Relief");
	case EMLSBakePreset::Custom: return TEXT("Custom");
	case EMLSBakePreset::StoneBrickMedium:
	default: return TEXT("Stone / Brick / Medium Relief");
	}
}

class FMLSBakerCore
{
public:
	static bool BakeAOForNormal(
		UTexture2D* Normal,
		const FMLSPhysicalBakeSettings& Physical,
		const FString& NormalSuffix,
		const FString& AOSuffix,
		FString& OutMessage,
		FMLSBakeDiagnostics* OutDiagnostics = nullptr);
};
