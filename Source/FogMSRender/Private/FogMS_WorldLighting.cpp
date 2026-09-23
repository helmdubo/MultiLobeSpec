#include "FogMS_WorldLighting.h"
#include "FogMS_LumenSource.h"
#include "FogMS_WorldSources.h"
#include "FogMS_Transport.h"

#include "DynamicRHI.h"
#include "GlobalShader.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#if PLATFORM_WINDOWS
#include "ID3D12DynamicRHI.h"
#endif
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "MultiGPU.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "RenderUtils.h"
#include "RHICommandList.h"
#include "RHIStaticStates.h"
#include "SceneRendering.h"
#include "SceneViewState.h"
#include "SceneRendererInterface.h"
#include "SceneUniformBuffer.h"
#include "ShaderParameterStruct.h"
#include "ShaderPlatformConfig.h"
#if RHI_RAYTRACING
#include "RayTracing/RayTracingScene.h"
#endif

// Implemented in FogMS_Transport.cpp (transport pass 17). Declared here, not in FogMS_Transport.h,
// to keep this change within the reviewed file set. SunTransmittance non-null selects the hybrid field.
void FogMS_PublishTransportField(FRDGBuilder& GraphBuilder, const FViewInfo& View, FRDGTextureRef Atlas, FRDGTextureRef Field,
	FRDGTextureRef SunTransmittance);
bool FogMS_TransportAsync(const FRDGBuilder& GraphBuilder);

namespace
{
	constexpr int32 WorldSize = 32;
	constexpr int32 WorldOrderCount = 3;
	constexpr int32 MaxWorldViews = 8;
	constexpr uint64 RetireAfterCalls = 120;
	TAutoConsoleVariable<int32> CVarWorldIndirect(TEXT("r.FogMS.World.Indirect"), 1,
		TEXT("World source diagnostic: 0 direct source only, 1 sky + Lumen surface radiance. Does not change native light components."), ECVF_RenderThreadSafe);
	TAutoConsoleVariable<int32> CVarSolveInterval(TEXT("r.FogMS.Transport.SolveInterval"), 1,
		TEXT("Transport solves every N-th frame per view, clamped to [1,8]. Frames in between hold the last publication (resident atlas, ")
		TEXT("row 22, injection volume) without any pass. Box/settings change, history reset, gap or failure solve at once. 1: every frame."),
		ECVF_RenderThreadSafe);

	// Producer only: all inputs are bound (BoxRows, density atlas, RDG textures). Inline RT on PCD3D_SM6 needs
	// bindless at least for ray tracing (not Disabled), the engine's own inline-RT condition (D3D12Adapter).
	// BindlessAll is needed only by the overlay consumers and the resident atlas (IsResidentAtlasAvailable).
	bool SupportsWorld(EShaderPlatform Platform)
	{
		return Platform == SP_PCD3D_SM6 && FShaderPlatformConfig::IsValid(Platform)
			&& !IsBindlessDisabled(FShaderPlatformConfig::GetBindlessConfiguration(Platform))
			&& IsRayTracingEnabledForProject(Platform) && RHISupportsRayTracing(Platform)
			&& RHISupportsInlineRayTracing(Platform) && !IsForwardShadingEnabled(Platform);
	}

	// The resident atlas exists only for hidden heap readers (overlay consumers), which require BindlessAll.
	bool IsResidentAtlasAvailable(EShaderPlatform Platform)
	{
		return FShaderPlatformConfig::IsValid(Platform)
			&& IsBindlessFullyEnabled(FShaderPlatformConfig::GetBindlessConfiguration(Platform));
	}

#if RHI_RAYTRACING
	class FFogMSWorldCS : public FGlobalShader
	{
		DECLARE_GLOBAL_SHADER(FFogMSWorldCS);
		SHADER_USE_PARAMETER_STRUCT(FFogMSWorldCS, FGlobalShader);
	public:
		class FPass : SHADER_PERMUTATION_INT("FOGMS_WORLD_PASS", 4);
		using FPermutationDomain = TShaderPermutationDomain<FPass>;
		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
			SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FSceneUniformParameters, Scene)
			SHADER_PARAMETER_STRUCT_INCLUDE(FFogMSLumenSourceParameters, LumenSource)
			SHADER_PARAMETER_STRUCT_INCLUDE(FFogMSWorldSourcesParameters, LightSources)
			SHADER_PARAMETER_RDG_BUFFER_SRV(RaytracingAccelerationStructure, TLAS)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FRayTracingSceneMetadataRecord>, RayTracingSceneMetadata)
			SHADER_PARAMETER_ARRAY(FVector4f, BoxRows, [24])
			SHADER_PARAMETER(FVector3f, BoxCenterTranslated)
			SHADER_PARAMETER(FVector4f, BoxAxisX)
			SHADER_PARAMETER(FVector4f, BoxAxisY)
			SHADER_PARAMETER(FVector4f, BoxAxisZ)
			SHADER_PARAMETER(float, RangeCm)
			SHADER_PARAMETER(float, SourceTraceDistance)
			SHADER_PARAMETER(float, Strength)
			SHADER_PARAMETER(int32, StepCount)
			SHADER_PARAMETER(int32, GridSize)
			SHADER_PARAMETER(int32, OrderIndex)
			SHADER_PARAMETER(int32, IndirectEnabled)
			SHADER_PARAMETER_RDG_TEXTURE(Texture3D<float>, DensityField)
			SHADER_PARAMETER_RDG_TEXTURE(Texture3D<float4>, SourceField)
			SHADER_PARAMETER_RDG_TEXTURE(Texture3D<float4>, PreviousMS)
			SHADER_PARAMETER_RDG_TEXTURE(Texture3D<float4>, BaseIndirect)
			SHADER_PARAMETER_SAMPLER(SamplerState, FieldSampler)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture3D<float>, OutDensity)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture3D<float4>, OutSource)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture3D<float4>, OutMS)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture3D<float4>, OutBaseIndirect)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutAtlas)
			// Pass 0 samples the Box density atlas through this ordinary binding (Request.DensityAtlas, raw RHI
			// texture resident in SRV state, not tracked by RDG); no bindless descriptor.
			SHADER_PARAMETER_TEXTURE(Texture2D<float4>, FogMSDensityAtlas)
		END_SHADER_PARAMETER_STRUCT()
		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) { return SupportsWorld(Parameters.Platform); }
		static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& Environment)
		{
			FGlobalShader::ModifyCompilationEnvironment(Parameters, Environment);
			Environment.CompilerFlags.Add(CFLAG_Wave32);
			Environment.CompilerFlags.Add(CFLAG_InlineRayTracing);
			Environment.SetDefine(TEXT("FOGMS_PRODUCER"), 1);
			Environment.SetDefine(TEXT("FOGMS_ENABLED"), 1);
			Environment.SetDefine(TEXT("FOGMS_BOX_MODE"), 1);
			Environment.SetDefine(TEXT("FOGMS_DEBUG_VIEWS"), 0);
			Environment.SetDefine(TEXT("FOGMS_BOX_DATA_ROWS"), 24);
			// FogMS_Indirect.ush: density atlas from the bound FogMSDensityAtlas, not from the heap (row 7.z).
			Environment.SetDefine(TEXT("FOGMS_BOUND_DENSITY_ATLAS"), 1);
		}
	};
	IMPLEMENT_GLOBAL_SHADER(FFogMSWorldCS, "/Plugin/FogMS/Private/FogMS_WorldLighting.usf", "WorldLightingCS", SF_Compute);
