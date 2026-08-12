#pragma once

#include "CoreMinimal.h"

class UTexture2D;

enum class EMLSHeightReconstruction : uint8
{
	/** Periodic FFT for power-of-two wrap textures; geometric multigrid otherwise. */
	Auto,
	/** Force periodic radix-2 FFT. Falls back to multigrid when the input is incompatible. */
	PeriodicFFT,
	/** Geometric multigrid Poisson solver. */
	Multigrid
};

enum class EMLSBoundaryMode : uint8
{
	/** Tileable material: opposite edges are neighbours. */
	Wrap,
	/** Unique/atlas texture: coordinates outside the image clamp to the edge. */
	Clamp,
	/** Unique/atlas texture: coordinates outside the image mirror at the edge. */
	Mirror
};

/** Settings for the offline normal -> Poisson height -> orthographic GTAO baker. */
struct FMLSBakeSettings
{
	// Input / gradient field.
	bool bDirectXNormal = true;
	bool bReconstructZ = true;          // RG normal maps: reconstruct positive Z.
	bool bRemoveMeanSlope = true;       // Required for a periodic height field.
	float ReliefScale = 1.0f;           // Multiplies the decoded slope before integration.
	float MaxSlope = 8.0f;              // Robust guard for compressed normals near N.z = 0.

	// Poisson integration.
	EMLSHeightReconstruction Reconstruction = EMLSHeightReconstruction::Auto;
	EMLSBoundaryMode Boundary = EMLSBoundaryMode::Wrap;
	int32 MultigridVCycles = 8;
	int32 MultigridPreSmooth = 3;
	int32 MultigridPostSmooth = 3;
	int32 MultigridCoarseIterations = 64;
	int32 MultigridCoarsestSize = 16;

	// Orthographic GTAO over the reconstructed height field.
	int32 RadiusPx = 16;
	int32 Slices = 8;                    // Each slice traces both directions.
	int32 StepsPerSide = 6;
	float SampleDistributionPower = 2.0f; // >1 concentrates samples near the texel.
	float FalloffStart = 0.70f;           // Fraction of radius where far-field fade starts.
	bool bUseSourceNormalForAO = true;     // Otherwise derive the local normal from height.

	// Output remap. Raw visibility is used when both values are 1.
	float Strength = 1.0f;                // AO = 1 - Strength * (1 - Visibility).
	float OutputPower = 1.0f;
	bool bWriteDebugTextures = false;      // Height and gradient-residual assets beside AO.
};

struct FMLSBakerCore
{
	/**
	 * Full P2 pipeline:
	 * RG normal -> robust slopes -> Poisson least-squares height (FFT/MG)
	 * -> slope-aware orthographic GTAO -> stable AO asset updated in place.
	 */
	static bool BakeAOForNormal(
		UTexture2D* NormalTex,
		const FMLSBakeSettings& Settings,
		const FString& NormalSuffix,
		const FString& AOSuffix,
		FString& OutMessage);
};
