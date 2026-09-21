#include "FogMS_Spatial.h"

#include "DynamicRHI.h"
#include "GlobalRenderResources.h"
#include "GlobalShader.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#if PLATFORM_WINDOWS
#include "ID3D12DynamicRHI.h"
#endif
#include "Math/TranslationMatrix.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "MultiGPU.h"
#include "PooledRenderTarget.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "RenderUtils.h"
#include "RHICommandList.h"
#include "RHIStaticStates.h"
#include "SceneRendering.h"
#include "SceneViewState.h"
#include "ShaderParameterStruct.h"
#include "ShaderPlatformConfig.h"
#if RHI_RAYTRACING
#include "RayTracing/RayTracingScene.h"
#endif

namespace
{
	constexpr int32 SpatialSize = 32;
	constexpr int32 SpatialDirectionCount = 12;
	constexpr int32 MaxSpatialViews = 8;
	constexpr uint64 RetireAfterCalls = 120;

	bool SupportsSpatial(EShaderPlatform Platform)
	{
		return Platform == SP_PCD3D_SM6 && FShaderPlatformConfig::IsValid(Platform)
			&& FShaderPlatformConfig::GetBindlessConfiguration(Platform) == ERHIBindlessConfiguration::All
			&& IsRayTracingEnabledForProject(Platform) && RHISupportsRayTracing(Platform)
			&& RHISupportsInlineRayTracing(Platform) && !IsForwardShadingEnabled(Platform);
	}

#if RHI_RAYTRACING
	class FFogMSSpatialCS : public FGlobalShader
	{
		DECLARE_GLOBAL_SHADER(FFogMSSpatialCS);
		SHADER_USE_PARAMETER_STRUCT(FFogMSSpatialCS, FGlobalShader);
	public:
		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
			SHADER_PARAMETER_RDG_BUFFER_SRV(RaytracingAccelerationStructure, TLAS)
			SHADER_PARAMETER_RDG_TEXTURE(Texture3D<float4>, SourceHistory)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, SourceHistoryDepth)
			SHADER_PARAMETER_SAMPLER(SamplerState, SourceHistorySampler)
			SHADER_PARAMETER(FMatrix44f, PrevTranslatedWorldToClip)
			SHADER_PARAMETER(FMatrix44f, PrevNativeClipToTranslatedWorld)
			SHADER_PARAMETER(FMatrix44f, PrevNativeTranslatedWorldToClip)
			SHADER_PARAMETER(FVector4f, PrevInvDeviceZToWorldZ)
			SHADER_PARAMETER(FVector4f, PrevGridZAndInvSize)
			SHADER_PARAMETER(FVector2f, PrevViewGridToResourceUV)
			SHADER_PARAMETER(FVector3f, HistoryUVMin)
			SHADER_PARAMETER(FVector3f, HistoryUVMax)
			SHADER_PARAMETER(FVector4f, HistoryClipXYBounds)
			SHADER_PARAMETER(FVector2f, HistoryClipDepthBounds)
			SHADER_PARAMETER(FIntVector, HistoryResourceGridSize)
			SHADER_PARAMETER(FIntPoint, HistoryViewGridSize)
			SHADER_PARAMETER(uint32, UseHistoryDepth)
			SHADER_PARAMETER(FVector3f, BoxCenterTranslated)
			SHADER_PARAMETER(FVector4f, BoxAxisX)
			SHADER_PARAMETER(FVector4f, BoxAxisY)
			SHADER_PARAMETER(FVector4f, BoxAxisZ)
			SHADER_PARAMETER(float, HistoryInvPreExposure)
			SHADER_PARAMETER(float, RangeCm)
			SHADER_PARAMETER(int32, StepCount)
			SHADER_PARAMETER(int32, GridSize)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutSpatial)
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return SupportsSpatial(Parameters.Platform);
		}
		static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
		{
			FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
			OutEnvironment.CompilerFlags.Add(CFLAG_Wave32);
			OutEnvironment.CompilerFlags.Add(CFLAG_InlineRayTracing);
		}
	};
	IMPLEMENT_GLOBAL_SHADER(FFogMSSpatialCS, "/Plugin/FogMS/Private/FogMS_Spatial.usf", "SpatialCS", SF_Compute);