#endif

	struct FWorldViewState
	{
#if PLATFORM_WINDOWS
		TRefCountPtr<ID3D12Resource> NativeTexture;
#endif
		FTextureRHIRef Texture;
		FShaderResourceViewRHIRef SRV;
		uint32 DescriptorIndex = MAX_uint32;
		FString AllocationError;
		bool bAllocationAttempted = false;
		uint32 LastFrameIndex = 0;
		uint64 LastUse = 0;
		FFogMSWorldRequest LastRequest;
		int32 LastIndirectEnabled = 1;
		int32 LastTransportTest = 0;
		int32 LastTransportGeometry = 0;
		int32 LastTransportBoundary = 0;
		bool bLastReconstructionTest = false;
		float LastTransportTau = 0;
		float LastTransportAlbedo = 1;
		// Previous transport atlas for the PCG warm start; valid only for the geometry in LastRequest.
		TRefCountPtr<IPooledRenderTarget> PreviousAtlas;
		// Late publication: Texture holds the solve of LastRequest, copied by the PrePostProcessPass of the graph
		// that solved it. Cleared by every late build, so a skipped copy is never offered to the next frame.
		bool bLateAtlasValid = false;
		// GFrameNumberRenderThread of that copy: a gap (Box off, view not rendered) is never bridged by an old solve.
		uint32 LateCopyRenderFrame = 0;
		// r.FogMS.Transport.SolveInterval hold chain. bHoldValid: the last call for this key published (same-frame solve,
		// completed late copy, or hold) at HoldRenderFrame; cleared at the start of every call and by Invalidate, so any
		// failed call, skipped late copy or gap forces a solve. HoldPhase counts holds since that solve (counter mod N).
		bool bHoldValid = false;
		uint32 HoldRenderFrame = 0;
		int32 HoldPhase = 0;
		int32 HoldSolveInterval = 1;
		bool bHoldInjection = false;
		uint32 HoldDescriptorIndex = MAX_uint32;
		int32 HoldGridSize = 0;

		bool EnsureResource(bool bTransport)
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
			Desc.Width = WorldSize;
			Desc.Height = (bTransport ? 4 : 2) * WorldSize * WorldSize;
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
				AllocationError = FString::Printf(TEXT("FogMS world resident atlas allocation failed: 0x%08x."), static_cast<uint32>(Result));
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
				AllocationError = TEXT("FogMS world atlas requires an exactly representable bindless float4 SRV.");
#else
			AllocationError = TEXT("FogMS world preview currently supports Windows D3D12 only.");
#endif
			return DescriptorIndex != MAX_uint32;
		}
	};

	struct FRetiredWorldView
	{
		TUniquePtr<FWorldViewState> State;
		FGPUFenceRHIRef Fence;
	};
	TMap<uint64, TUniquePtr<FWorldViewState>> WorldViews;
	TArray<FRetiredWorldView> RetiredViews;
	uint64 WorldAccessSerial = 0;
	uint64 LastValidViewKey = 0;
	uint32 LastValidRenderFrame = 0;
	bool bLastBuildValid = false;

	void RetireView(TUniquePtr<FWorldViewState>&& State, FRHICommandListImmediate& RHICmdList)
	{
		if (!State->Texture.IsValid()) return;
		FRetiredWorldView Item;
		Item.State = MoveTemp(State);
		Item.Fence = RHICreateGPUFence(TEXT("FogMS.WorldRetired"));
		if (Item.Fence.IsValid()) RHICmdList.WriteGPUFence(Item.Fence);
		// A failed fence retains its resource until the explicit GPU-idle shutdown.
		RetiredViews.Add(MoveTemp(Item));
	}

	void CollectWorldViews(uint64 CurrentKey, FRHICommandListImmediate& RHICmdList)
	{
		for (int32 Index = RetiredViews.Num() - 1; Index >= 0; --Index)
		{
			const FGPUFenceRHIRef& Fence = RetiredViews[Index].Fence;
			if (Fence.IsValid() && Fence->NumPendingWriteCommands.GetValue() == 0 && Fence->Poll())
				RetiredViews.RemoveAtSwap(Index, 1, EAllowShrinking::No);
		}
		for (auto It = WorldViews.CreateIterator(); It; ++It)
		{
			if (It.Key() != CurrentKey && WorldAccessSerial - It.Value()->LastUse > RetireAfterCalls)
			{
				RetireView(MoveTemp(It.Value()), RHICmdList);
				It.RemoveCurrent();
			}
		}
		if (!WorldViews.Contains(CurrentKey) && WorldViews.Num() >= MaxWorldViews)
		{
			uint64 OldestKey = 0;
			uint64 OldestUse = MAX_uint64;
			for (const auto& Item : WorldViews)
			{
				if (Item.Value->LastUse < OldestUse) { OldestKey = Item.Key; OldestUse = Item.Value->LastUse; }
			}
			RetireView(MoveTemp(WorldViews.FindChecked(OldestKey)), RHICmdList);
			WorldViews.Remove(OldestKey);
		}
	}

	bool ValidRequest(const FFogMSWorldRequest& Request)
	{
		return !Request.CenterWS.ContainsNaN() && !Request.AxisX.ContainsNaN() && !Request.AxisY.ContainsNaN()
			&& !Request.AxisZ.ContainsNaN() && !Request.Extent.ContainsNaN() && Request.Extent.GetMin() > 0.0f
			&& FMath::Abs(Request.AxisX.SizeSquared() - 1.0f) < 1.0e-3f
			&& FMath::Abs(Request.AxisY.SizeSquared() - 1.0f) < 1.0e-3f
			&& FMath::Abs(Request.AxisZ.SizeSquared() - 1.0f) < 1.0e-3f
			&& FMath::Abs(FVector3f::DotProduct(Request.AxisX, Request.AxisY)) < 1.0e-3f
			&& FMath::Abs(FVector3f::DotProduct(Request.AxisX, Request.AxisZ)) < 1.0e-3f
			&& FMath::Abs(FVector3f::DotProduct(Request.AxisY, Request.AxisZ)) < 1.0e-3f
			&& FMath::IsFinite(Request.RangeCm) && Request.RangeCm > 0.0f && (Request.bTransport || Request.RangeCm <= 2000.0f)
			&& FMath::IsFinite(Request.PhaseG) && FMath::Abs(Request.PhaseG) <= 1.0e-6f
			&& Request.Steps >= 1 && Request.Steps <= 32 && (Request.bTransport ? (Request.Directions == 6 || Request.Directions == 16 || Request.Directions == 24 || Request.Directions == 48 || Request.Directions == 96) : Request.Directions == 12)
			&& (!Request.bTransport || (Request.Iterations >= 1 && Request.Iterations <= 64 && FMath::IsFinite(Request.Tolerance)));
	}

	int32 CVarIntValue(const TCHAR* Name) { return IConsoleManager::Get().FindConsoleVariable(Name)->GetInt(); }
	float CVarFloatValue(const TCHAR* Name) { return IConsoleManager::Get().FindConsoleVariable(Name)->GetFloat(); }

	// Warm start needs the same Box and the same diagnostic inputs; Directions/Iterations/Tolerance may differ (J is direction independent).
	// Also the first gate of a SolveInterval hold: a hold never re-offers a field solved for other bounds or test inputs.
	bool WarmStartValid(const FWorldViewState& State, const FFogMSWorldRequest& Request)
	{
		const FFogMSWorldRequest& Last = State.LastRequest;
		return Last.bTransport && !Request.ResetHistory && !State.bLastReconstructionTest
			&& Last.CenterWS == Request.CenterWS && Last.AxisX == Request.AxisX && Last.AxisY == Request.AxisY
			&& Last.AxisZ == Request.AxisZ && Last.Extent == Request.Extent
			&& State.LastTransportTest == CVarIntValue(TEXT("r.FogMS.Transport.Test"))
			&& State.LastTransportGeometry == CVarIntValue(TEXT("r.FogMS.Transport.TestGeometry"))
			&& State.LastTransportBoundary == CVarIntValue(TEXT("r.FogMS.Transport.TestBoundary"))
			&& State.LastTransportTau == CVarFloatValue(TEXT("r.FogMS.Transport.TestTau"))
			&& State.LastTransportAlbedo == CVarFloatValue(TEXT("r.FogMS.Transport.TestAlbedo"));
	}

	// Second gate of a hold: everything else the published field depends on is what the last solve used. Revision
	// covers density, packet rows (strength, directions, iterations, delivery) and the sun; the rest is checked directly.
	bool HoldCompatible(const FWorldViewState& State, const FFogMSWorldRequest& Request, int32 IndirectEnabled)
	{
		const FFogMSWorldRequest& Last = State.LastRequest;
		return Last.Revision == Request.Revision && Last.Directions == Request.Directions && Last.Iterations == Request.Iterations
			&& Last.Tolerance == Request.Tolerance && Last.Strength == Request.Strength && Last.DirectionToSun == Request.DirectionToSun
			&& Last.bHybridInjection == Request.bHybridInjection && Last.bLatePublish == Request.bLatePublish
			&& State.bHoldInjection == Request.InjectionTexture.IsValid() && State.LastIndirectEnabled == IndirectEnabled
			&& CVarIntValue(TEXT("r.FogMS.Transport.TestReconstruction")) == 0;
	}

	uint64 TransportViewKey(const FSceneView& View)
	{
		// Same key as FogMS_BuildWorldLighting (FViewInfo::ViewState is FSceneView::State).
		return (uint64(View.GetViewKey()) << 1) | 1u;
	}
}

