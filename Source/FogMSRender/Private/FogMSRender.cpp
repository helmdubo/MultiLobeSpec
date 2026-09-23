#include "FogMS_ShadowCache.h"
#include "FogMS_RHICompatibility.h"

#include "DataDrivenShaderPlatformInfo.h"
#include "DynamicRHI.h"
#include "GlobalShader.h"
#include "Interfaces/IPluginManager.h"
#if PLATFORM_WINDOWS
#include "ID3D12DynamicRHI.h"
#endif
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "MultiGPU.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "PooledRenderTarget.h"
#include "RenderingThread.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "SceneView.h"
#include "ShaderCore.h"
#include "ShaderParameterStruct.h"
#include "ShaderPlatformConfig.h"

namespace
{
	constexpr int32 ShadowSizeX = 128;
	constexpr int32 ShadowSizeY = 128;
	constexpr int32 ShadowIntervals = 64;
	constexpr int32 ShadowBoundaries = ShadowIntervals + 1;
	constexpr int32 ShadowMaxFilterRadius = 64;

	bool SupportsShadowCache(EShaderPlatform Platform)
	{
		return Platform == SP_PCD3D_SM6 && FShaderPlatformConfig::IsValid(Platform)
			&& FShaderPlatformConfig::GetBindlessConfiguration(Platform) == ERHIBindlessConfiguration::All;
	}

	void FogMS_SetShadowProducerEnvironment(FShaderCompilerEnvironment& OutEnvironment)
	{
		OutEnvironment.SetDefine(TEXT("FOGMS_PRODUCER"), 1);
		OutEnvironment.SetDefine(TEXT("FOGMS_ENABLED"), 1);
		OutEnvironment.SetDefine(TEXT("FOGMS_BOX_MODE"), 1);
		OutEnvironment.SetDefine(TEXT("FOGMS_DEBUG_VIEWS"), 0);
		OutEnvironment.SetDefine(TEXT("FOGMS_BOX_DATA_ROWS"), FogMSRender::BoxRowCount);
	}

	class FFogMSShadowPrefixCS : public FGlobalShader
	{
		DECLARE_GLOBAL_SHADER(FFogMSShadowPrefixCS);
		SHADER_USE_PARAMETER_STRUCT(FFogMSShadowPrefixCS, FGlobalShader);
	public:
		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
			SHADER_PARAMETER_ARRAY(FVector4f, BoxRows, [24])
			SHADER_PARAMETER(FVector4f, LightAxisX)
			SHADER_PARAMETER(FVector4f, LightAxisY)
			SHADER_PARAMETER(FVector4f, LightAxisZ)
			SHADER_PARAMETER(FIntVector, GridSize)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, OutShadow)
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return SupportsShadowCache(Parameters.Platform);
		}
		static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
		{
			FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
			FogMS_SetShadowProducerEnvironment(OutEnvironment);
		}
	};
	IMPLEMENT_GLOBAL_SHADER(FFogMSShadowPrefixCS, "/Plugin/FogMS/Private/FogMS_ShadowCache.usf", "ShadowPrefixCS", SF_Compute);

	class FFogMSShadowFilterCS : public FGlobalShader
	{
		DECLARE_GLOBAL_SHADER(FFogMSShadowFilterCS);
		SHADER_USE_PARAMETER_STRUCT(FFogMSShadowFilterCS, FGlobalShader);
	public:
		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER(FIntVector, GridSize)
			SHADER_PARAMETER(FIntPoint, FilterAxis)
			SHADER_PARAMETER(float, FilterSigmaTexels)
			SHADER_PARAMETER(int32, FilterRadius)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, FilterInput)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, OutShadow)
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return SupportsShadowCache(Parameters.Platform);
		}
		static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
		{
			FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
			FogMS_SetShadowProducerEnvironment(OutEnvironment);
		}
	};
	IMPLEMENT_GLOBAL_SHADER(FFogMSShadowFilterCS, "/Plugin/FogMS/Private/FogMS_ShadowCache.usf", "ShadowFilterCS", SF_Compute);

	bool FogMS_IsFiniteShadowRow(const FVector4f& Row)
	{
		return FMath::IsFinite(Row.X) && FMath::IsFinite(Row.Y) && FMath::IsFinite(Row.Z) && FMath::IsFinite(Row.W);
	}

	struct FShadowCacheSignature
	{
		FVector4f DensityRows[16];
		FVector4f SunAndSigma = FVector4f(0, 0, 0, 0);
		uint64 AtlasRevision = 0;

		FShadowCacheSignature() { FMemory::Memzero(DensityRows, sizeof(DensityRows)); }
		bool Equals(const FShadowCacheSignature& Other) const
		{
			return AtlasRevision == Other.AtlasRevision
				&& FMemory::Memcmp(DensityRows, Other.DensityRows, sizeof(DensityRows)) == 0
				&& FMemory::Memcmp(&SunAndSigma, &Other.SunAndSigma, sizeof(SunAndSigma)) == 0;
		}
	};

	// Cooked density atlas (no editor TextureSource): one thread per voxel copies mip 0 of the BGRA8 Volume Texture into
	// the X by (Y + Z*SizeY) BGRA8 atlas. Registered here because global shaders need this PostConfigInit module.
	class FFogMSDensityVolumeToAtlasCS : public FGlobalShader
	{
		DECLARE_GLOBAL_SHADER(FFogMSDensityVolumeToAtlasCS);
		SHADER_USE_PARAMETER_STRUCT(FFogMSDensityVolumeToAtlasCS, FGlobalShader);
	public:
		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER(FIntVector, VolumeSize)
			SHADER_PARAMETER_RDG_TEXTURE(Texture3D<float4>, DensityVolume)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, DensityAtlas)
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
		}
	};
	IMPLEMENT_GLOBAL_SHADER(FFogMSDensityVolumeToAtlasCS, "/Plugin/FogMS/Private/FogMS_DensityAtlas.usf", "VolumeToAtlasCS", SF_Compute);
}