#endif

	struct FSpatialViewState
	{
#if PLATFORM_WINDOWS
		TRefCountPtr<ID3D12Resource> NativeTexture;
#endif
		FTextureRHIRef Texture;
		FShaderResourceViewRHIRef SRV;
		uint32 DescriptorIndex = MAX_uint32;
		FString AllocationError;
		bool bAllocationAttempted = false;
		bool bHaveMetadata = false;
		uint32 LastFrameIndex = 0;
		uint64 LastUse = 0;
		FFogMSSpatialRequest LastRequest;
		FVector3f LastGridZ = FVector3f::ZeroVector;
		FVector3f LastInvGridSize = FVector3f::ZeroVector;
		FVector3f LastGridSize = FVector3f::ZeroVector;
		FMatrix44f LastNativeClipToTranslatedWorld = FMatrix44f::Identity;
		FMatrix44f LastNativeTranslatedWorldToClip = FMatrix44f::Identity;
		FVector4f LastInvDeviceZToWorldZ = FVector4f(0, 0, 0, 0);
		bool bLastConservativeDepth = false;

		bool EnsureResource()
		{
			if (bAllocationAttempted) return DescriptorIndex != MAX_uint32;
			bAllocationAttempted = true;
#if PLATFORM_WINDOWS
			ID3D12DynamicRHI* D3D12 = GetID3D12DynamicRHI();
			D3D12_HEAP_PROPERTIES Heap{};
			Heap.Type = D3D12_HEAP_TYPE_DEFAULT;
			Heap.CreationNodeMask = Heap.VisibleNodeMask = D3D12->RHIGetDeviceNodeMask(0);
			D3D12_RESOURCE_DESC Desc{};
			Desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			Desc.Width = SpatialSize;
			Desc.Height = SpatialSize * SpatialSize;
			Desc.DepthOrArraySize = Desc.MipLevels = Desc.SampleDesc.Count = 1;
			Desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
			Desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
			// SRV-only external wrapper: a transient RDG UAV is copied here. No native UAV
			// flag and no CREATE_NOT_RESIDENT; hidden bindless readers cannot report residency.
			const HRESULT Result = D3D12->RHIGetDevice(0)->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc,
				D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
				nullptr, IID_PPV_ARGS(NativeTexture.GetInitReference()));
			if (FAILED(Result))
			{
				AllocationError = FString::Printf(TEXT("FogMS spatial resident atlas allocation failed: 0x%08x."), static_cast<uint32>(Result));
				return false;
			}
			Texture = D3D12->RHICreateTexture2DFromResource(PF_A32B32G32R32F,
				ETextureCreateFlags::ShaderResource | ETextureCreateFlags::External, FClearValueBinding::None, NativeTexture);
			if (Texture.IsValid())
			{
				FRHICommandListImmediate& RHICmdList = FRHICommandListExecutor::GetImmediateCommandList();
				SRV = RHICmdList.CreateShaderResourceView(Texture, FRHIViewDesc::CreateTextureSRV().SetDimensionFromTexture(Texture));
				if (SRV.IsValid())
				{
					const FRHIDescriptorHandle Handle = SRV->GetBindlessHandle();
					if (Handle.IsValid() && Handle.GetIndex() < (1u << 24)) DescriptorIndex = Handle.GetIndex();
				}
			}
			if (DescriptorIndex == MAX_uint32)
				AllocationError = TEXT("FogMS spatial atlas requires an exactly representable bindless float4 SRV.");
#else
			AllocationError = TEXT("FogMS spatial preview currently supports Windows D3D12 only.");
#endif
			return DescriptorIndex != MAX_uint32;
		}
	};

	struct FRetiredSpatialView
	{
		TUniquePtr<FSpatialViewState> State;
		FGPUFenceRHIRef Fence;
	};
	TMap<uint32, TUniquePtr<FSpatialViewState>> SpatialViews;
	TArray<FRetiredSpatialView> RetiredViews;
	uint64 SpatialAccessSerial = 0;
	uint32 LastValidViewKey = 0;
	uint32 LastValidRenderFrame = 0;
	bool bLastBuildValid = false;

	void RetireView(TUniquePtr<FSpatialViewState>&& State, FRHICommandListImmediate& RHICmdList)
	{
		if (!State->Texture.IsValid()) return;
		FRetiredSpatialView Item;
		Item.State = MoveTemp(State);
		Item.Fence = RHICreateGPUFence(TEXT("FogMS.SpatialRetired"));
		if (Item.Fence.IsValid()) RHICmdList.WriteGPUFence(Item.Fence);
		// A failed fence retains its resource until the explicit GPU-idle shutdown.
		RetiredViews.Add(MoveTemp(Item));
	}

	void CollectSpatialViews(uint32 CurrentKey, FRHICommandListImmediate& RHICmdList)
	{
		for (int32 Index = RetiredViews.Num() - 1; Index >= 0; --Index)
		{
			const FGPUFenceRHIRef& Fence = RetiredViews[Index].Fence;
			if (Fence.IsValid() && Fence->NumPendingWriteCommands.GetValue() == 0 && Fence->Poll())
				RetiredViews.RemoveAtSwap(Index, 1, EAllowShrinking::No);
		}
		for (auto It = SpatialViews.CreateIterator(); It; ++It)
		{
			if (It.Key() != CurrentKey && SpatialAccessSerial - It.Value()->LastUse > RetireAfterCalls)
			{
				RetireView(MoveTemp(It.Value()), RHICmdList);
				It.RemoveCurrent();
			}
		}
		if (!SpatialViews.Contains(CurrentKey) && SpatialViews.Num() >= MaxSpatialViews)
		{
			uint32 OldestKey = 0;
			uint64 OldestUse = MAX_uint64;
			for (const auto& Item : SpatialViews)
			{
				if (Item.Value->LastUse < OldestUse) { OldestKey = Item.Key; OldestUse = Item.Value->LastUse; }
			}
			RetireView(MoveTemp(SpatialViews.FindChecked(OldestKey)), RHICmdList);
			SpatialViews.Remove(OldestKey);
		}
	}

	bool SameRequest(const FFogMSSpatialRequest& A, const FFogMSSpatialRequest& B)
	{
		return A.Revision == B.Revision && A.CenterWS == B.CenterWS && A.AxisX == B.AxisX
			&& A.AxisY == B.AxisY && A.AxisZ == B.AxisZ && A.Extent == B.Extent
			&& A.RangeCm == B.RangeCm && A.PhaseG == B.PhaseG && A.Steps == B.Steps && A.Directions == B.Directions;
	}

	bool ValidRequest(const FFogMSSpatialRequest& Request)
	{
		return !Request.CenterWS.ContainsNaN() && !Request.AxisX.ContainsNaN() && !Request.AxisY.ContainsNaN()
			&& !Request.AxisZ.ContainsNaN() && !Request.Extent.ContainsNaN() && Request.Extent.GetMin() > 0.0f
			&& FMath::Abs(Request.AxisX.SizeSquared() - 1.0f) < 1.0e-3f
			&& FMath::Abs(Request.AxisY.SizeSquared() - 1.0f) < 1.0e-3f
			&& FMath::Abs(Request.AxisZ.SizeSquared() - 1.0f) < 1.0e-3f
			&& FMath::Abs(FVector3f::DotProduct(Request.AxisX, Request.AxisY)) < 1.0e-3f
			&& FMath::Abs(FVector3f::DotProduct(Request.AxisX, Request.AxisZ)) < 1.0e-3f
			&& FMath::Abs(FVector3f::DotProduct(Request.AxisY, Request.AxisZ)) < 1.0e-3f
			&& FMath::IsFinite(Request.RangeCm) && Request.RangeCm > 0.0f && Request.RangeCm <= 2000.0f
			&& FMath::IsFinite(Request.PhaseG) && FMath::Abs(Request.PhaseG) <= 1.0e-6f
			&& Request.Steps >= 1 && Request.Steps <= 32 && Request.Directions == SpatialDirectionCount;
	}
}

