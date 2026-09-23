#pragma once

#include "FogMS_Spatial.h"

/** Current-frame isotropic lighting in the authored Box, independent of fog history.
 * Native Lumen surface-cache radiance remains subject to native scene coverage.
 */
struct FFogMSWorldRequest : FFogMSSpatialRequest
{
	FVector4f BoxRows[24];
	float Strength = 0.35f;
	bool bTransport = false;
	int32 Iterations = 24;
	/** Transport convergence tolerance of this Box (AFogMSBoxVolume::TransportTolerance), clamped to [0,1]
	 * by the solver. Negative: use r.FogMS.Transport.Tolerance. Must be finite. Does not affect warm start.
	 */
	float Tolerance = -1.f;
	/** World-space unit vector toward the atmosphere sun light; zero when there is none.
	 * B3 transport rotates its angular quadrature so one ordinate points exactly at the sun.
	 */
	FVector3f DirectionToSun = FVector3f::ZeroVector;
	/** Render thread only. Transport + Emissive Injection: 32^3 UAV-capable Texture3D (FloatRGBA or
	 * RGBA32F) that receives total incident J per Box cell (alpha 1), then is left in SRV state for
	 * the Box Volume material. Null: no field write. An invalid texture fails the request.
	 */
	FTextureRHIRef InjectionTexture;
	/** Hybrid injection (packet row 23.w == 6): native fog keeps all single scattering, so InjectionTexture receives
	 * RGB = J_ms = max(total - uncollided incident, 0) and A = 0.5 + 0.5 * T_sun, the per-cell transmittance toward
	 * the atmosphere sun (medium x RT visibility, 1 without that sun). Requires InjectionTexture; false = full field.
	 */
	bool bHybridInjection = false;
	/** Render thread only; required. The Box's resident BGRA8 density atlas (FFogMSDensityAtlas, X by Y+Z*SizeY,
	 * left in SRV state after its upload). Producers bind it as an ordinary SRV, so it works without -BindlessAll;
	 * packet row 7.z (its heap index) is for overlay consumers only. Null or non-2D fails the request.
	 */
	FTextureRHIRef DensityAtlas;
	/** Render thread only; Transport only. Set when FogMS_TransportPublishesLate(GraphBuilder): the async solver
	 * writes transient graph textures only, and FogMS_PublishWorldLightingLate copies them into the resident atlas
	 * and InjectionTexture from PrePostProcessPass of the same graph. Consumers see the field one frame late.
	 */
	bool bLatePublish = false;
	FFogMSWorldRequest() { FMemory::Memzero(BoxRows, sizeof(BoxRows)); }
};

/** PostTLAS, graphics queue only. Resident atlas: N x (2*N*N).
 * Without BindlessAll only Transport + InjectionTexture is accepted: no resident atlas or descriptor is created
 * (Result.Texture null, DescriptorIndex MAX_uint32); the field reaches fog through InjectionTexture alone.
 * Lower half: additional incident radiance, including per-order damping.
 * Upper half: primary sky + surface/emissive incident radiance.
 * Both are scene-linear, g=0, no receiver sigma_s or camera pre-exposure.
 */
FOGMSRENDER_API FFogMSSpatialResult FogMS_BuildWorldLighting(
	FRDGBuilder& GraphBuilder, const FSceneView& View, const FFogMSWorldRequest& Request);
/** True when this graph runs the Transport solver on async compute (r.FogMS.Transport.AsyncCompute resolved on;
 * the solver's own predicate). The caller then sets bLatePublish and defers every graphics consumer of the solve.
 */
FOGMSRENDER_API bool FogMS_TransportPublishesLate(const FRDGBuilder& GraphBuilder);
/** Render thread, before this graph's late copy. The resident Transport atlas of this view when the previous
 * graph completed its late copy (FogMS_PublishWorldLightingLate) for the same Box bounds (BoxRows 0..4).
 * False otherwise: the caller must publish no field (fail closed).
 */
FOGMSRENDER_API bool FogMS_GetLateTransportField(const FSceneView& View, const FVector4f* BoxRows, uint32& OutDescriptorIndex, int32& OutGridSize);
/** PrePostProcessPass of the graph whose PostTLAS build set bLatePublish. Graphics copies: transient atlas ->
 * resident atlas (only when one exists, i.e. BindlessAll) and, when requested, transient field -> InjectionTexture,
 * each left in external SRV access. False when this graph has no pending late publication for this view or
 * nothing was copied (nothing is written).
 */
FOGMSRENDER_API bool FogMS_PublishWorldLightingLate(FRDGBuilder& GraphBuilder, const FSceneView& View, bool& bOutInjectionCopied);
FOGMSRENDER_API void FogMS_ShutdownWorldLighting_RenderThread(FRHICommandListImmediate& RHICmdList);
FOGMSRENDER_API void FogMS_InvalidateWorldLighting_RenderThread();
/** Returns false when there is no current world-mode producer; caller may dump legacy. */
FOGMSRENDER_API bool FogMS_DumpWorldLighting_RenderThread(FRHICommandListImmediate& RHICmdList, const FString& PathPrefix);