// Used by FogMS_DensityAtlas.cpp (MultiLobeSpec), which declares it locally; no public header in this change set.
FOGMSRENDER_API bool FogMS_CopyVolumeToDensityAtlas(FRHICommandListImmediate& RHICmdList, FRHITexture* Volume, FRHITexture* Atlas,
	FString& OutError);

class FFogMSShadowCacheState
{
public:
#if PLATFORM_WINDOWS
	TRefCountPtr<ID3D12Resource> NativeTexture;
#endif
	FTextureRHIRef Texture;
	FShaderResourceViewRHIRef SRV;
	uint32 DescriptorIndex = MAX_uint32;
	bool bAllocationAttempted = false;
	FString AllocationError;
	bool bHasCachedResult = false;
	FShadowCacheSignature Signature;
	FogMSRender::FShadowCacheResult CachedResult; // GraphTexture is always null here.
	const FRDGBuilder* LastBuildGraph = nullptr;
	uint32 LastBuildFrame = 0;

	bool EnsureResource()
	{
		check(IsInRenderingThread());
		if (bAllocationAttempted) return DescriptorIndex != MAX_uint32;
		bAllocationAttempted = true;
#if PLATFORM_WINDOWS
		ID3D12DynamicRHI* D3D12 = GetID3D12DynamicRHI();
		D3D12_HEAP_PROPERTIES Heap{};
		Heap.Type = D3D12_HEAP_TYPE_DEFAULT;
		Heap.CreationNodeMask = Heap.VisibleNodeMask = D3D12->RHIGetDeviceNodeMask(0);
		D3D12_RESOURCE_DESC Desc{};
		Desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		Desc.Width = ShadowSizeX;
		Desc.Height = ShadowSizeY * ShadowBoundaries;
		Desc.DepthOrArraySize = Desc.MipLevels = Desc.SampleDesc.Count = 1;
		Desc.Format = DXGI_FORMAT_R32_FLOAT;
		Desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		// The public external wrapper supports 2D SRV resources. RDG writes transient
		// UAVs and copies here; a native external UAV would require a different wrapper.
		// No CREATE_NOT_RESIDENT: deferred bindless readers cannot enumerate this allocation.
		const HRESULT Result = D3D12->RHIGetDevice(0)->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc,
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
			nullptr, IID_PPV_ARGS(NativeTexture.GetInitReference()));
		if (FAILED(Result))
		{
			AllocationError = FString::Printf(TEXT("FogMS shadow cache resident allocation failed: 0x%08x."), static_cast<uint32>(Result));
			return false;
		}
		Texture = D3D12->RHICreateTexture2DFromResource(PF_R32_FLOAT,
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
			AllocationError = TEXT("FogMS shadow cache could not create an exactly representable bindless float SRV.");
#else
		AllocationError = TEXT("FogMS shadow cache currently supports Windows D3D12 only.");
#endif
		return DescriptorIndex != MAX_uint32;
	}
};