FFogMSSpatialResult FogMS_BuildSpatial(FRDGBuilder& GraphBuilder, const FSceneView& SceneView, const FFogMSSpatialRequest& Request)
{
	check(IsInRenderingThread());
	FFogMSSpatialResult Result;
	bLastBuildValid = false;
#if RHI_RAYTRACING
	if (!GDynamicRHI || FCString::Strcmp(GDynamicRHI->GetName(), TEXT("D3D12")) != 0
		|| GNumExplicitGPUsForRendering != 1 || !GRHISupportsInlineRayTracing || !SupportsSpatial(SceneView.GetShaderPlatform()))
	{
		Result.Error = TEXT("Spatial Preview requires single-GPU deferred D3D12/SM6, hardware inline ray tracing and -BindlessAll.");
		return Result;
	}
	if (!SceneView.Family || SceneView.Family->Views.Num() != 1 || SceneView.bIsSceneCapture
		|| SceneView.bIsReflectionCapture || SceneView.bIsPlanarReflection || !SceneView.IsPerspectiveProjection()
		|| !SceneView.Family->bRealtimeUpdate || !SceneView.IsRayTracingAllowedForView())
	{
		Result.Error = TEXT("Spatial Preview requires one real-time perspective view; scene/reflection captures and stereo are unsupported.");
		return Result;
	}
	if (!ValidRequest(Request))
	{
		Result.Error = TEXT("Spatial Preview requires a finite orthonormal Box, g=0, range (0,2000] cm, 1-32 steps and 12 directions.");
		return Result;
	}
	// PostTLASBuild in the deferred renderer passes an actual FViewInfo. No private
	// Renderer function is called unless it is exported by the UE 5.8 interface.
	const FViewInfo& View = static_cast<const FViewInfo&>(SceneView);
	if (!View.ViewState || !View.CachedViewUniformShaderParameters || !View.ViewUniformBuffer.IsValid() || !View.HasRayTracingScene())
	{
		Result.Error = TEXT("Spatial Preview is waiting for a persistent view, native view uniforms and a built TLAS.");
		return Result;
	}
	const FViewUniformShaderParameters& Uniforms = *View.CachedViewUniformShaderParameters;
	const uint32 ViewKey = View.ViewState->GetViewKey();
	FRHICommandListImmediate& RHICmdList = FRHICommandListExecutor::GetImmediateCommandList();
	++SpatialAccessSerial;
	CollectSpatialViews(ViewKey, RHICmdList);
	TUniquePtr<FSpatialViewState>& StatePtr = SpatialViews.FindOrAdd(ViewKey);
	if (!StatePtr) StatePtr = MakeUnique<FSpatialViewState>();
	FSpatialViewState& State = *StatePtr;
	State.LastUse = SpatialAccessSerial;
	const uint32 FrameIndex = View.ViewState->GetFrameIndex();
	const FVector3f PreviousGridZ = State.LastGridZ;
	const float PreviousInvGridZ = State.LastInvGridSize.Z;
	const FMatrix44f PreviousNativeClipToTranslatedWorld = State.LastNativeClipToTranslatedWorld;
	const FMatrix44f PreviousNativeTranslatedWorldToClip = State.LastNativeTranslatedWorldToClip;
	const FVector4f PreviousInvDeviceZ = State.LastInvDeviceZToWorldZ;
	const bool bPreviousConservativeDepth = State.bLastConservativeDepth;
	const bool bCompatiblePreviousFrame = State.bHaveMetadata && FrameIndex == State.LastFrameIndex + 1u
		&& SameRequest(State.LastRequest, Request) && State.LastGridZ == Uniforms.VolumetricFogGridZParams
		&& State.LastGridSize == Uniforms.VolumetricFogGridSize && State.LastInvGridSize == Uniforms.VolumetricFogInvGridSize;
	State.bHaveMetadata = true;
	State.LastFrameIndex = FrameIndex;
	State.LastRequest = Request;
	State.LastGridZ = Uniforms.VolumetricFogGridZParams;
	State.LastInvGridSize = Uniforms.VolumetricFogInvGridSize;
	State.LastGridSize = Uniforms.VolumetricFogGridSize;
	// Preserve exactly the matrices and perspective depth conversion used by the
	// previous native producer's conservative-depth early-out, not current-view data.
	State.LastNativeClipToTranslatedWorld = FMatrix44f(View.ViewMatrices.ComputeInvProjectionNoAAMatrix()
		* View.ViewMatrices.GetTranslatedViewMatrix().GetTransposed());
	State.LastNativeTranslatedWorldToClip = Uniforms.TranslatedWorldToClip;
	State.LastInvDeviceZToWorldZ = Uniforms.InvDeviceZToWorldZTransform;
	static const IConsoleVariable* const ConservativeDepth = IConsoleManager::Get().FindConsoleVariable(TEXT("r.VolumetricFog.ConservativeDepth"));
	State.bLastConservativeDepth = ConservativeDepth && ConservativeDepth->GetInt() > 0;

	static const IConsoleVariable* const TemporalReprojection = IConsoleManager::Get().FindConsoleVariable(TEXT("r.VolumetricFog.TemporalReprojection"));
	if (!bCompatiblePreviousFrame || Request.ResetHistory || View.bCameraCut || View.bPrevTransformsReset
		|| !TemporalReprojection || TemporalReprojection->GetInt() == 0 || !View.ViewState->LightScatteringHistory.IsValid()
		|| !View.Family->EngineShowFlags.Fog || !View.Family->EngineShowFlags.VolumetricFog)
	{
		Result.Error = TEXT("Spatial Preview is warming up: previous native fog history is missing, reset, discontinuous or incompatible.");
		return Result;
	}
	const float HistoryExposure = View.ViewState->LightScatteringHistoryPreExposure;
	const FIntVector PreviousSize = View.ViewState->VolumetricFogPrevResourceGridSize;
	const FVector2f PreviousScale = View.ViewState->VolumetricFogPrevViewGridRectUVToResourceUV;
	const FVector2f PreviousMax = View.ViewState->VolumetricFogPrevUVMax;
	FRHITexture* HistoryRHI = View.ViewState->LightScatteringHistory->GetRHI();
	if (!HistoryRHI || !HistoryRHI->GetDesc().IsTexture3D() || HistoryRHI->GetDesc().Extent != FIntPoint(PreviousSize.X, PreviousSize.Y)
		|| HistoryRHI->GetDesc().Depth != PreviousSize.Z || PreviousSize.GetMin() <= 1
		|| !FMath::IsFinite(HistoryExposure) || HistoryExposure <= 0.0f || !FMath::IsFinite(1.0f / HistoryExposure)
		|| PreviousGridZ.ContainsNaN() || PreviousGridZ.X <= 0.0f || PreviousGridZ.Z <= 0.0f
		|| !FMath::IsFinite(PreviousInvGridZ) || PreviousInvGridZ <= 0.0f
		|| PreviousScale.ContainsNaN() || PreviousMax.ContainsNaN() || PreviousScale.GetMin() <= 0.0f || PreviousMax.GetMin() <= 0.0f)
	{
		Result.Error = TEXT("Spatial Preview rejected invalid native history dimensions, projection parameters or pre-exposure.");
		return Result;
	}
	const FVector3f HistoryMin(0.5f / PreviousSize.X, 0.5f / PreviousSize.Y, 0.5f / PreviousSize.Z);
	const FVector3f HistoryMax(PreviousMax.X, PreviousMax.Y, 1.0f - 0.5f / PreviousSize.Z);
	// UV.xy = (NDC.xy * (0.5,-0.5) + 0.5) * PreviousScale.
	// UV.z = log2(Clip.w * Z.x + Z.y) * Z.z / ResourceSize.z.
	// Inverting these bounds gives six linear half-spaces along a homogeneous ray.
	const FVector4f ClipXYBounds(2.0f * HistoryMin.X / PreviousScale.X - 1.0f,
		2.0f * HistoryMax.X / PreviousScale.X - 1.0f,
		1.0f - 2.0f * HistoryMax.Y / PreviousScale.Y,
		1.0f - 2.0f * HistoryMin.Y / PreviousScale.Y);
	const double LogScale = static_cast<double>(PreviousGridZ.Z) * PreviousInvGridZ;
	const FVector2f ClipDepthBounds(
		static_cast<float>((FMath::Exp2(static_cast<double>(HistoryMin.Z) / LogScale) - PreviousGridZ.Y) / PreviousGridZ.X),
		static_cast<float>((FMath::Exp2(static_cast<double>(HistoryMax.Z) / LogScale) - PreviousGridZ.Y) / PreviousGridZ.X));
	const FIntPoint PreviousViewSize(FMath::RoundToInt(PreviousScale.X * PreviousSize.X),
		FMath::RoundToInt(PreviousScale.Y * PreviousSize.Y));
	if (HistoryMax.X < HistoryMin.X || HistoryMax.Y < HistoryMin.Y || HistoryMax.Z < HistoryMin.Z
		|| ClipXYBounds.ContainsNaN() || ClipDepthBounds.ContainsNaN() || ClipDepthBounds.X <= 0.0f || ClipDepthBounds.Y <= ClipDepthBounds.X
		|| PreviousViewSize.X <= 0 || PreviousViewSize.Y <= 0 || PreviousViewSize.X > PreviousSize.X || PreviousViewSize.Y > PreviousSize.Y)
	{
		Result.Error = TEXT("Spatial Preview rejected invalid previous-history frustum bounds.");
		return Result;
	}
	FRHITexture* HistoryDepthRHI = nullptr;
	if (bPreviousConservativeDepth)
	{
		if (View.ViewState->PrevLightScatteringConservativeDepthTexture.IsValid())
			HistoryDepthRHI = View.ViewState->PrevLightScatteringConservativeDepthTexture->GetRHI();
		if (!HistoryDepthRHI || !HistoryDepthRHI->GetDesc().IsTexture2D()
			|| HistoryDepthRHI->GetDesc().Extent != FIntPoint(PreviousSize.X, PreviousSize.Y)
			|| PreviousNativeClipToTranslatedWorld.ContainsNaN() || PreviousNativeTranslatedWorldToClip.ContainsNaN()
			|| PreviousInvDeviceZ.ContainsNaN() || PreviousInvDeviceZ.Z <= 0.0f)
		{
			Result.Error = TEXT("Spatial Preview is waiting for conservative depth matching the previous native fog history.");
			return Result;
		}
	}
	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(View.GetShaderPlatform());
	if (!ShaderMap || !ShaderMap->HasShader(&FFogMSSpatialCS::GetStaticType(), 0))
	{
		Result.Error = TEXT("Spatial Preview shader is unavailable; FogMSRender must load at PostConfigInit with inline SM6 shaders.");
		return Result;
	}
	if (!State.EnsureResource())
	{
		Result.Error = State.AllocationError;
		return Result;
	}
	const FVector3f CenterTranslated(Request.CenterWS + View.ViewMatrices.GetPreViewTranslation());
	const FMatrix PreviousWorldToClip = FTranslationMatrix(-View.ViewMatrices.GetPreViewTranslation())
		* View.PrevViewInfo.ViewMatrices.GetWorldToView() * View.PrevViewInfo.ViewMatrices.ComputeProjectionNoAAMatrix();
	if (CenterTranslated.ContainsNaN() || PreviousWorldToClip.ContainsNaN())
	{
		Result.Error = TEXT("Spatial Preview rejected a non-finite translated-world transform.");
		return Result;
	}
	FRDGTextureRef History = GraphBuilder.RegisterExternalTexture(View.ViewState->LightScatteringHistory, TEXT("FogMS.SpatialNativeHistory"));
	const FRDGTextureDesc OutputDesc = FRDGTextureDesc::Create2D(FIntPoint(SpatialSize, SpatialSize * SpatialSize),
		PF_A32B32G32R32F, FClearValueBinding::None, ETextureCreateFlags::ShaderResource | ETextureCreateFlags::UAV);
	FRDGTextureRef Work = GraphBuilder.CreateTexture(OutputDesc, TEXT("FogMS.SpatialIncoming"));
	FRDGTextureRef Output = RegisterExternalTexture(GraphBuilder, State.Texture, TEXT("FogMS.SpatialResident"));
	GraphBuilder.UseInternalAccessMode(Output);
	auto* Parameters = GraphBuilder.AllocParameters<FFogMSSpatialCS::FParameters>();
	Parameters->View = View.ViewUniformBuffer;
	Parameters->TLAS = View.GetRayTracingSceneLayerViewChecked(ERayTracingSceneLayer::Base);
	Parameters->SourceHistory = History;
	Parameters->SourceHistoryDepth = bPreviousConservativeDepth
		? GraphBuilder.RegisterExternalTexture(View.ViewState->PrevLightScatteringConservativeDepthTexture, TEXT("FogMS.SpatialHistoryDepth"))
		: RegisterExternalTexture(GraphBuilder, GBlackTexture->TextureRHI, TEXT("FogMS.SpatialDepthDummy"));
	Parameters->SourceHistorySampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	Parameters->PrevTranslatedWorldToClip = FMatrix44f(PreviousWorldToClip);
	Parameters->PrevNativeClipToTranslatedWorld = PreviousNativeClipToTranslatedWorld;
	Parameters->PrevNativeTranslatedWorldToClip = PreviousNativeTranslatedWorldToClip;
	Parameters->PrevInvDeviceZToWorldZ = PreviousInvDeviceZ;
	Parameters->PrevGridZAndInvSize = FVector4f(PreviousGridZ, PreviousInvGridZ);
	// Native producer reconstructs NDC with ViewGridSize, so arbitrary world points map
	// directly through the previous ViewGrid/ResourceGrid ratio. The current/previous
	// viewport-padding ratio in native temporal reprojection is not a world-space transform.
	Parameters->PrevViewGridToResourceUV = PreviousScale;
	Parameters->HistoryUVMin = HistoryMin;
	Parameters->HistoryUVMax = HistoryMax;
	Parameters->HistoryClipXYBounds = ClipXYBounds;
	Parameters->HistoryClipDepthBounds = ClipDepthBounds;
	Parameters->HistoryResourceGridSize = PreviousSize;
	Parameters->HistoryViewGridSize = PreviousViewSize;
	Parameters->UseHistoryDepth = bPreviousConservativeDepth ? 1u : 0u;
	Parameters->BoxCenterTranslated = CenterTranslated;
	Parameters->BoxAxisX = FVector4f(Request.AxisX, Request.Extent.X);
	Parameters->BoxAxisY = FVector4f(Request.AxisY, Request.Extent.Y);
	Parameters->BoxAxisZ = FVector4f(Request.AxisZ, Request.Extent.Z);
	Parameters->HistoryInvPreExposure = 1.0f / HistoryExposure;
	Parameters->RangeCm = Request.RangeCm;
	Parameters->StepCount = Request.Steps;
	Parameters->GridSize = SpatialSize;
	Parameters->OutSpatial = GraphBuilder.CreateUAV(Work);
	const TShaderMapRef<FFogMSSpatialCS> Shader(ShaderMap);
	FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("FogMS Spatial Transport %dx%d", SpatialDirectionCount, Request.Steps),
		ERDGPassFlags::Compute, Shader, Parameters, FIntVector(SpatialSize / 8, SpatialSize / 4, SpatialSize));
	AddCopyTexturePass(GraphBuilder, Work, Output);
	// Native fog reads the persistent bindless descriptor later in this graph. This
	// explicit external-access transition makes the copy visible to those hidden reads.
	GraphBuilder.UseExternalAccessMode(Output, ERHIAccess::SRVMask, ERHIPipeline::Graphics);
	Result.Texture = State.Texture;
	Result.SRV = State.SRV;
	Result.GraphTexture = Output;
	Result.DescriptorIndex = State.DescriptorIndex;
	Result.Valid = true;
	LastValidViewKey = ViewKey;
	LastValidRenderFrame = GFrameNumberRenderThread;
	bLastBuildValid = true;