// Late Transport publication handed from PostTLASBuild to PrePostProcessPass of the SAME render graph.
// The RDG blackboard lives exactly as long as its graph, so a later graph never sees these references.
struct FFogMSLateTransportPublish
{
	uint64 Key = 0;
	FRDGTextureRef Work = nullptr;      // transient atlas written on async compute
	FRDGTextureRef FieldBack = nullptr; // transient injection field written on async compute (pass 17)
	FTextureRHIRef Field;               // Box-owned injection volume, copy destination
};
RDG_REGISTER_BLACKBOARD_STRUCT(FFogMSLateTransportPublish)

FFogMSWorldResult FogMS_BuildWorldLighting(FRDGBuilder& GraphBuilder, const FSceneView& SceneView, const FFogMSWorldRequest& Request)
{
	check(IsInRenderingThread());
	FFogMSWorldResult Result;
	bLastBuildValid = false;
	const bool bLate = Request.bTransport && Request.bLatePublish;
	// SolveInterval hold chain: read and break it before any early return, so only a call that publishes (or holds)
	// again can continue it. A hold restores bLateAtlasValid only when it was valid here (the copy it re-offers exists).
	bool bPriorHoldValid = false, bPriorLateAtlasValid = false;
	if (Request.bTransport)
	{
		if (TUniquePtr<FWorldViewState>* Found = WorldViews.Find(TransportViewKey(SceneView)))
		{
			bPriorHoldValid = (*Found)->bHoldValid;
			bPriorLateAtlasValid = (*Found)->bLateAtlasValid;
			(*Found)->bHoldValid = false;
		}
	}
	if (bLate)
	{
		// Before any early return: this graph's own PrePostProcessPass copy is the only way back to a valid atlas.
		if (TUniquePtr<FWorldViewState>* Found = WorldViews.Find(TransportViewKey(SceneView))) (*Found)->bLateAtlasValid = false;
	}
#if RHI_RAYTRACING
	if (!GDynamicRHI || GDynamicRHI->GetInterfaceType() != ERHIInterfaceType::D3D12
		|| GNumExplicitGPUsForRendering != 1 || !GRHISupportsInlineRayTracing || !SupportsWorld(SceneView.GetShaderPlatform()))
	{
		// GRHISupportsInlineRayTracing already implies bindless enabled at least for ray tracing (D3D12Adapter).
		Result.Error = TEXT("World lighting requires single-GPU deferred D3D12/SM6 with inline hardware ray tracing (bindless enabled at least for ray tracing).");
		return Result;
	}
	if (!SceneView.Family || SceneView.Family->Views.Num() != 1 || SceneView.bIsSceneCapture
		|| SceneView.bIsReflectionCapture || SceneView.bIsPlanarReflection || !SceneView.IsPerspectiveProjection()
		|| !SceneView.Family->bRealtimeUpdate || !SceneView.IsRayTracingAllowedForView())
	{
		Result.Error = TEXT("World lighting requires one real-time perspective view, without scene/reflection captures or stereo.");
		return Result;
	}
	if (!ValidRequest(Request) || !FMath::IsFinite(Request.Strength) || Request.Strength < 0 || (!Request.bTransport && Request.Strength > .5f))
	{
		Result.Error = TEXT("World lighting requires finite orthonormal bounds, g=0, range (0,2000] cm and strength [0,0.5].");
		return Result;
	}
	if (Request.InjectionTexture.IsValid())
	{
		// Fail before the solve: the Box material is the only consumer of an injection request.
		const FRHITextureDesc& FieldDesc = Request.InjectionTexture->GetDesc();
		if (!Request.bTransport || FieldDesc.Dimension != ETextureDimension::Texture3D
			|| FieldDesc.Extent != FIntPoint(WorldSize, WorldSize) || FieldDesc.Depth != WorldSize
			|| (FieldDesc.Format != PF_FloatRGBA && FieldDesc.Format != PF_A32B32G32R32F)
			|| !EnumHasAnyFlags(FieldDesc.Flags, TexCreate_UAV))
		{
			Result.Error = TEXT("Emissive injection requires Transport and a 32^3 UAV FloatRGBA/RGBA32F volume field.");
			return Result;
		}
	}
	if (Request.bHybridInjection && !Request.InjectionTexture.IsValid())
	{
		// Hybrid only changes what the injection field carries (J_ms, T_sun); without a field it has no consumer.
		Result.Error = TEXT("Hybrid injection requires Transport with an Emissive Injection volume field.");
		return Result;
	}
	// Pass 0 of both producers binds the density atlas directly (FOGMS_BOUND_DENSITY_ATLAS); a missing or
	// non-2D texture would fail shader-parameter validation, so reject it before any pass is added.
	if (!Request.DensityAtlas.IsValid() || Request.DensityAtlas->GetDesc().Dimension != ETextureDimension::Texture2D)
	{
		Result.Error = TEXT("World lighting requires the Box density atlas (2D texture); it is not uploaded yet.");
		return Result;
	}
	// Resident atlas + bindless descriptor feed only the overlay consumers (BindlessAll). Without BindlessAll the
	// solve reaches fog only through the Emissive Injection volume, so a request without one has no consumer.
	const bool bResident = IsResidentAtlasAvailable(SceneView.GetShaderPlatform());
	if (!bResident && !(Request.bTransport && Request.InjectionTexture.IsValid()))
	{
		Result.Error = TEXT("Without -BindlessAll only Transport with Emissive Injection is available (the overlay consumers of the resident atlas need -BindlessAll).");
		return Result;
	}
	static const IConsoleVariable* const Culling = IConsoleManager::Get().FindConsoleVariable(TEXT("r.RayTracing.Culling"));
	static const IConsoleVariable* const LumenAsync = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Lumen.AsyncCompute"));
	if (!Culling || Culling->GetInt() != 0 || !LumenAsync || LumenAsync->GetInt() != 0)
	{
		Result.Error = TEXT("World lighting requires r.RayTracing.Culling=0 and r.Lumen.AsyncCompute=0. Use Enable Indirect Preview.");
		return Result;
	}
	const FViewInfo& View = static_cast<const FViewInfo&>(SceneView);
	if (Request.bTransport && !View.LumenHardwareRayTracingHitDataBuffer)
	{
		Result.Error = TEXT("B2 is waiting for per-segment ray-traced shadow flags.");
		return Result;
	}
	if (!View.ViewState || !View.ViewUniformBuffer.IsValid() || !View.HasRayTracingScene()
		|| !View.GetInlineRayTracingBindingDataBuffer())
	{
		Result.Error = TEXT("World lighting is waiting for a persistent view, TLAS and inline triangle metadata.");
		return Result;
	}
	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(View.GetShaderPlatform());
	if (!ShaderMap)
	{
		Result.Error = TEXT("World lighting shader map is unavailable.");
		return Result;
	}
	const uint64 Key = (uint64(View.ViewState->GetViewKey()) << 1) | (Request.bTransport ? 1u : 0u);
	const int32 SolveInterval = Request.bTransport ? FMath::Clamp(CVarSolveInterval.GetValueOnRenderThread(), 1, 8) : 1;
	if (SolveInterval > 1)
	{
		// Hold: every gate above passed, the previous call for this key published on the previous render frame, and the
		// field in the resident atlas / injection volume was solved for these bounds and settings. Decided before the
		// source bindings so a hold adds no pass or upload; PreviousAtlas stays the last solve's (next warm start).
		TUniquePtr<FWorldViewState>* Found = WorldViews.Find(Key);
		FWorldViewState* Held = Found ? Found->Get() : nullptr;
		const int32 IndirectEnabled = CVarWorldIndirect.GetValueOnRenderThread() != 0 ? 1 : 0;
		if (Held && bPriorHoldValid && Request.bAllowHold && GFrameNumberRenderThread - Held->HoldRenderFrame <= 1u
			&& Held->HoldSolveInterval == SolveInterval && Held->HoldPhase + 1 < SolveInterval
			&& Held->Texture.IsValid() == bResident && (!bLate || !Held->Texture.IsValid() || bPriorLateAtlasValid)
			&& WarmStartValid(*Held, Request) && HoldCompatible(*Held, Request, IndirectEnabled))
		{
			Held->LastUse = ++WorldAccessSerial;
			++Held->HoldPhase;
			Held->bHoldValid = true;
			Held->HoldRenderFrame = GFrameNumberRenderThread;
			if (bLate && bPriorLateAtlasValid)
			{
				// Late gap check (FogMS_GetLateTransportField): a hold refreshes the copy it re-offers, it is not a gap.
				Held->bLateAtlasValid = true;
				Held->LateCopyRenderFrame = GFrameNumberRenderThread;
			}
			Result.Texture = Held->Texture;
			Result.SRV = Held->SRV;
			Result.DescriptorIndex = Held->HoldDescriptorIndex;
			Result.GridSize = Held->HoldGridSize;
			Result.Valid = true;
			Result.bHeld = true;
			Result.HoldPhase = Held->HoldPhase;
			Result.SolveInterval = SolveInterval;
			// FogMS.DumpSpatial reads the resident atlas, which still holds the published solve.
			LastValidViewKey = Key;
			LastValidRenderFrame = GFrameNumberRenderThread;
			bLastBuildValid = true;
			return Result;
		}
	}
	FFogMSWorldCS::FParameters Common;
	FMemory::Memzero(&Common, sizeof(Common));
	if (!FogMS_GetLumenSource(GraphBuilder, View, Common.LumenSource, Result.Error)
		|| !FogMS_GetWorldSources(GraphBuilder, View, Request.CenterWS, Request.Extent, Common.LightSources, Result.Error)) return Result;
	FRHICommandListImmediate& RHICmdList = FRHICommandListExecutor::GetImmediateCommandList();
	++WorldAccessSerial;
	CollectWorldViews(Key, RHICmdList);
	TUniquePtr<FWorldViewState>& StatePtr = WorldViews.FindOrAdd(Key);
	if (!StatePtr) StatePtr = MakeUnique<FWorldViewState>();
	FWorldViewState& State = *StatePtr;
	State.LastUse = WorldAccessSerial;
	// Injection-only (no BindlessAll): no resident atlas, no descriptor; State.Texture stays null.
	if (bResident && !State.EnsureResource(Request.bTransport)) { Result.Error = State.AllocationError; return Result; }
	Common.View = View.ViewUniformBuffer;
	Common.FogMSDensityAtlas = Request.DensityAtlas.GetReference();
	Common.Scene = GetSceneUniformBufferRef(GraphBuilder, View);
	Common.TLAS = View.GetRayTracingSceneLayerViewChecked(ERayTracingSceneLayer::Base);
	Common.RayTracingSceneMetadata = GraphBuilder.CreateSRV(View.GetInlineRayTracingBindingDataBuffer());
	for (int32 Index = 0; Index < 24; ++Index) Common.BoxRows[Index] = Request.BoxRows[Index];
	Common.BoxCenterTranslated = FVector3f(Request.CenterWS + View.ViewMatrices.GetPreViewTranslation());
	Common.BoxAxisX = FVector4f(Request.AxisX, Request.Extent.X);
	Common.BoxAxisY = FVector4f(Request.AxisY, Request.Extent.Y);
	Common.BoxAxisZ = FVector4f(Request.AxisZ, Request.Extent.Z);
	Common.IndirectEnabled = CVarWorldIndirect.GetValueOnRenderThread() != 0 ? 1 : 0;
	const auto CVarInt = [](const TCHAR* Name) { return IConsoleManager::Get().FindConsoleVariable(Name)->GetInt(); };
	const auto CVarFloat = [](const TCHAR* Name) { return IConsoleManager::Get().FindConsoleVariable(Name)->GetFloat(); };
	FRDGTextureRef Work = nullptr;
	FRDGTextureRef FieldBack = nullptr;
	if (Request.bTransport)
	{
		const bool bWarmValid = WarmStartValid(State, Request); // Same predicate as before, shared with the hold gate.
		FRDGTextureRef Previous = nullptr;
		if (!bWarmValid) State.PreviousAtlas.SafeRelease();
		else if (State.PreviousAtlas.IsValid()) Previous = GraphBuilder.RegisterExternalTexture(State.PreviousAtlas, TEXT("FogMS.Transport.PreviousAtlas"));
		// Hybrid injection: pass 2 of this graph also writes T_sun (RDG texture); pass 17 below consumes it in the
		// same graph for both deliveries, so the late path needs no extra lifetime or fence handling.
		FRDGTextureRef SunTransmittance = nullptr;
		Work = FogMS_RenderTransport(GraphBuilder, View, Request, Common.LumenSource, Common.LightSources, Common.IndirectEnabled != 0, Previous,
			Request.bHybridInjection ? &SunTransmittance : nullptr);
		if (!Work) { Result.Error = TEXT("B2 transport graph unavailable."); return Result; }
		// Fail closed: a hybrid material must never receive the full field (native single scattering would double count).
		if (Request.bHybridInjection && !SunTransmittance) { Result.Error = TEXT("Hybrid injection: sun transmittance unavailable."); return Result; }
		GraphBuilder.QueueTextureExtraction(Work, &State.PreviousAtlas);
		if (Request.InjectionTexture.IsValid() && bLate)
		{
			// Late (async queue): pass 17 writes a transient field only. Nothing on graphics reads it before the
			// PrePostProcessPass copy into the Box volume, so the Box volume keeps last frame's J through fog.
			FieldBack = GraphBuilder.CreateTexture(FRDGTextureDesc::Create3D(FIntVector(WorldSize), Request.InjectionTexture->GetDesc().Format,
				FClearValueBinding::None, ETextureCreateFlags::ShaderResource | ETextureCreateFlags::UAV), TEXT("FogMS.TransportFieldBack"));
			FogMS_PublishTransportField(GraphBuilder, View, Work, FieldBack, SunTransmittance);
		}
		else if (Request.InjectionTexture.IsValid())
		{
			// Emissive Injection: slab 0 total J of cell (x,y,z) -> field texel (x,y,z), written in
			// this graph before native fog voxelization samples it through the Box Volume material.
			// That material binding is invisible to RDG: leave the field in external SRV access.
			FRDGTextureRef Field = RegisterExternalTexture(GraphBuilder, Request.InjectionTexture, TEXT("FogMS.TransportField"));
			GraphBuilder.UseInternalAccessMode(Field);
			FogMS_PublishTransportField(GraphBuilder, View, Work, Field, SunTransmittance);
			GraphBuilder.UseExternalAccessMode(Field, ERHIAccess::SRVMask, ERHIPipeline::Graphics);
		}
	}
	else
	{
	Common.RangeCm = Request.RangeCm;
	Common.SourceTraceDistance = FMath::Max(1000.0f, View.FinalPostProcessSettings.LumenMaxTraceDistance);
	Common.Strength = Request.Strength;
	Common.StepCount = 16;
	Common.GridSize = WorldSize;
	Common.IndirectEnabled = CVarWorldIndirect.GetValueOnRenderThread() != 0 ? 1 : 0;
	Common.FieldSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	const ETextureCreateFlags Flags = ETextureCreateFlags::ShaderResource | ETextureCreateFlags::UAV;
	const FRDGTextureDesc FloatDesc = FRDGTextureDesc::Create3D(FIntVector(WorldSize), PF_R32_FLOAT, FClearValueBinding::None, Flags);
	const FRDGTextureDesc ColorDesc = FRDGTextureDesc::Create3D(FIntVector(WorldSize), PF_A32B32G32R32F, FClearValueBinding::None, Flags);
	FRDGTextureRef Density = GraphBuilder.CreateTexture(FloatDesc, TEXT("FogMS.WorldDensity"));
	FRDGTextureRef Base = GraphBuilder.CreateTexture(ColorDesc, TEXT("FogMS.WorldBaseIndirect"));
	FRDGTextureRef Source = GraphBuilder.CreateTexture(ColorDesc, TEXT("FogMS.WorldPrimarySource"));
	Common.DensityField = Density;
	Common.BaseIndirect = Base;
	const auto Dispatch = [&](int32 Pass, const TCHAR* Name, const FFogMSWorldCS::FParameters& Params)
	{
		FFogMSWorldCS::FPermutationDomain Permutation;
		Permutation.Set<FFogMSWorldCS::FPass>(Pass);
		const TShaderMapRef<FFogMSWorldCS> Shader(ShaderMap, Permutation);
		auto* Parameters = GraphBuilder.AllocParameters<FFogMSWorldCS::FParameters>();
		*Parameters = Params;
		FComputeShaderUtils::AddPass(GraphBuilder, FRDGEventName(TEXT("%s"), Name), ERDGPassFlags::Compute,
			Shader, Parameters, FIntVector(WorldSize / 4));
	};
	{
		auto Parameters = Common;
		Parameters.OutDensity = GraphBuilder.CreateUAV(Density);
		Dispatch(0, TEXT("FogMS World Density"), Parameters);
	}
	{
		auto Parameters = Common;
		Parameters.OutBaseIndirect = GraphBuilder.CreateUAV(Base);
		Parameters.OutSource = GraphBuilder.CreateUAV(Source);
		Dispatch(1, TEXT("FogMS World Primary 64 rays"), Parameters);
	}
	FRDGTextureRef MS = nullptr;
	for (int32 Order = 0; Order < WorldOrderCount; ++Order)
	{
		FRDGTextureRef Next = GraphBuilder.CreateTexture(ColorDesc, TEXT("FogMS.WorldNextOrder"));
		FRDGTextureRef Sum = GraphBuilder.CreateTexture(ColorDesc, TEXT("FogMS.WorldMS"));
		auto Parameters = Common;
		Parameters.SourceField = Source;
		// Dynamic branch still binds PreviousMS on the first order. Source is a valid
		// initialized dummy; that branch returns zero and never treats q0 as MS.
		Parameters.PreviousMS = MS ? MS : Source;
		Parameters.OrderIndex = Order;
		Parameters.OutSource = GraphBuilder.CreateUAV(Next);
		Parameters.OutMS = GraphBuilder.CreateUAV(Sum);
		Dispatch(2, TEXT("FogMS World Transport 12x16"), Parameters);
		Source = Next;
		MS = Sum;
	}
	Work = GraphBuilder.CreateTexture(FRDGTextureDesc::Create2D(FIntPoint(WorldSize, 2 * WorldSize * WorldSize),
		PF_A32B32G32R32F, FClearValueBinding::None, Flags), TEXT("FogMS.WorldPacked"));
	{
		auto Parameters = Common;
		Parameters.PreviousMS = MS;
		Parameters.OutAtlas = GraphBuilder.CreateUAV(Work);
		Dispatch(3, TEXT("FogMS World Publish"), Parameters);
	}
	}
	FRDGTextureRef Output = nullptr;
	if (bLate)
	{
		// Late: no graphics pass of this hook touches an async output. The resident atlas and the Box volume keep
		// the previous copy through lights/Lumen/fog; FogMS_PublishWorldLightingLate copies in PrePostProcessPass.
		FFogMSLateTransportPublish& Late = GraphBuilder.Blackboard.GetOrCreate<FFogMSLateTransportPublish>();
		Late.Key = Key;
		Late.Work = Work;
		Late.FieldBack = FieldBack;
		Late.Field = FieldBack ? Request.InjectionTexture : FTextureRHIRef();
	}
	else if (bResident)
	{
		// Same-frame resident copy for the overlay consumers (BindlessAll only). Injection-only has no resident
		// atlas: the injection field written above is the only output, and Result carries no descriptor.
		Output = RegisterExternalTexture(GraphBuilder, State.Texture, TEXT("FogMS.WorldResident"));
		GraphBuilder.UseInternalAccessMode(Output);
		AddCopyTexturePass(GraphBuilder, Work, Output);
		GraphBuilder.UseExternalAccessMode(Output, ERHIAccess::SRVMask, ERHIPipeline::Graphics);
	}
	State.LastFrameIndex = View.ViewState->GetFrameIndex();
	State.LastRequest = Request;
	State.LastRequest.InjectionTexture.SafeRelease(); // Do not extend the Box field's lifetime.
	State.LastRequest.DensityAtlas.SafeRelease(); // Nor the density atlas's (warm start compares bounds only).
	State.LastIndirectEnabled = Common.IndirectEnabled;
	if (Request.bTransport)
	{
		State.LastTransportTest = CVarInt(TEXT("r.FogMS.Transport.Test"));
		State.LastTransportGeometry = CVarInt(TEXT("r.FogMS.Transport.TestGeometry"));
		State.LastTransportBoundary = CVarInt(TEXT("r.FogMS.Transport.TestBoundary"));
		State.bLastReconstructionTest = CVarInt(TEXT("r.FogMS.Transport.TestReconstruction")) != 0;
		State.LastTransportTau = CVarFloat(TEXT("r.FogMS.Transport.TestTau"));
		State.LastTransportAlbedo = CVarFloat(TEXT("r.FogMS.Transport.TestAlbedo"));
		// Hold chain restarts at this solve (phase 0). A same-frame publication is complete in this graph; a late one
		// only once FogMS_PublishWorldLightingLate adds its copy, so a skipped copy can never be held.
		State.bHoldValid = !bLate;
		State.HoldRenderFrame = GFrameNumberRenderThread;
		State.HoldPhase = 0;
		State.HoldSolveInterval = SolveInterval;
		State.bHoldInjection = Request.InjectionTexture.IsValid();
		State.HoldDescriptorIndex = State.DescriptorIndex;
		State.HoldGridSize = WorldSize;
	}
	Result.Texture = State.Texture;
	Result.SRV = State.SRV;
	Result.GraphTexture = Output;
	Result.DescriptorIndex = State.DescriptorIndex;
	Result.GridSize = WorldSize;
	Result.Valid = true;
	Result.SolveInterval = SolveInterval;
	LastValidViewKey = Key;
	LastValidRenderFrame = GFrameNumberRenderThread;
	bLastBuildValid = true;
#else
	Result.Error = TEXT("World lighting requires RHI ray tracing support.");
#endif
	return Result;
}

