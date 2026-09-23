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
// contribution / RAY_TRACING_NUM_SHADER_SLOTS + geometry index); FogMS_BuildShadowHitFlags or, as the A/B
// fallback, the renderer's Lumen hit-data buffer. Null returns null (no graph).
// View: the PostTLASBuild view of a deferred scene renderer (public FSceneView API only).
// AltShadowHitData (r.FogMS.Transport.HitFlagsDebug 2 only, else null): the other A/B path's buffer; pass 2 then counts
// shadow candidates whose bit 29 differs between the two and records the first 32 (read back and logged by the debug state).
FRDGTextureRef FogMS_RenderTransport(FRDGBuilder& GraphBuilder, const FSceneView& View,
	const FFogMSWorldRequest& Request, const FFogMSLumenSourceParameters& Lumen,
	const FFogMSWorldSourcesParameters& Lights, bool bIndirect, FRDGBufferRef ShadowHitData,
	FRDGTextureRef PreviousAtlas = nullptr, FRDGTextureRef* OutSunTransmittance = nullptr,
	FRDGBufferRef AltShadowHitData = nullptr);

// r.FogMS.Transport.PublicHitFlags: true builds the CastShadow flags from public ray tracing bindings
// (FogMS_BuildShadowHitFlags); false uses the renderer-private Lumen hit-data buffer (A/B fallback).
bool FogMS_UsePublicShadowHitFlags();

// PostTLASBuild, render thread. Per-segment hit-group user data built from the view's visible ray tracing
// shader bindings (UE::FXRenderingUtils::RayTracing), exactly as the engine's Lumen hit-group buffer:
// entry SBTRecordIndex / RAY_TRACING_NUM_SHADER_SLOTS, bit 29 = FRayTracingMeshCommand::bCastRayTracedShadows.
// Sized max index + 1; entries without a visible binding (and, in the shader, indices past the end) cast shadows.
// Null without a render scene (or without RHI_RAYTRACING).
FRDGBufferRef FogMS_BuildShadowHitFlags(FRDGBuilder& GraphBuilder, const FSceneView& View);

// r.FogMS.Transport.HitFlagsDebug clamped to 0..2 (render thread). 0 off; 1 CPU comparison of both CastShadow buffers;
// 2 CPU comparison plus the per-candidate GPU check in transport pass 2.
int32 FogMS_HitFlagsDebugMode();

// HitFlagsDebug >= 1, render thread, PostTLASBuild, before FogMS_RenderTransport of the same view. Logs the previous readback
// set when ready ('HitFlagsDebug' lines), then (at most once per r.FogMS.Transport.HitFlagsDebugInterval frames, one set in
// flight) queues readbacks of both buffers of this graph with the CPU snapshot of the visible bindings. PrivateHitData may be null.
void FogMS_QueueHitFlagsCompare(FRDGBuilder& GraphBuilder, const FSceneView& View, FRDGBufferRef PublicHitData, FRDGBufferRef PrivateHitData);
