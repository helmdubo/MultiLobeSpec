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
	/** World-space unit vector toward the atmosphere sun light; zero when there is none.
	 * B3 transport rotates its angular quadrature so one ordinate points exactly at the sun.
	 */
	FVector3f DirectionToSun = FVector3f::ZeroVector;
	FFogMSWorldRequest() { FMemory::Memzero(BoxRows, sizeof(BoxRows)); }
};

/** PostTLAS, graphics queue only. Resident atlas: N x (2*N*N).
 * Lower half: additional incident radiance, including per-order damping.
 * Upper half: primary sky + surface/emissive incident radiance.
 * Both are scene-linear, g=0, no receiver sigma_s or camera pre-exposure.
 */
FOGMSRENDER_API FFogMSSpatialResult FogMS_BuildWorldLighting(
	FRDGBuilder& GraphBuilder, const FSceneView& View, const FFogMSWorldRequest& Request);
FOGMSRENDER_API void FogMS_ShutdownWorldLighting_RenderThread(FRHICommandListImmediate& RHICmdList);
FOGMSRENDER_API void FogMS_InvalidateWorldLighting_RenderThread();
/** Returns false when there is no current world-mode producer; caller may dump legacy. */
FOGMSRENDER_API bool FogMS_DumpWorldLighting_RenderThread(FRHICommandListImmediate& RHICmdList, const FString& PathPrefix);