void FogMS_InvalidateWorldLighting_RenderThread()
{
	check(IsInRenderingThread());
	bLastBuildValid = false;
	// The caller stopped publishing (Box off, mode change) and cleared row 22 / the injection field: never hold across.
	for (auto& Item : WorldViews) Item.Value->bHoldValid = false;
}

bool FogMS_TransportPublishesLate(const FRDGBuilder& GraphBuilder)
{
	return FogMS_TransportAsync(GraphBuilder);
}

bool FogMS_GetLateTransportField(const FSceneView& View, const FVector4f* BoxRows, uint32& OutDescriptorIndex, int32& OutGridSize)
{
	check(IsInRenderingThread());
	OutDescriptorIndex = MAX_uint32;
	OutGridSize = WorldSize;
	if (!BoxRows || !View.State) return false;
	// A live state of the CURRENT view key: only a build for another key can retire a state (CollectWorldViews),
	// so the descriptor stays allocated through this graph. Its atlas is Box-local: offer it only for the bounds
	// it was solved in (LastRequest; built from rows 0..4 exactly as BoxRuntime does). Otherwise fail closed.
	const TUniquePtr<FWorldViewState>* Found = WorldViews.Find(TransportViewKey(View));
	if (!Found || !(*Found)->bLateAtlasValid || !(*Found)->Texture.IsValid() || (*Found)->DescriptorIndex >= (1u << 24)
		|| GFrameNumberRenderThread - (*Found)->LateCopyRenderFrame > 1u) return false;
	const FFogMSWorldRequest& Last = (*Found)->LastRequest;
	if (!Last.bTransport || Last.CenterWS != FVector(BoxRows[0]) + FVector(BoxRows[1])
		|| Last.AxisX != FVector3f(BoxRows[2]) || Last.AxisY != FVector3f(BoxRows[3]) || Last.AxisZ != FVector3f(BoxRows[4])
		|| Last.Extent != FVector3f(BoxRows[2].W, BoxRows[3].W, BoxRows[4].W)) return false;
	OutDescriptorIndex = (*Found)->DescriptorIndex;
	return true;
}

