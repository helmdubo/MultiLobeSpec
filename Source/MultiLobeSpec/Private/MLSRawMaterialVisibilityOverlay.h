#pragma once

#include "CoreMinimal.h"

/**
 * Shader-overlay-only transport for raw MaterialAO while
 * r.GBufferDiffuseSampleOcclusion remains enabled.
 *
 * The stock legacy GBuffer reuses GenericAO for an 8-bit diffuse-indirect
 * sample mask when GBUFFER_HAS_DIFFUSE_SAMPLE_OCCLUSION is compiled. MLS
 * micro-shadowing instead needs the continuous material visibility value.
 * UE 5.7 keeps the GenericAO encode/decode behind its own
 * GBUFFER_HAS_DIFFUSE_SAMPLE_OCCLUSION branches, so no BasePass compile guard
 * rewrite is needed. This post-patch changes only the immutable overlay copy:
 *
 *  - BasePass keeps MaterialAO instead of the stock specular-occlusion value;
 *  - GBufferPreEncode stores that scalar in GenericAO;
 *  - GBufferPostDecode restores it to GBufferAO and exposes a zero sample mask.
 *
 * Legacy/vanilla overlays are not touched. No Engine source file is modified.
 */
class FMLSRawMaterialVisibilityOverlay
{
public:
	static bool Patch(const FString& OverlayDir, FString& OutError);

private:
	static bool PatchGBufferHelpers(const FString& OverlayDir, FString& OutError);
	static void UpdateCapabilityReceipt(const FString& OverlayDir);
};