FogMSRender::FShadowCacheStatePtr FogMSRender::CreateShadowCacheState()
{
	return MakeShared<FFogMSShadowCacheState, ESPMode::ThreadSafe>();
}

bool FogMSRender::BuildShadow(FRDGBuilder& GraphBuilder, const FSceneView& View,
	const FShadowCacheStatePtr& State, const FVector4f (&BoxRows)[BoxRowCount],
	FVector3f DirectionToSun, float FilterSigmaCm, FShadowCacheResult& OutResult, uint64 DensityAtlasRevision)
{
	check(IsInRenderingThread());
	OutResult = FShadowCacheResult{};
	if (!State.IsValid() || !GDynamicRHI || FCString::Strcmp(GDynamicRHI->GetName(), TEXT("D3D12")) != 0
		|| GNumExplicitGPUsForRendering != 1 || !SupportsShadowCache(View.GetShaderPlatform()) || !View.ViewUniformBuffer.IsValid())
	{
		OutResult.Error = TEXT("FogMS shadow cache requires a valid rendered view and single-GPU D3D12/SM6 with -BindlessAll.");
		return false;
	}
	if (DirectionToSun.ContainsNaN() || !FMath::IsFinite(DirectionToSun.SizeSquared()) || DirectionToSun.SizeSquared() < 1.0e-12f
		|| !FMath::IsFinite(FilterSigmaCm) || FilterSigmaCm < 0.0f || FilterSigmaCm > 1.0e8f)
	{
		OutResult.Error = TEXT("FogMS shadow cache requires a finite nonzero sun direction and finite Filter Sigma in [0,100000000] cm.");
		return false;
	}
	for (uint32 Row = 0; Row < BoxRowCount; ++Row)
	{
		if (!FogMS_IsFiniteShadowRow(BoxRows[Row]))
		{
			OutResult.Error = TEXT("FogMS shadow cache received a non-finite Box packet.");
			return false;
		}
	}
	if (BoxRows[0].W < 0.5f || BoxRows[7].W <= 0.0f || BoxRows[7].Z < 0.0f || BoxRows[7].Z >= static_cast<float>(1u << 24)
		|| BoxRows[2].W <= 0.0f || BoxRows[3].W <= 0.0f || BoxRows[4].W <= 0.0f
		|| BoxRows[10].Z < 1.0f || BoxRows[10].W < 1.0f || BoxRows[11].X < 1.0f
		|| BoxRows[10].Z > 16384.0f || BoxRows[10].W * BoxRows[11].X > 16384.0f)
	{
		OutResult.Error = TEXT("FogMS shadow cache requires one active Box with valid authored density and a ready atlas descriptor.");
		return false;
	}
	const FVector3f AxisZ = DirectionToSun.GetSafeNormal();
	FShadowCacheSignature Signature;
	for (uint32 Row = 0; Row < 16; ++Row)
	{
		if (Row < 5 || Row >= 7) Signature.DensityRows[Row] = BoxRows[Row];
	}
	// Receiver-region feather, history/scattering and other shadow controls do not change extinction.
	Signature.DensityRows[1].W = 0.0f;
	Signature.DensityRows[7].X = Signature.DensityRows[7].Y = 0.0f;
	Signature.DensityRows[13].W = Signature.DensityRows[14].W = Signature.DensityRows[15].W = 0.0f;
	Signature.SunAndSigma = FVector4f(AxisZ, FilterSigmaCm);
	Signature.AtlasRevision = DensityAtlasRevision;
	const bool bSameGraphAndFrame = State->LastBuildGraph == &GraphBuilder && State->LastBuildFrame == GFrameNumberRenderThread;
	if (State->bHasCachedResult && State->Signature.Equals(Signature) && (DensityAtlasRevision != 0 || bSameGraphAndFrame))
	{
		OutResult = State->CachedResult;
		OutResult.GraphTexture = RegisterExternalTexture(GraphBuilder, State->Texture, TEXT("FogMS.ShadowResident"));
		// Same-graph views reuse the earlier queued copy/transition. A later graph sees its
		// resident SRV in graphics render order. No previous FRDGTextureRef survives the graph.
		GraphBuilder.UseExternalAccessMode(OutResult.GraphTexture, ERHIAccess::SRVMask, ERHIPipeline::Graphics);
		return true;
	}
	const FVector3f Reference = FMath::Abs(AxisZ.Z) < 0.99f ? FVector3f(0, 0, 1) : FVector3f(0, 1, 0);
	const FVector3f AxisX = FVector3f::CrossProduct(Reference, AxisZ).GetSafeNormal();
	const FVector3f AxisY = FVector3f::CrossProduct(AxisZ, AxisX);
	const FVector3f BoxAxisX(BoxRows[2].X, BoxRows[2].Y, BoxRows[2].Z);
	const FVector3f BoxAxisY(BoxRows[3].X, BoxRows[3].Y, BoxRows[3].Z);
	const FVector3f BoxAxisZ(BoxRows[4].X, BoxRows[4].Y, BoxRows[4].Z);
	const auto ProjectExtent = [&](const FVector3f& Axis)
	{
		return FMath::Abs(FVector3f::DotProduct(Axis, BoxAxisX)) * BoxRows[2].W
			+ FMath::Abs(FVector3f::DotProduct(Axis, BoxAxisY)) * BoxRows[3].W
			+ FMath::Abs(FVector3f::DotProduct(Axis, BoxAxisZ)) * BoxRows[4].W;
	};
	const FVector3f HalfExtent(ProjectExtent(AxisX) + 3.0f * FilterSigmaCm,
		ProjectExtent(AxisY) + 3.0f * FilterSigmaCm, ProjectExtent(AxisZ));
	if (HalfExtent.ContainsNaN() || HalfExtent.GetMin() <= 0.0f
		|| !FMath::IsFinite(2.0f * HalfExtent.X) || !FMath::IsFinite(2.0f * HalfExtent.Y) || !FMath::IsFinite(2.0f * HalfExtent.Z))
	{
		OutResult.Error = TEXT("FogMS shadow cache light-space bounds are invalid.");
		return false;
	}
	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(View.GetShaderPlatform());
	if (!ShaderMap || !ShaderMap->HasShader(&FFogMSShadowPrefixCS::GetStaticType(), 0)
		|| (FilterSigmaCm > 0.0f && !ShaderMap->HasShader(&FFogMSShadowFilterCS::GetStaticType(), 0)))
	{
		OutResult.Error = TEXT("FogMS shadow cache shaders are unavailable. Load FogMSRender at PostConfigInit and compile the SM6 bindless shaders.");
		return false;
	}
	if (!State->EnsureResource())
	{
		OutResult.Error = State->AllocationError;
		return false;
	}
	const FIntVector GridSize(ShadowSizeX, ShadowSizeY, ShadowIntervals);
	const FRDGTextureDesc Desc = FRDGTextureDesc::Create2D(FIntPoint(ShadowSizeX, ShadowSizeY * ShadowBoundaries),
		PF_R32_FLOAT, FClearValueBinding::None, ETextureCreateFlags::ShaderResource | ETextureCreateFlags::UAV);
	FRDGTextureRef Raw = GraphBuilder.CreateTexture(Desc, TEXT("FogMS.ShadowPrefix"));
	FRDGTextureRef Output = RegisterExternalTexture(GraphBuilder, State->Texture, TEXT("FogMS.ShadowResident"));
	GraphBuilder.UseInternalAccessMode(Output);

	auto* Prefix = GraphBuilder.AllocParameters<FFogMSShadowPrefixCS::FParameters>();
	Prefix->View = View.ViewUniformBuffer;
	for (uint32 Row = 0; Row < BoxRowCount; ++Row) Prefix->BoxRows[Row] = BoxRows[Row];
	Prefix->LightAxisX = FVector4f(AxisX, HalfExtent.X);
	Prefix->LightAxisY = FVector4f(AxisY, HalfExtent.Y);
	Prefix->LightAxisZ = FVector4f(AxisZ, HalfExtent.Z);
	Prefix->GridSize = GridSize;
	Prefix->OutShadow = GraphBuilder.CreateUAV(Raw);
	const TShaderMapRef<FFogMSShadowPrefixCS> PrefixShader(ShaderMap);
	FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("FogMS Shadow Prefix"), ERDGPassFlags::Compute,
		PrefixShader, Prefix, FIntVector(FMath::DivideAndRoundUp(ShadowSizeX, 8), FMath::DivideAndRoundUp(ShadowSizeY, 8), 1));
	if (FilterSigmaCm > 0.0f)
	{
		FRDGTextureRef Intermediate = GraphBuilder.CreateTexture(Desc, TEXT("FogMS.ShadowFilterX"));
		const TShaderMapRef<FFogMSShadowFilterCS> FilterShader(ShaderMap);
		for (int32 Axis = 0; Axis < 2; ++Axis)
		{
			auto* Filter = GraphBuilder.AllocParameters<FFogMSShadowFilterCS::FParameters>();
			Filter->GridSize = GridSize;
			Filter->FilterAxis = Axis == 0 ? FIntPoint(1, 0) : FIntPoint(0, 1);
			Filter->FilterSigmaTexels = FilterSigmaCm / (2.0f * HalfExtent[Axis] / static_cast<float>(Axis == 0 ? ShadowSizeX : ShadowSizeY));
			Filter->FilterRadius = FMath::Clamp(FMath::CeilToInt(3.0f * Filter->FilterSigmaTexels), 1, ShadowMaxFilterRadius);
			Filter->FilterInput = Axis == 0 ? Raw : Intermediate;
			Filter->OutShadow = GraphBuilder.CreateUAV(Axis == 0 ? Intermediate : Raw);
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("FogMS Shadow Filter %s", Axis == 0 ? TEXT("X") : TEXT("Y")),
				ERDGPassFlags::Compute, FilterShader, Filter,
				FIntVector(FMath::DivideAndRoundUp(ShadowSizeX, 8), FMath::DivideAndRoundUp(ShadowSizeY, 8), ShadowBoundaries));
		}
	}
	AddCopyTexturePass(GraphBuilder, Raw, Output);
	// Hidden bindless reads in subsequent native lighting passes cannot declare RDG parameters.
	GraphBuilder.UseExternalAccessMode(Output, ERHIAccess::SRVMask, ERHIPipeline::Graphics);
	OutResult.Texture = State->Texture;
	OutResult.GraphTexture = Output;
	OutResult.DescriptorIndex = State->DescriptorIndex;
	OutResult.AxisX = AxisX;
	OutResult.AxisY = AxisY;
	OutResult.AxisZ = AxisZ;
	OutResult.HalfExtent = HalfExtent;
	OutResult.GridSize = GridSize;
	State->Signature = Signature;
	State->CachedResult = OutResult;
	State->CachedResult.GraphTexture = nullptr;
	State->LastBuildGraph = &GraphBuilder;
	State->LastBuildFrame = GFrameNumberRenderThread;
	State->bHasCachedResult = true;
	return true;
}

