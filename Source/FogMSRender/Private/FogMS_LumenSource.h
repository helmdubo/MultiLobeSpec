#pragma once

#include "CoreMinimal.h"
#include "RenderGraphFwd.h"
#include "ShaderParameterStruct.h"

class FRDGBuilder;
class FSceneView;

// Plugin-owned metadata. Do not import the unexported FLumenCardScene metadata.
// Field names match the UE 5.8.2 LumenCardCommon / SurfaceCacheSampling contract.
BEGIN_GLOBAL_SHADER_PARAMETER_STRUCT(FFogMSLumenCardScene, )
	SHADER_PARAMETER(uint32, NumCards)
	SHADER_PARAMETER(uint32, NumMeshCards)
	SHADER_PARAMETER(uint32, NumCardPages)
	SHADER_PARAMETER(uint32, NumHeightfields)
	SHADER_PARAMETER(uint32, NumPrimitiveGroups)
	SHADER_PARAMETER(FVector2f, PhysicalAtlasSize)
	SHADER_PARAMETER(FVector2f, InvPhysicalAtlasSize)
	SHADER_PARAMETER(float, IndirectLightingAtlasDownsampleFactor)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, CardData)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, CardPageData)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, MeshCardsData)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, HeightfieldData)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, PrimitiveGroupData)
	SHADER_PARAMETER_RDG_BUFFER_SRV(ByteAddressBuffer, PageTableBuffer)
	SHADER_PARAMETER_RDG_BUFFER_SRV(ByteAddressBuffer, SceneInstanceIndexToMeshCardsIndexBuffer)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, AlbedoAtlas)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, NormalAtlas)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, EmissiveAtlas)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, DepthAtlas)
END_GLOBAL_SHADER_PARAMETER_STRUCT()

BEGIN_SHADER_PARAMETER_STRUCT(FFogMSLumenSourceParameters, )
	SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FFogMSLumenCardScene, FogMSLumenCardScene)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, FogMSLumenDirectLightingAtlas)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, FogMSLumenIndirectLightingAtlas)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, FogMSLumenFinalLightingAtlas)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, FogMSLumenDepthAtlas)
	SHADER_PARAMETER(float, FogMSLumenOneOverCachedLightingPreExposure)
	SHADER_PARAMETER(float, FogMSLumenSurfaceCacheDepthBias)
	SHADER_PARAMETER(uint32, FogMSLumenInstanceMapCount)
	// Transport boundary hits (FogMS_Transport.usf BoundaryRadiance, passes 1/14; uniform branch, no permutation):
	// 1 = surface-cache radiance (FogMS_SurfaceRadiance), 0 = public fallback FogMSFallbackAlbedo * (sun + SH sky).
	// Set by the caller after FogMS_GetLumenSource / FogMS_GetFallbackLumenSource (both leave it 0).
	SHADER_PARAMETER(uint32, FogMSLumenBounce)
	// r.FogMS.World.FallbackAlbedo clamped to [0,1]: neutral diffuse albedo of a fallback hit surface.
	SHADER_PARAMETER(float, FogMSFallbackAlbedo)
END_SHADER_PARAMETER_STRUCT()

/** Render-thread/PostTLAS adapter for UE 5.8.2 D3D12 SM6 and Nanite RT fallback mode 0.
 * Include FFogMSLumenSourceParameters in the caller's shader parameters. The caller
 * supplies View/PrimaryView, ray tracing metadata and a valid geometric hit normal.
 * Reads only the renderer's already-registered current-graph Lumen resources. A
 * first-frame/reallocation/missing-resource failure returns false with a reason (OutError).
 * World (non-transport) callers must not publish a valid multiple-scattering result then; Transport
 * callers bind FogMS_GetFallbackLumenSource instead and select the public fallback (FogMSLumenBounce 0).
 * This is surface-cache radiance, not a camera-fog history or native hit-lighting pass.
 * View must be the renderer's FViewInfo (FSceneView::bIsViewInfo, as passed to PostTLASBuild);
 * the renderer-private cast and includes stay in FogMS_LumenSource.cpp. Other views fail with a reason.
 * Engine builds other than 5.8.2 (patch-specific card/page strides) compile a stub that always returns false
 * with the reason "Lumen surface cache layout verified for 5.8.2 only".
 * Leaves FogMSLumenBounce and FogMSFallbackAlbedo zero: the caller selects the branch.
 */
bool FogMS_GetLumenSource(FRDGBuilder& GraphBuilder, const FSceneView& View,
	FFogMSLumenSourceParameters& OutParameters, FString& OutError);

/** Every engine version. Valid black/empty bindings for every FFogMSLumenSourceParameters resource (empty card
 * scene, instance map count 0), so a shader that keeps the surface-cache code behind a uniform branch still binds
 * valid resources. Reads no renderer-private state. Leaves FogMSLumenBounce and FogMSFallbackAlbedo zero.
 */
void FogMS_GetFallbackLumenSource(FRDGBuilder& GraphBuilder, FFogMSLumenSourceParameters& OutParameters);

/** r.FogMS.Transport.PublicHitFlags 0 fallback (P5, A/B only): the renderer-private
 * FViewInfo::LumenHardwareRayTracingHitDataBuffer of View, or null (none this frame, or not an FViewInfo). */
FRDGBufferRef FogMS_GetPrivateLumenHitDataBuffer(const FSceneView& View);