bool FogMS_PublishWorldLightingLate(FRDGBuilder& GraphBuilder, const FSceneView& View, bool& bOutInjectionCopied)
{
	check(IsInRenderingThread());
	bOutInjectionCopied = false;
	FFogMSLateTransportPublish* Late = GraphBuilder.Blackboard.GetMutable<FFogMSLateTransportPublish>();
	if (!Late || !Late->Work || Late->Key != TransportViewKey(View)) return false;
	TUniquePtr<FWorldViewState>* Found = WorldViews.Find(Late->Key);
	const FRDGTextureRef Work = Late->Work, FieldBack = Late->FieldBack;
	const FTextureRHIRef Field = Late->Field;
	// Consume once per graph whatever happens below.
	Late->Work = Late->FieldBack = nullptr;
	Late->Field.SafeRelease();
	// Injection-only (no BindlessAll): the state has no resident atlas; only the injection field is copied.
	if (!Found || (!(*Found)->Texture.IsValid() && !(FieldBack && Field.IsValid()))) return false;
	FWorldViewState& State = **Found;
	// PrePostProcessPass, graphics queue, after ComputeVolumetricFog: these copies are the first graphics consumers
	// of the async solve, so RDG joins async->graphics here and the solver overlapped lights, Lumen and fog.
	RDG_EVENT_SCOPE(GraphBuilder, "FogMS transport publish (one frame late)");
	if (State.Texture.IsValid())
	{
		FRDGTextureRef Output = RegisterExternalTexture(GraphBuilder, State.Texture, TEXT("FogMS.WorldResident"));
		GraphBuilder.UseInternalAccessMode(Output);
		AddCopyTexturePass(GraphBuilder, Work, Output);
		GraphBuilder.UseExternalAccessMode(Output, ERHIAccess::SRVMask, ERHIPipeline::Graphics);
		// The resident pair is offered to FogMS_GetLateTransportField only when it exists.
		State.bLateAtlasValid = true;
		State.LateCopyRenderFrame = GFrameNumberRenderThread;
	}
	if (FieldBack && Field.IsValid())
	{
		// The Box Volume material binding is invisible to RDG: hand the volume back in external SRV access.
		FRDGTextureRef Target = RegisterExternalTexture(GraphBuilder, Field, TEXT("FogMS.TransportField"));
		GraphBuilder.UseInternalAccessMode(Target);
		AddCopyTexturePass(GraphBuilder, FieldBack, Target);
		GraphBuilder.UseExternalAccessMode(Target, ERHIAccess::SRVMask, ERHIPipeline::Graphics);
		bOutInjectionCopied = true;
	}
	// SolveInterval hold chain: the late publication exists only now; the next frames may hold exactly these copies.
	State.bHoldValid = true;
	State.HoldRenderFrame = GFrameNumberRenderThread;
	return true;
}

