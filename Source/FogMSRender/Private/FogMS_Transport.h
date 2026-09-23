#pragma once

#include "CoreMinimal.h"
#include "RenderGraphFwd.h"

class FSceneView;
struct FFogMSWorldRequest;
class FFogMSLumenSourceParameters;
class FFogMSWorldSourcesParameters;

// B2: same-frame, six-ordinate cell-average transport. The four atlas slabs are
// total incident / primary incident + connectivity / coefficients / flux metrics.
// PreviousAtlas (same layout, same Box) warm-starts the PCG solution; null is a cold start.
// OutSunTransmittance non-null (hybrid injection): pass 2 also writes a GridSize^3 R16F RDG texture of the
// per-cell atmosphere-sun transmittance T_sun (1 without that sun); set to null when the graph is unavailable.
// ShadowHitData: per-geometry-segment hit-group user data (bit 29 = CastShadow, index = instance hit-group
// contribution / RAY_TRACING_NUM_SHADER_SLOTS + geometry index) from FogMS_BuildShadowHitFlags. Null returns null (no graph).
// View: the PostTLASBuild view of a deferred scene renderer (public FSceneView API only).
FRDGTextureRef FogMS_RenderTransport(FRDGBuilder& GraphBuilder, const FSceneView& View,
	const FFogMSWorldRequest& Request, const FFogMSLumenSourceParameters& Lumen,
	const FFogMSWorldSourcesParameters& Lights, bool bIndirect, FRDGBufferRef ShadowHitData,
	FRDGTextureRef PreviousAtlas = nullptr, FRDGTextureRef* OutSunTransmittance = nullptr);

// PostTLASBuild, render thread. Per-segment hit-group user data built from the view's visible ray tracing
// shader bindings (UE::FXRenderingUtils::RayTracing), exactly as the engine's Lumen hit-group buffer:
// entry SBTRecordIndex / RAY_TRACING_NUM_SHADER_SLOTS, bit 29 = FRayTracingMeshCommand::bCastRayTracedShadows.
// Sized max index + 1; entries without a visible binding (and, in the shader, indices past the end) cast shadows.
// Null without a render scene (or without RHI_RAYTRACING).
FRDGBufferRef FogMS_BuildShadowHitFlags(FRDGBuilder& GraphBuilder, const FSceneView& View);
