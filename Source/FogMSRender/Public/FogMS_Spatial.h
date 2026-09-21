#pragma once

#include "CoreMinimal.h"
#include "RHIResources.h"
#include "RenderGraphFwd.h"

class FSceneView;
class FRHICommandListImmediate;

/** Experimental isotropic transport from the previous native, camera-frustum fog source.
 * Revision must change when density, lighting mode or other source-affecting settings change.
 * All positions/ranges are centimetres. Axes are orthonormal; Extent is positive half extent.
 */
struct FFogMSSpatialRequest
{
	FVector CenterWS = FVector::ZeroVector;
	FVector3f AxisX = FVector3f(1, 0, 0);
	FVector3f AxisY = FVector3f(0, 1, 0);
	FVector3f AxisZ = FVector3f(0, 0, 1);
	FVector3f Extent = FVector3f::ZeroVector;
	uint64 Revision = 0;
	float RangeCm = 300.0f;
	float PhaseG = 0.0f;
	int32 Steps = 12;
	int32 Directions = 12;
	bool ResetHistory = false;
};

struct FFogMSSpatialResult
{
	FTextureRHIRef Texture;
	FShaderResourceViewRHIRef SRV;
	FRDGTextureRef GraphTexture = nullptr;
	uint32 DescriptorIndex = MAX_uint32;
	int32 GridSize = 32;
	bool Valid = false;
	FString Error;
};

/** Call only from PostTLASBuild_RenderThread for a deferred, single-view real-time family.
 * Adds a graphics-queue CS, copy, and transition for later native bindless consumers.
 * Output is a resident float4 Texture2D atlas: (x, y + z * 32), at OBB cell centres.
 * RGB is de-exposed angular-average incoming radiance J; no receiver sigma_s, gain or
 * pre-exposure is applied. Alpha is the fraction of valid history samples (diagnostic only).
 * Missing history/config changes fail closed for a warm-up frame; they are never edge-clamped.
 * Geometry uses nearest opaque triangle hits; masked geometry is treated as opaque and
 * procedural primitives are excluded. This is a short-range preview, not offscreen GI.
 */
FOGMSRENDER_API FFogMSSpatialResult FogMS_BuildSpatial(
	FRDGBuilder& GraphBuilder, const FSceneView& View, const FFogMSSpatialRequest& Request);

/** Unregister the caller's view extension and drain its render commands before this call.
 * Render thread only. Waits for GPU completion before releasing resident bindless views.
 */
FOGMSRENDER_API void FogMS_ShutdownSpatial_RenderThread(FRHICommandListImmediate& RHICmdList);

/** Explicit diagnostic only; stalls for a raw linear RGBA32F readback of the last valid
 * atlas. Writes PathPrefix.rgba32f and a completion PathPrefix.json (success or reason).
 * Invoke after the producing graph executes. No capture or steady-frame overhead.
 */
FOGMSRENDER_API void FogMS_DumpSpatial_RenderThread(FRHICommandListImmediate& RHICmdList, const FString& PathPrefix);