void FogMS_ShutdownWorldLighting_RenderThread(FRHICommandListImmediate& RHICmdList)
{
	check(IsInRenderingThread());
	RHICmdList.SubmitAndBlockUntilGPUIdle();
	WorldViews.Empty();
	RetiredViews.Empty();
	WorldAccessSerial = 0;
	bLastBuildValid = false;
}

bool FogMS_DumpWorldLighting_RenderThread(FRHICommandListImmediate& RHICmdList, const FString& PathPrefix)
{
	check(IsInRenderingThread());
	if (!bLastBuildValid || GFrameNumberRenderThread - LastValidRenderFrame > 2u) return false;
	if (PathPrefix.IsEmpty() || FPaths::IsRelative(PathPrefix)) return true;
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(PathPrefix), true);
	const TUniquePtr<FWorldViewState>* StatePtr = WorldViews.Find(LastValidViewKey);
	if (!StatePtr || !(*StatePtr)->Texture.IsValid()) return false;
	const FWorldViewState& State = **StatePtr;
	TArray<FLinearColor> Pixels;
	FReadSurfaceDataFlags Flags(RCM_MinMax);
	Flags.SetLinearToGamma(false);
	const int32 Layers = State.LastRequest.bTransport ? 4 : 2;
	RHICmdList.ReadSurfaceData(State.Texture, FIntRect(0, 0, WorldSize, Layers * WorldSize * WorldSize), Pixels, Flags);
	RHICmdList.Transition(FRHITransitionInfo(State.Texture, ERHIAccess::Unknown, ERHIAccess::SRVMask));
	if (Pixels.Num() != Layers * WorldSize * WorldSize * WorldSize) return false;
	TArray<uint8> Bytes;
	Bytes.SetNumUninitialized(Pixels.Num() * sizeof(FLinearColor));
	FMemory::Memcpy(Bytes.GetData(), Pixels.GetData(), Bytes.Num());
	const bool Saved = FFileHelper::SaveArrayToFile(Bytes, *(PathPrefix + TEXT(".rgba32f")));
	const FFogMSWorldRequest& Last = State.LastRequest;
	const FString AnimationMetadata = FString::Printf(
		TEXT(",\"revision\":%llu,\"animationActive\":%s,\"historyReset\":%s,\"densityPhase0\":[%.9g,%.9g,%.9g],\"densityPhase1\":[%.9g,%.9g,%.9g],\"densityPhase2\":[%.9g,%.9g,%.9g]"),
		static_cast<unsigned long long>(Last.Revision), Last.BoxRows[5].W > .5f ? TEXT("true") : TEXT("false"),
		Last.ResetHistory ? TEXT("true") : TEXT("false"),
		Last.BoxRows[13].X, Last.BoxRows[13].Y, Last.BoxRows[13].Z,
		Last.BoxRows[14].X, Last.BoxRows[14].Y, Last.BoxRows[14].Z,
		Last.BoxRows[15].X, Last.BoxRows[15].Y, Last.BoxRows[15].Z);
	if (State.LastRequest.bTransport && State.bLastReconstructionTest)
	{
		const FString Metadata = FString::Printf(
			TEXT("{\"success\":%s,\"domain\":\"transport_reconstruction\",\"format\":\"RGBA32F_LE\",\"width\":%d,\"height\":%d,\"grid\":%d,\"bytes\":%d,")
			TEXT("\"layout\":\"x,y+z*N; slabs: totalJ/residual, primaryJ/openFaces, sigma_s/sigma_t, maxReceiverRGB/valid\",\"reconstructionTest\":true,\"fluxDiagnosticsValid\":false,")
			TEXT("\"viewKey\":%u,\"renderFrame\":%u,\"dumpRenderFrame\":%u,\"sourceProducedRenderFrame\":%u,\"iterations\":%d,\"directions\":%d,\"transport_scheme\":\"%s\",")
			TEXT("\"test\":%d,\"testTau\":%.9g,\"testAlbedo\":%.9g,\"testGeometry\":%d,\"testBoundary\":%d,\"cellSizeCm\":[%.9g,%.9g,%.9g]%s}\n"),
			Saved ? TEXT("true") : TEXT("false"), WorldSize, Layers * WorldSize * WorldSize, WorldSize, Bytes.Num(),
			uint32(LastValidViewKey >> 1), LastValidRenderFrame, GFrameNumberRenderThread, LastValidRenderFrame, State.LastRequest.Iterations, State.LastRequest.Directions,
			State.LastRequest.Directions == 6 ? TEXT("formal_six") : TEXT("upwind_half_gauss"),
			State.LastTransportTest, State.LastTransportTau, State.LastTransportAlbedo, State.LastTransportGeometry, State.LastTransportBoundary,
			State.LastRequest.Extent.X * (2.0 / WorldSize), State.LastRequest.Extent.Y * (2.0 / WorldSize), State.LastRequest.Extent.Z * (2.0 / WorldSize), *AnimationMetadata);
		FFileHelper::SaveStringToFile(Metadata, *(PathPrefix + TEXT(".json")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		return true;
	}
	if (State.LastRequest.bTransport)
	{
		constexpr int32 Cells = WorldSize * WorldSize * WorldSize;
		double Incoming = 0, Outgoing = 0, Absorbed = 0, DirectSource = 0;
		float MaxResidual = 0, MinRadiance = MAX_flt;
		uint32 MissingSurfaceSamples = 0, MissingAngularSamples = 0;
		bool bFinite = true;
		for (int32 I = 0; I < Cells; ++I)
		{
			const FLinearColor J = Pixels[I], F = Pixels[3 * Cells + I];
			const uint32 Packed = uint32(FMath::Max(Pixels[Cells + I].A, 0.0f));
			const uint32 MissingMask = (Packed >> 6) & 63u;
			MissingAngularSamples += Packed >> 12;
			for (uint32 Bit = 0; Bit < 6; ++Bit) MissingSurfaceSamples += (MissingMask >> Bit) & 1u;
			bFinite = bFinite && FMath::IsFinite(J.R) && FMath::IsFinite(J.G) && FMath::IsFinite(J.B) && FMath::IsFinite(J.A)
				&& FMath::IsFinite(F.R) && FMath::IsFinite(F.G) && FMath::IsFinite(F.B) && FMath::IsFinite(F.A);
			MaxResidual = FMath::Max(MaxResidual, J.A);
			MinRadiance = FMath::Min(MinRadiance, FMath::Min3(J.R, J.G, J.B));
			Incoming += F.R; Outgoing += F.G; Absorbed += F.B; DirectSource += F.A;
		}
		const double Balance = (Outgoing + Absorbed - Incoming - DirectSource) / FMath::Max(Incoming + DirectSource, 1.e-20);
		const FString Metadata = FString::Printf(
			TEXT("{\"success\":%s,\"domain\":\"transport\",\"format\":\"RGBA32F_LE\",\"width\":%d,\"height\":%d,\"grid\":%d,\"bytes\":%d,")
			TEXT("\"layout\":\"x,y+z*N; slabs: totalJ/residual, primaryJ/openFaces, sigma_s/sigma_t, diffuse input/output/absorption/directSource\",")
			TEXT("\"viewKey\":%u,\"renderFrame\":%u,\"dumpRenderFrame\":%u,\"sourceProducedRenderFrame\":%u,\"iterations\":%d,\"directions\":%d,\"transport_scheme\":\"%s\",")
			TEXT("\"test\":%d,\"testTau\":%.9g,\"testAlbedo\":%.9g,\"testGeometry\":%d,\"testBoundary\":%d,\"cellSizeCm\":[%.9g,%.9g,%.9g],\"finite\":%s,\"minRadiance\":%.9g,\"maxRelativeCellResidual\":%.9g,")
			TEXT("\"missingSurfaceBoundarySamples\":%u,\"missingAngularBoundarySamples\":%u,\"diffuseIncoming\":%.17g,\"diffuseOutgoing\":%.17g,\"diffuseAbsorbed\":%.17g,\"directScatteringSource\":%.17g,\"relativeFluxDefect\":%.17g%s}\n"),
			Saved ? TEXT("true") : TEXT("false"), WorldSize, Layers * WorldSize * WorldSize, WorldSize, Bytes.Num(),
			uint32(LastValidViewKey >> 1), LastValidRenderFrame, GFrameNumberRenderThread, LastValidRenderFrame, State.LastRequest.Iterations, State.LastRequest.Directions,
			State.LastRequest.Directions == 6 ? TEXT("formal_six") : TEXT("upwind_half_gauss"),
			State.LastTransportTest, State.LastTransportTau, State.LastTransportAlbedo, State.LastTransportGeometry, State.LastTransportBoundary,
			State.LastRequest.Extent.X * (2.0 / WorldSize), State.LastRequest.Extent.Y * (2.0 / WorldSize), State.LastRequest.Extent.Z * (2.0 / WorldSize),
			bFinite ? TEXT("true") : TEXT("false"), MinRadiance, MaxResidual,
			MissingSurfaceSamples, MissingAngularSamples, Incoming, Outgoing, Absorbed, DirectSource, Balance, *AnimationMetadata);
		FFileHelper::SaveStringToFile(Metadata, *(PathPrefix + TEXT(".json")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		return true;
	}
	const FString Metadata = FString::Printf(
		TEXT("{\"success\":%s,\"domain\":\"world\",\"format\":\"RGBA32F_LE\",\"width\":%d,\"height\":%d,\"grid\":%d,\"bytes\":%d,")
		TEXT("\"layout\":\"x,y+z*N; lower half MSOnly; upper half BaseIndirect\",\"rgb\":\"scene-linear incident radiance\",\"alpha\":\"zero (MS); missing surface-card ray fraction (base)\",")
		TEXT("\"viewKey\":%u,\"viewFrame\":%u,\"renderFrame\":%u,\"dumpRenderFrame\":%u,\"sourceProducedRenderFrame\":%u,")
		TEXT("\"revision\":%llu,\"rangeCm\":%.9g,\"strength\":%.9g,\"steps\":16,\"directions\":12,\"orders\":3,\"indirectEnabled\":%d}\n"),
		Saved ? TEXT("true") : TEXT("false"), WorldSize, 2 * WorldSize * WorldSize, WorldSize, Bytes.Num(),
		uint32(LastValidViewKey >> 1), State.LastFrameIndex, LastValidRenderFrame, GFrameNumberRenderThread, LastValidRenderFrame,
		static_cast<unsigned long long>(State.LastRequest.Revision), State.LastRequest.RangeCm,
		State.LastRequest.Strength, State.LastIndirectEnabled);
	FFileHelper::SaveStringToFile(Metadata, *(PathPrefix + TEXT(".json")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	return true;
}
