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
END_SHADER_PARAMETER_STRUCT()

/** Render-thread/PostTLAS adapter for UE 5.8.2 D3D12 SM6 and Nanite RT fallback mode 0.
 * Include FFogMSLumenSourceParameters in the caller's shader parameters. The caller
 * supplies View/PrimaryView, ray tracing metadata and a valid geometric hit normal.
 * Reads only the renderer's already-registered current-graph Lumen resources. A
 * first-frame/reallocation/missing-resource failure returns false with a reason;
 * the caller must not publish a valid multiple-scattering result in that case.
 * This is surface-cache radiance, not a camera-fog history or native hit-lighting pass.
 * View must be the renderer's FViewInfo (FSceneView::bIsViewInfo, as passed to PostTLASBuild);
 * the renderer-private cast and includes stay in FogMS_LumenSource.cpp. Other views fail with a reason.
 */
bool FogMS_GetLumenSource(FRDGBuilder& GraphBuilder, const FSceneView& View,
	FFogMSLumenSourceParameters& OutParameters, FString& OutError);

/** r.FogMS.Transport.PublicHitFlags 0 fallback (P5, A/B only): the renderer-private
 * FViewInfo::LumenHardwareRayTracingHitDataBuffer of View, or null (none this frame, or not an FViewInfo). */
FRDGBufferRef FogMS_GetPrivateLumenHitDataBuffer(const FSceneView& View);
