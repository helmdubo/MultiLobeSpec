#pragma once

#include "CoreMinimal.h"
#include "ShaderParameterStruct.h"

class FRDGBuilder;
class FSceneView;
struct FFogMSWorldSky;

// Include this with SHADER_PARAMETER_STRUCT_INCLUDE(..., WorldSources). The .ush
// declares the same flattened names; it needs Common.ush, but no native light UB.
// The source grid is 32^3. Changing it also requires changing the local-light
// distance bias below. These lights are collected from Scene->Lights, not a view
// light grid. View is used only for translated coordinates and native exposure.
BEGIN_SHADER_PARAMETER_STRUCT(FFogMSWorldSourcesParameters, )
	SHADER_PARAMETER(uint32, FogMSWorldNumLights)
	SHADER_PARAMETER(float, FogMSWorldDistanceBiasSqr)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, FogMSWorldLights)
	SHADER_PARAMETER_RDG_TEXTURE(TextureCube, FogMSWorldSkyTexture)
	SHADER_PARAMETER_SAMPLER(SamplerState, FogMSWorldSkySampler)
	SHADER_PARAMETER_RDG_TEXTURE(TextureCube, FogMSWorldSkyBlendTexture)
	SHADER_PARAMETER_SAMPLER(SamplerState, FogMSWorldSkyBlendSampler)
	// Sky volumetric scattering intensity (0 = no sky term). The shader multiplies by View.SkyLightColor from the
	// bound View uniform buffer (FogMS_WorldSky), i.e. exactly the former CPU product SkyLightColor * intensity.
	SHADER_PARAMETER(float, FogMSWorldSkyIntensity)
	SHADER_PARAMETER(float, FogMSWorldSkyBlend)
	// Active sky boundary source, chosen by FogMS_GetWorldSources (r.FogMS.World.SkySource): 0 none (black), 1 cubemap
	// (public processed capture, or the public processed capture; sector mip + sun-exclusion cone), 2 Sky View LUT
	// (View UB), 3 sky SH (View UB SkyIrradianceEnvironmentMap, Lambert band weights removed).
	SHADER_PARAMETER(uint32, FogMSWorldSkySource)
	// 1 when the sky light has Real Time Capture on but the bound source is NOT the RTC cubemap/SH (LUT or static
	// capture): View.SkyLightColor then carries 1/cached lighting pre-exposure (SceneRendering.cpp:2091-2101), which the
	// shader cancels with View.RealTimeReflectionCapturePreExposure (the same cached value, SceneRendering.cpp:2076).
	SHADER_PARAMETER(float, FogMSWorldSkyUndoRTCExposure)
	// Sky View LUT source only: rgb = USkyLightComponent::LowerHemisphereColor, a = its coverage (0 = off), applied to
	// world directions with z < 0 exactly as the RTC capture composites it (ReflectionEnvironmentShaders.usf:446-453).
	SHADER_PARAMETER(FVector4f, FogMSWorldSkyLowerHemisphere)
	// Sky View LUT source only: taps of the sector average (r.FogMS.World.SkyLutSamples, clamped 1..13; 1 = point sample).
	SHADER_PARAMETER(uint32, FogMSWorldSkyLutSamples)
	// Added to the sector-matched sky cubemap mip FogMS_WorldSky picks when given a
	// sector solid angle (r.FogMS.World.SkyMipBias). Not applied to point samples.
	SHADER_PARAMETER(float, FogMSWorldSkyMipBias)
	// Unit vector toward the sun (world space, same Direction row FogMS_WorldLight
	// uses for the directional light); zero when no directional light is gathered.
	SHADER_PARAMETER(FVector3f, FogMSWorldSunDirection)
	// cos(half-angle) of the cone removed from the captured sky radiance around the
	// sun disc (r.FogMS.World.SunExcludeDegrees); 2 = never (no sun or cvar 0).
	SHADER_PARAMETER(float, FogMSWorldSunExcludeCos)
	// Light-list index (FogMSWorldLights row block) of the atmosphere sun, Scene.AtmosphereLights[0]; -1 when
	// that light is not gathered (none, hidden, zero colour). Only the atmosphere sun, never a fallback
	// directional light: hybrid injection darkens native single scattering by this light's transmittance.
	SHADER_PARAMETER(int32, FogMSWorldSunLightIndex)
END_SHADER_PARAMETER_STRUCT()

// Render thread, after light proxies / View UB have been updated. BoxExtent is
// the OBB half extent; its enclosing sphere conservatively selects local lights.
// Direct output includes g=0 phase and VolumetricScatteringIntensity, but neither
// geometry/medium attenuation, receiver sigma_s nor View.PreExposure.
// Sky output is radiance with native RTC de-exposure and sky volumetric intensity,
// without phase, in the same units for every r.FogMS.World.SkySource (FogMS_WorldSky).
// Apply it only to escaped sky rays, not surface GI.
// Caller owns all geometry and medium visibility. Shadows use one central ray:
// source-radius / source-angle penumbrae are not reproduced by this interface.
// Rect / IES / light-function / baked-static / native cloud-shadow sources
// intersecting this Box fail explicitly. Camera MaxDrawDistance fading and
// screen-froxel LightSoftFading are intentionally not applied to this world grid.
// View: FSceneView of the PostTLASBuild callback; the caller's shader must bind that view's View uniform buffer.
// Sky: game-thread snapshot (FFogMSWorldRequest::Sky) for the public sky sources; OutSkySource names the bound one.
bool FogMS_GetWorldSources(FRDGBuilder& GraphBuilder, const FSceneView& View,
	FVector BoxCenterWS, FVector3f BoxExtent, const FFogMSWorldSky& Sky,
	FFogMSWorldSourcesParameters& OutParameters, FString& OutSkySource, FString& Error);