#else
	Result.Error = TEXT("Spatial Preview requires a build with RHI ray tracing support.");
#endif
	return Result;
}

void FogMS_ShutdownSpatial_RenderThread(FRHICommandListImmediate& RHICmdList)
{
	check(IsInRenderingThread());
	RHICmdList.SubmitAndBlockUntilGPUIdle();
	SpatialViews.Empty();
	RetiredViews.Empty();
	SpatialAccessSerial = 0;
	bLastBuildValid = false;
}

void FogMS_DumpSpatial_RenderThread(FRHICommandListImmediate& RHICmdList, const FString& PathPrefix)
{
	check(IsInRenderingThread());
	if (PathPrefix.IsEmpty() || FPaths::IsRelative(PathPrefix)) return;
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(PathPrefix), true);
	const auto WriteFailure = [&PathPrefix](const TCHAR* Reason)
	{
		FFileHelper::SaveStringToFile(FString::Printf(TEXT("{\"success\":false,\"reason\":\"%s\"}\n"), Reason),
			*(PathPrefix + TEXT(".json")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	};
	const TUniquePtr<FSpatialViewState>* StatePtr = SpatialViews.Find(LastValidViewKey);
	if (!bLastBuildValid || GFrameNumberRenderThread - LastValidRenderFrame > 2u || !StatePtr || !(*StatePtr)->Texture.IsValid())
	{
		WriteFailure(TEXT("No current valid spatial atlas; enable Spatial Preview and wait for history warm-up."));
		return;
	}
	const FSpatialViewState& State = **StatePtr;
	TArray<FLinearColor> Pixels;
	// UE 5.8 D3D12 ReadSurfaceData supports PF_A32B32G32R32F. RCM_MinMax copies
	// the original float32 values, including HDR values; it does not normalize them.
	FReadSurfaceDataFlags ReadFlags(RCM_MinMax);
	ReadFlags.SetLinearToGamma(false);
	RHICmdList.ReadSurfaceData(State.Texture, FIntRect(0, 0, SpatialSize, SpatialSize * SpatialSize), Pixels, ReadFlags);
	RHICmdList.Transition(FRHITransitionInfo(State.Texture, ERHIAccess::Unknown, ERHIAccess::SRVMask));
	if (Pixels.Num() != SpatialSize * SpatialSize * SpatialSize)
	{
		WriteFailure(TEXT("Spatial atlas readback returned an unexpected pixel count."));
		return;
	}
	static_assert(sizeof(FLinearColor) == sizeof(float) * 4);
	TArray<uint8> Bytes;
	Bytes.SetNumUninitialized(Pixels.Num() * sizeof(FLinearColor));
	FMemory::Memcpy(Bytes.GetData(), Pixels.GetData(), Bytes.Num());
	if (!FFileHelper::SaveArrayToFile(Bytes, *(PathPrefix + TEXT(".rgba32f"))))
	{
		WriteFailure(TEXT("Failed to save spatial atlas bytes."));
		return;
	}
	const FString Metadata = FString::Printf(
		TEXT("{\"success\":true,\"format\":\"RGBA32F_LE\",\"width\":32,\"height\":1024,\"grid\":32,\"bytes\":%d,")
		TEXT("\"layout\":\"x,y+z*32\",\"rgb\":\"de-exposed incoming radiance J\",\"alpha\":\"valid sample fraction\",")
		TEXT("\"viewKey\":%u,\"viewFrame\":%u,\"renderFrame\":%u,\"revision\":%llu,\"rangeCm\":%.9g,\"steps\":%d,\"directions\":12}\n"),
		Bytes.Num(), LastValidViewKey, State.LastFrameIndex, LastValidRenderFrame,
		static_cast<unsigned long long>(State.LastRequest.Revision), State.LastRequest.RangeCm, State.LastRequest.Steps);
	FFileHelper::SaveStringToFile(Metadata, *(PathPrefix + TEXT(".json")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}
