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
// to keep this change within the reviewed file set.
void FogMS_PublishTransportField(FRDGBuilder& GraphBuilder, const FViewInfo& View, FRDGTextureRef Atlas, FRDGTextureRef Field);

namespace
{
	constexpr int32 WorldSize = 32;
	constexpr int32 WorldOrderCount = 3;
	constexpr int32 MaxWorldViews = 8;
	constexpr uint64 RetireAfterCalls = 120;
	TAutoConsoleVariable<int32> CVarWorldIndirect(TEXT("r.FogMS.World.Indirect"), 1,
		TEXT("World source diagnostic: 0 direct source only, 1 sky + Lumen surface radiance. Does not change native light components."), ECVF_RenderThreadSafe);

	bool SupportsWorld(EShaderPlatform Platform)
	{
		return Platform == SP_PCD3D_SM6 && FShaderPlatformConfig::IsValid(Platform)
			&& FShaderPlatformConfig::GetBindlessConfiguration(Platform) == ERHIBindlessConfiguration::All
			&& IsRayTracingEnabledForProject(Platform) && RHISupportsRayTracing(Platform)
			&& RHISupportsInlineRayTracing(Platform) && !IsForwardShadingEnabled(Platform);
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
}

FFogMSSpatialResult FogMS_BuildWorldLighting(FRDGBuilder& GraphBuilder, const FSceneView& SceneView, const FFogMSWorldRequest& Request)
{
	check(IsInRenderingThread());
	FFogMSSpatialResult Result;
	bLastBuildValid = false;
#if RHI_RAYTRACING
	if (!GDynamicRHI || GDynamicRHI->GetInterfaceType() != ERHIInterfaceType::D3D12
		|| GNumExplicitGPUsForRendering != 1 || !GRHISupportsInlineRayTracing || !SupportsWorld(SceneView.GetShaderPlatform()))
	{
		Result.Error = TEXT("World lighting requires single-GPU deferred D3D12/SM6, inline HWRT and -BindlessAll.");
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
	FFogMSWorldCS::FParameters Common;
	FMemory::Memzero(&Common, sizeof(Common));
	if (!FogMS_GetLumenSource(GraphBuilder, View, Common.LumenSource, Result.Error)
		|| !FogMS_GetWorldSources(GraphBuilder, View, Request.CenterWS, Request.Extent, Common.LightSources, Result.Error)) return Result;
	const uint64 Key = (uint64(View.ViewState->GetViewKey()) << 1) | (Request.bTransport ? 1u : 0u);
	FRHICommandListImmediate& RHICmdList = FRHICommandListExecutor::GetImmediateCommandList();
	++WorldAccessSerial;
	CollectWorldViews(Key, RHICmdList);
	TUniquePtr<FWorldViewState>& StatePtr = WorldViews.FindOrAdd(Key);
	if (!StatePtr) StatePtr = MakeUnique<FWorldViewState>();
	FWorldViewState& State = *StatePtr;
	State.LastUse = WorldAccessSerial;
	if (!State.EnsureResource(Request.bTransport)) { Result.Error = State.AllocationError; return Result; }
	Common.View = View.ViewUniformBuffer;
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
	if (Request.bTransport)
	{
		// Warm start needs the same Box and the same diagnostic inputs; Directions/Iterations/Tolerance may differ (J is direction independent).
		const FFogMSWorldRequest& Last = State.LastRequest;
		const bool bWarmValid = Last.bTransport && !Request.ResetHistory && !State.bLastReconstructionTest
			&& Last.CenterWS == Request.CenterWS && Last.AxisX == Request.AxisX && Last.AxisY == Request.AxisY
			&& Last.AxisZ == Request.AxisZ && Last.Extent == Request.Extent
			&& State.LastTransportTest == CVarInt(TEXT("r.FogMS.Transport.Test"))
			&& State.LastTransportGeometry == CVarInt(TEXT("r.FogMS.Transport.TestGeometry"))
			&& State.LastTransportBoundary == CVarInt(TEXT("r.FogMS.Transport.TestBoundary"))
			&& State.LastTransportTau == CVarFloat(TEXT("r.FogMS.Transport.TestTau"))
			&& State.LastTransportAlbedo == CVarFloat(TEXT("r.FogMS.Transport.TestAlbedo"));
		FRDGTextureRef Previous = nullptr;
		if (!bWarmValid) State.PreviousAtlas.SafeRelease();
		else if (State.PreviousAtlas.IsValid()) Previous = GraphBuilder.RegisterExternalTexture(State.PreviousAtlas, TEXT("FogMS.Transport.PreviousAtlas"));
		Work = FogMS_RenderTransport(GraphBuilder, View, Request, Common.LumenSource, Common.LightSources, Common.IndirectEnabled != 0, Previous);
		if (!Work) { Result.Error = TEXT("B2 transport graph unavailable."); return Result; }
		GraphBuilder.QueueTextureExtraction(Work, &State.PreviousAtlas);
		if (Request.InjectionTexture.IsValid())
		{
			// Emissive Injection: slab 0 total J of cell (x,y,z) -> field texel (x,y,z), written in
			// this graph before native fog voxelization samples it through the Box Volume material.
			// That material binding is invisible to RDG: leave the field in external SRV access.
			FRDGTextureRef Field = RegisterExternalTexture(GraphBuilder, Request.InjectionTexture, TEXT("FogMS.TransportField"));
			GraphBuilder.UseInternalAccessMode(Field);
			FogMS_PublishTransportField(GraphBuilder, View, Work, Field);
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
	FRDGTextureRef Output = RegisterExternalTexture(GraphBuilder, State.Texture, TEXT("FogMS.WorldResident"));
	GraphBuilder.UseInternalAccessMode(Output);
	AddCopyTexturePass(GraphBuilder, Work, Output);
	GraphBuilder.UseExternalAccessMode(Output, ERHIAccess::SRVMask, ERHIPipeline::Graphics);
	State.LastFrameIndex = View.ViewState->GetFrameIndex();
	State.LastRequest = Request;
	State.LastRequest.InjectionTexture.SafeRelease(); // Do not extend the Box field's lifetime.
	State.LastIndirectEnabled = Common.IndirectEnabled;
	if (Request.bTransport)
	{
		State.LastTransportTest = CVarInt(TEXT("r.FogMS.Transport.Test"));
		State.LastTransportGeometry = CVarInt(TEXT("r.FogMS.Transport.TestGeometry"));
		State.LastTransportBoundary = CVarInt(TEXT("r.FogMS.Transport.TestBoundary"));
		State.bLastReconstructionTest = CVarInt(TEXT("r.FogMS.Transport.TestReconstruction")) != 0;
		State.LastTransportTau = CVarFloat(TEXT("r.FogMS.Transport.TestTau"));
		State.LastTransportAlbedo = CVarFloat(TEXT("r.FogMS.Transport.TestAlbedo"));
	}
	Result.Texture = State.Texture;
	Result.SRV = State.SRV;
	Result.GraphTexture = Output;
	Result.DescriptorIndex = State.DescriptorIndex;
	Result.GridSize = WorldSize;
	Result.Valid = true;
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
