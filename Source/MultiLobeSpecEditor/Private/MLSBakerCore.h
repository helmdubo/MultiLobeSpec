#pragma once

#include "CoreMinimal.h"

class UTexture2D;

enum class EMLSBoundaryMode : uint8 { Wrap, Clamp, Mirror };
enum class EMLSHeightReconstruction : uint8 { Auto, PeriodicFFT, Multigrid };
enum class EMLSBakePreset : uint8 { DebrisDeep, StoneBrickMedium, SandEarthShallow, Custom };

struct FMLSBakeSettings
{
	EMLSHeightReconstruction Reconstruction = EMLSHeightReconstruction::Auto;
	EMLSBoundaryMode Boundary = EMLSBoundaryMode::Wrap;
	float ReliefScale = 0.65f;
	float MaxSlope = 8.f;
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
	float FalloffStart = 0.60f;
	float Strength = 1.f;
	float OutputPower = 1.f;
	bool bDirectXNormal = true;
	bool bReconstructZ = true;
	bool bUseSourceNormalForAO = true;
	bool bWriteDebugTextures = false;
};

inline FMLSBakeSettings MLS_MakeBakePreset(const EMLSBakePreset Preset)
{
	FMLSBakeSettings Settings;
	Settings.Reconstruction = EMLSHeightReconstruction::Auto;
	Settings.Boundary = EMLSBoundaryMode::Wrap;
	Settings.bDirectXNormal = true;
	Settings.bReconstructZ = true;
	Settings.bRemoveMeanSlope = true;
	Settings.bUseSourceNormalForAO = true;
	Settings.Strength = 1.f;
	Settings.OutputPower = 1.f;

	switch (Preset)
	{
	case EMLSBakePreset::DebrisDeep:
		Settings.ReliefScale = 0.82f;
		Settings.MaxSlope = 10.f;
		Settings.RadiusPx = 36;
		Settings.Slices = 16;
		Settings.StepsPerSide = 12;
		Settings.SampleDistributionPower = 2.55f;
		Settings.FalloffStart = 0.58f;
		break;
	case EMLSBakePreset::SandEarthShallow:
		Settings.ReliefScale = 0.38f;
		Settings.MaxSlope = 6.f;
		Settings.RadiusPx = 16;
		Settings.Slices = 12;
		Settings.StepsPerSide = 8;
		Settings.SampleDistributionPower = 2.20f;
		Settings.FalloffStart = 0.55f;
		break;
	case EMLSBakePreset::Custom:
		break;
	case EMLSBakePreset::StoneBrickMedium:
	default:
		Settings.ReliefScale = 0.65f;
		Settings.MaxSlope = 8.f;
		Settings.RadiusPx = 28;
		Settings.Slices = 16;
		Settings.StepsPerSide = 10;
		Settings.SampleDistributionPower = 2.35f;
		Settings.FalloffStart = 0.60f;
		break;
	}
	return Settings;
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
		const FMLSBakeSettings& Settings,
		const FString& NormalSuffix,
		const FString& AOSuffix,
		FString& OutMessage);
};
