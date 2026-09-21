#pragma once

#include "CoreMinimal.h"
#include "ShaderParameterStruct.h"

class FRDGBuilder;
class FViewInfo;

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
	SHADER_PARAMETER(FVector3f, FogMSWorldSkyColor)
	SHADER_PARAMETER(float, FogMSWorldSkyBlend)
END_SHADER_PARAMETER_STRUCT()

// Render thread, after light proxies / View UB have been updated. BoxExtent is
// the OBB half extent; its enclosing sphere conservatively selects local lights.
// Direct output includes g=0 phase and VolumetricScatteringIntensity, but neither
// geometry/medium attenuation, receiver sigma_s nor View.PreExposure.
// Sky output is mip-0 radiance with native RTC de-exposure and sky volumetric
// intensity, without phase. Apply it only to escaped sky rays, not surface GI.
// Caller owns all geometry and medium visibility. Shadows use one central ray:
// source-radius / source-angle penumbrae are not reproduced by this interface.
// Rect / IES / light-function / baked-static / native cloud-shadow sources
// intersecting this Box fail explicitly. Camera MaxDrawDistance fading and
// screen-froxel LightSoftFading are intentionally not applied to this world grid.
bool FogMS_GetWorldSources(FRDGBuilder& GraphBuilder, const FViewInfo& View,
	FVector BoxCenterWS, FVector3f BoxExtent,
	FFogMSWorldSourcesParameters& OutParameters, FString& Error);