bool FogMS_CopyVolumeToDensityAtlas(FRHICommandListImmediate& RHICmdList, FRHITexture* Volume, FRHITexture* Atlas, FString& OutError)
{
	check(IsInRenderingThread());
	OutError.Reset();
	if (!Volume || !Atlas)
	{
		OutError = TEXT("FogMS density atlas GPU copy has no source volume or destination atlas.");
		return false;
	}
	const FRHITextureDesc& VolumeDesc = Volume->GetDesc();
	const FRHITextureDesc& AtlasDesc = Atlas->GetDesc();
	// Mip 0 of the resident RHI texture must be the full-resolution BGRA8 voxel grid, read without sRGB decoding, so the
	// UNORM8 -> float -> UNORM8 round trip reproduces the bytes and the atlas matches the CPU-source layout exactly.
	if (!VolumeDesc.IsTexture3D() || VolumeDesc.Format != PF_B8G8R8A8 || EnumHasAnyFlags(VolumeDesc.Flags, ETextureCreateFlags::SRGB)
		|| AtlasDesc.Dimension != ETextureDimension::Texture2D || AtlasDesc.Format != PF_B8G8R8A8 || !EnumHasAnyFlags(AtlasDesc.Flags, ETextureCreateFlags::UAV)
		|| AtlasDesc.Extent.X != VolumeDesc.Extent.X || AtlasDesc.Extent.Y != VolumeDesc.Extent.Y * VolumeDesc.Depth)
	{
		OutError = TEXT("FogMS density atlas GPU copy requires a linear PF_B8G8R8A8 3D source and a matching X by (Y*Z) PF_B8G8R8A8 UAV atlas.");
		return false;
	}
	if (!RHIPixelFormatHasCapabilities(PF_B8G8R8A8, EPixelFormatCapabilities::TypedUAVStore))
	{
		OutError = TEXT("FogMS density atlas GPU copy requires typed UAV stores to B8G8R8A8 on this GPU.");
		return false;
	}
	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIShaderPlatform);
	if (!ShaderMap || !ShaderMap->HasShader(&FFogMSDensityVolumeToAtlasCS::GetStaticType(), 0))
	{
		OutError = TEXT("FogMS density atlas GPU copy shader is unavailable.");
		return false;
	}
	TShaderMapRef<FFogMSDensityVolumeToAtlasCS> Shader(ShaderMap);
	FRDGBuilder GraphBuilder(RHICmdList, RDG_EVENT_NAME("FogMS.DensityAtlasGPUCopy"));
	// The Volume Texture is read-only and engine-owned (left readable by the texture/streaming code): never transitioned.
	FRDGTextureRef VolumeTexture = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(Volume, TEXT("FogMS.DensityVolume")),
		ERDGTextureFlags::SkipTracking);
	FRDGTextureRef AtlasTexture = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(Atlas, TEXT("FogMS.DensityAtlas")));
	auto* Parameters = GraphBuilder.AllocParameters<FFogMSDensityVolumeToAtlasCS::FParameters>();
	Parameters->VolumeSize = FIntVector(VolumeDesc.Extent.X, VolumeDesc.Extent.Y, VolumeDesc.Depth);
	Parameters->DensityVolume = VolumeTexture;
	Parameters->DensityAtlas = GraphBuilder.CreateUAV(AtlasTexture);
	FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("FogMS Density Volume To Atlas"), ERDGPassFlags::Compute, Shader, Parameters,
		FComputeShaderUtils::GetGroupCount(Parameters->VolumeSize, FIntVector(8, 8, 1)));
	// Same final state as the CPU upload path: producers bind the atlas as an SRV.
	GraphBuilder.SetTextureAccessFinal(AtlasTexture, ERHIAccess::SRVMask);
	GraphBuilder.Execute();
	return true;
}

class FFogMSRenderModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		RHICompatibility.Startup();
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("MultiLobeSpec"));
		checkf(Plugin.IsValid(), TEXT("FogMSRender requires the MultiLobeSpec plugin."));
		AddShaderSourceDirectoryMapping(TEXT("/Plugin/FogMS"), FPaths::Combine(Plugin->GetBaseDir(), TEXT("Shaders")));
	}

	virtual void ShutdownModule() override
	{
		RHICompatibility.Shutdown();
	}

	virtual bool SupportsDynamicReloading() override { return false; }

private:
	FFogMSRHICompatibility RHICompatibility;
};

IMPLEMENT_MODULE(FFogMSRenderModule, FogMSRender)
