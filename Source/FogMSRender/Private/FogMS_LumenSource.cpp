#include "FogMS_LumenSource.h"

#include "DynamicRHI.h"
#include "HAL/IConsoleManager.h"
#include "Lumen/LumenSceneData.h"
#include "PooledRenderTarget.h"
#include "RenderGraphBuilder.h"
#include "RenderingThread.h"
#include "Runtime/Launch/Resources/Version.h"
#include "SceneRendering.h"
#include "SystemTextures.h"

IMPLEMENT_GLOBAL_SHADER_PARAMETER_STRUCT(FFogMSLumenCardScene, "FogMSLumenCardScene");

namespace
{
	// The only renderer-private view access of the producer path. PostTLASBuild of the deferred renderer passes an
	// actual FViewInfo, which marks itself with FSceneView::bIsViewInfo (FViewInfo constructor).
	const FViewInfo* FogMS_AsViewInfo(const FSceneView& View)
	{
		return View.bIsViewInfo ? static_cast<const FViewInfo*>(&View) : nullptr;
	}
}

FRDGBufferRef FogMS_GetPrivateLumenHitDataBuffer(const FSceneView& SceneView)
{
	const FViewInfo* View = FogMS_AsViewInfo(SceneView);
	return View ? View->LumenHardwareRayTracingHitDataBuffer : nullptr;
}

bool FogMS_GetLumenSource(FRDGBuilder& GraphBuilder, const FSceneView& SceneView,
	FFogMSLumenSourceParameters& OutParameters, FString& OutError)
{
	check(IsInRenderingThread());
	FMemory::Memzero(&OutParameters, sizeof(OutParameters));
	OutError.Reset();

#if PLATFORM_WINDOWS && ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION == 8 && ENGINE_PATCH_VERSION == 2
	const FViewInfo* const ViewInfo = FogMS_AsViewInfo(SceneView);
	if (!ViewInfo)
	{
		OutError = TEXT("Lumen source requires the renderer's scene view (FViewInfo).");
		return false;
	}
	const FViewInfo& View = *ViewInfo;
	if (!GDynamicRHI || GDynamicRHI->GetInterfaceType() != ERHIInterfaceType::D3D12
		|| View.GetShaderPlatform() != SP_PCD3D_SM6)
	{
		OutError = TEXT("Lumen source requires UE 5.8.2 D3D12 SM6.");
		return false;
	}
	// Native Nanite mode is an FAutoConsoleVariableRef, which has no typed data
	// accessor. All three CVars below are RenderThreadSafe; GetInt/GetFloat select
	// the render-thread shadow through the common IConsoleVariable interface.
	static const IConsoleVariable* const NaniteRTMode = IConsoleManager::Get().FindConsoleVariable(TEXT("r.RayTracing.Nanite.Mode"));
	if (!NaniteRTMode || NaniteRTMode->GetInt() != 0)
	{
		OutError = TEXT("Lumen source requires r.RayTracing.Nanite.Mode=0 (fallback triangle normals).");
		return false;
	}

	const FLumenSceneData* Lumen = View.ViewLumenSceneData;
	if (!Lumen || !Lumen->bFinalLightingAtlasContentsValid)
	{
		OutError = TEXT("Lumen surface lighting has not initialized for this view.");
		return false;
	}

	// A pooled pointer can still name the previous atlas during reallocation.
	// Native AllocateCardAtlases fills FrameTemporaries, then FillFrameTemporaries
	// registers pooled textures only when no replacement exists. Never register
	// an otherwise absent Lumen texture here: that could join old texels to new cards.
	const auto FindTexture = [&](const TRefCountPtr<IPooledRenderTarget>& Pooled, const TCHAR* Name) -> FRDGTextureRef
	{
		FRDGTextureRef Texture = Pooled ? GraphBuilder.FindExternalTexture(Pooled.GetReference()) : nullptr;
		if (!Texture && OutError.IsEmpty())
		{
			OutError = FString::Printf(TEXT("Lumen source waiting for current-graph %s (initializing/reallocating or Lumen inactive)."), Name);
		}
		return Texture;
	};
	const auto FindBuffer = [&](const TRefCountPtr<FRDGPooledBuffer>& Pooled, const TCHAR* Name) -> FRDGBufferRef
	{
		FRDGBufferRef Buffer = Pooled ? GraphBuilder.FindExternalBuffer(Pooled.GetReference()) : nullptr;
		if (!Buffer && OutError.IsEmpty())
		{
			OutError = FString::Printf(TEXT("Lumen source waiting for current-graph %s metadata."), Name);
		}
		return Buffer;
	};

	FRDGTextureRef FinalLighting = FindTexture(Lumen->FinalLightingAtlas, TEXT("FinalLightingAtlas"));
	FRDGTextureRef Depth = FindTexture(Lumen->DepthAtlas, TEXT("DepthAtlas"));
	FRDGTextureRef Albedo = FindTexture(Lumen->AlbedoAtlas, TEXT("AlbedoAtlas"));
	FRDGTextureRef Normal = FindTexture(Lumen->NormalAtlas, TEXT("NormalAtlas"));
	FRDGTextureRef Emissive = FindTexture(Lumen->EmissiveAtlas, TEXT("EmissiveAtlas"));
	FRDGTextureRef DirectLighting = FindTexture(Lumen->DirectLightingAtlas, TEXT("DirectLightingAtlas"));
	FRDGBufferRef Cards = FindBuffer(Lumen->CardBuffer, TEXT("CardBuffer"));
	FRDGBufferRef MeshCards = FindBuffer(Lumen->MeshCardsBuffer, TEXT("MeshCardsBuffer"));
	FRDGBufferRef CardPages = FindBuffer(Lumen->CardPageBuffer, TEXT("CardPageBuffer"));
	FRDGBufferRef PageTable = FindBuffer(Lumen->PageTableBuffer, TEXT("PageTableBuffer"));
	FRDGBufferRef InstanceMap = FindBuffer(Lumen->SceneInstanceIndexToMeshCardsIndexBuffer, TEXT("SceneInstanceIndexToMeshCardsIndexBuffer"));
	if (!OutError.IsEmpty()) return false;

	// Radiosity may be disabled. Native FinalLighting already contains the correct
	// combined result; the unused separate irradiance output then gets a black dummy.
	FRDGTextureRef IndirectLighting = nullptr;
	if (Lumen->IndirectLightingAtlas)
	{
		IndirectLighting = FindTexture(Lumen->IndirectLightingAtlas, TEXT("IndirectLightingAtlas"));
		if (!IndirectLighting) return false;
	}

	const FIntPoint AtlasSize = Lumen->GetPhysicalAtlasSize();
	if (AtlasSize.X <= 0 || AtlasSize.Y <= 0 || FinalLighting->Desc.Extent != AtlasSize
		|| Depth->Desc.Extent != AtlasSize || Albedo->Desc.Extent != AtlasSize
		|| Normal->Desc.Extent != AtlasSize || Emissive->Desc.Extent != AtlasSize
		|| DirectLighting->Desc.Extent != AtlasSize
		|| (IndirectLighting && IndirectLighting->Desc.Extent != AtlasSize))
	{
		OutError = TEXT("Lumen source atlas dimensions do not match current card metadata.");
		return false;
	}
	// Strides are the version-pinned LumenCardCommon.ush layout, in float4s.
	if (Cards->Desc.GetSize() < uint64(Lumen->Cards.Num()) * 7u * sizeof(FVector4f)
		|| MeshCards->Desc.GetSize() < uint64(Lumen->MeshCards.Num()) * 6u * sizeof(FVector4f)
		|| CardPages->Desc.GetSize() < uint64(Lumen->GetNumCardPages()) * 5u * sizeof(FVector4f)
		|| PageTable->Desc.GetSize() < uint64(Lumen->GetNumCardPages()) * 2u * sizeof(uint32)
		|| InstanceMap->Desc.GetSize() == 0 || InstanceMap->Desc.GetSize() % sizeof(uint32) != 0)
	{
		OutError = TEXT("Lumen source buffer bounds do not cover the current card/page/instance metadata.");
		return false;
	}

	static const IConsoleVariable* const CachedExposure = IConsoleManager::Get().FindConsoleVariable(TEXT("r.EyeAdaptation.CachedLightingPreExposure"));
	static const IConsoleVariable* const DepthBias = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Lumen.HardwareRayTracing.SurfaceCacheSampling.DepthBias"));
	if (!CachedExposure || !DepthBias)
	{
		OutError = TEXT("Lumen source requires the native cached-exposure and surface-cache depth-bias CVars.");
		return false;
	}
	const float CachedExposureEV = CachedExposure->GetFloat();
	const float SurfaceCacheDepthBias = DepthBias->GetFloat();
	if (!FMath::IsFinite(CachedExposureEV) || !FMath::IsFinite(SurfaceCacheDepthBias) || SurfaceCacheDepthBias <= 0.0f)
	{
		OutError = TEXT("Lumen source requires finite cached exposure and a positive finite surface-cache depth bias.");
		return false;
	}

	FFogMSLumenCardScene* Uniforms = GraphBuilder.AllocParameters<FFogMSLumenCardScene>();
	Uniforms->NumCards = Lumen->Cards.Num();
	Uniforms->NumMeshCards = Lumen->MeshCards.Num();
	Uniforms->NumCardPages = Lumen->GetNumCardPages();
	Uniforms->PhysicalAtlasSize = FVector2f(AtlasSize);
	Uniforms->InvPhysicalAtlasSize = FVector2f(1.0f / AtlasSize.X, 1.0f / AtlasSize.Y);
	// UE 5.8.2 LumenRadiosity::GetAtlasDownsampleFactor returns one.
	Uniforms->IndirectLightingAtlasDownsampleFactor = 1.0f;
	Uniforms->CardData = GraphBuilder.CreateSRV(Cards);
	Uniforms->MeshCardsData = GraphBuilder.CreateSRV(MeshCards);
	Uniforms->CardPageData = GraphBuilder.CreateSRV(CardPages);
	Uniforms->PageTableBuffer = GraphBuilder.CreateSRV(PageTable);
	Uniforms->SceneInstanceIndexToMeshCardsIndexBuffer = GraphBuilder.CreateSRV(InstanceMap);

	// The hit-index path does not traverse heightfield/primitive-group metadata.
	// Keep the complete native declaration compatible without requiring unused buffers.
	FRDGBufferSRVRef EmptyData = GraphBuilder.CreateSRV(GSystemTextures.GetDefaultStructuredBuffer(GraphBuilder, sizeof(FVector4f)));
	Uniforms->NumHeightfields = 0;
	Uniforms->NumPrimitiveGroups = 0;
	Uniforms->HeightfieldData = EmptyData;
	Uniforms->PrimitiveGroupData = EmptyData;
	Uniforms->AlbedoAtlas = Albedo;
	Uniforms->NormalAtlas = Normal;
	Uniforms->EmissiveAtlas = Emissive;
	Uniforms->DepthAtlas = Depth;

	OutParameters.FogMSLumenCardScene = GraphBuilder.CreateUniformBuffer(Uniforms);
	OutParameters.FogMSLumenDirectLightingAtlas = DirectLighting;
	OutParameters.FogMSLumenIndirectLightingAtlas = IndirectLighting ? IndirectLighting : GSystemTextures.GetBlackDummy(GraphBuilder);
	OutParameters.FogMSLumenFinalLightingAtlas = FinalLighting;
	OutParameters.FogMSLumenDepthAtlas = Depth;
	// This is cached-lighting exposure, not the current camera's PreExposure.
	OutParameters.FogMSLumenOneOverCachedLightingPreExposure = FMath::Exp2(FMath::Clamp(CachedExposureEV, -16.0f, 16.0f));
	OutParameters.FogMSLumenSurfaceCacheDepthBias = SurfaceCacheDepthBias;
	OutParameters.FogMSLumenInstanceMapCount = static_cast<uint32>(InstanceMap->Desc.GetSize() / sizeof(uint32));
	return true;
#else
	OutError = TEXT("Lumen source adapter is supported only on UE 5.8.2 Windows D3D12 SM6.");
	return false;
#endif
}
