#include "FogMS_ScreenScattering.h"

#include "GlobalShader.h"
#include "HAL/IConsoleManager.h"
#include "SceneTexturesConfig.h"
#include "PostProcess/PostProcessInputs.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderUtils.h"
#include "RHIStaticStates.h"
#include "SceneRendering.h"
#include "SceneRenderTargetParameters.h"
#include "ShaderParameterStruct.h"

namespace
{
	TAutoConsoleVariable<int32> CVarSSFS(TEXT("r.FogMS.SSFS"), 0,
		TEXT("Optional FogMS current-frame screen scattering. 0 off, 1 on. Does not enable native FSSS."), ECVF_RenderThreadSafe);
	TAutoConsoleVariable<float> CVarAmount(TEXT("r.FogMS.SSFS.Amount"), .5f,
		TEXT("Screen redistribution fraction [0,1], additionally weighted by current volumetric coverage."), ECVF_RenderThreadSafe);
	TAutoConsoleVariable<float> CVarRadius(TEXT("r.FogMS.SSFS.Radius"), 24.f,
		TEXT("Approximate blur radius in current render pixels [1,128]. No temporal history."), ECVF_RenderThreadSafe);

	class FFogMSScreenScatteringCS : public FGlobalShader
	{
		DECLARE_GLOBAL_SHADER(FFogMSScreenScatteringCS);
		SHADER_USE_PARAMETER_STRUCT(FFogMSScreenScatteringCS, FGlobalShader);
	public:
		class FPass : SHADER_PERMUTATION_INT("FOGMS_SSFS_PASS", 3);
		class FFiltering : SHADER_PERMUTATION_INT("FOGMS_SSFS_FOG_FILTER", 3);
		using FPermutationDomain = TShaderPermutationDomain<FPass, FFiltering>;
		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, SceneColorTexture)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, SceneDepthTexture)
			SHADER_PARAMETER_RDG_TEXTURE(Texture3D<float4>, IntegratedFogTexture)
			SHADER_PARAMETER_SAMPLER(SamplerState, LinearClampSampler)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, SourceRadianceTexture)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, SourceGuideTexture)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, BlurRadianceA)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, BlurRadianceB)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, BlurGuideA)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, BlurGuideB)
			SHADER_PARAMETER(FIntPoint, ViewRectMin)
			SHADER_PARAMETER(FIntPoint, ViewSize)
			SHADER_PARAMETER(FIntPoint, InputSize)
			SHADER_PARAMETER(FIntPoint, OutputSize)
			SHADER_PARAMETER(FIntPoint, MipASize)
			SHADER_PARAMETER(FIntPoint, MipBSize)
			SHADER_PARAMETER(float, FogStartDistance)
			SHADER_PARAMETER(float, FogUpsampleJitter)
			SHADER_PARAMETER(float, Amount)
			SHADER_PARAMETER(float, LodBlend)
			SHADER_PARAMETER(uint32, FirstDownsample)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutRadiance)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutGuide)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutComposite)
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return Parameters.Platform == SP_PCD3D_SM6 && !IsForwardShadingEnabled(Parameters.Platform);
		}
	};
	IMPLEMENT_GLOBAL_SHADER(FFogMSScreenScatteringCS,
		"/Plugin/FogMS/Private/FogMS_ScreenScatteringPost.usf", "ScreenScatteringCS", SF_Compute);

	struct FLevel
	{
		FRDGTextureRef Radiance = nullptr;
		FRDGTextureRef Guide = nullptr;
		FIntPoint Size;
	};

	FLevel CreateLevel(FRDGBuilder& GraphBuilder, const FIntPoint& Size)
	{
		FLevel Level;
		Level.Size = Size;
		Level.Radiance = GraphBuilder.CreateTexture(FRDGTextureDesc::Create2D(Size, PF_FloatRGBA,
			FClearValueBinding::None, TexCreate_ShaderResource | TexCreate_UAV), TEXT("FogMS.SSFS.Radiance"));
		Level.Guide = GraphBuilder.CreateTexture(FRDGTextureDesc::Create2D(Size, PF_A32B32G32R32F,
			FClearValueBinding::None, TexCreate_ShaderResource | TexCreate_UAV), TEXT("FogMS.SSFS.DepthRange"));
		return Level;
	}

	void AddPass(FRDGBuilder& GraphBuilder, const FViewInfo& View, int32 Pass, int32 Filter,
		FFogMSScreenScatteringCS::FParameters* Parameters, const FIntPoint& Size)
	{
		FFogMSScreenScatteringCS::FPermutationDomain Permutation;
		Permutation.Set<FFogMSScreenScatteringCS::FPass>(Pass);
		Permutation.Set<FFogMSScreenScatteringCS::FFiltering>(Filter);
		TShaderMapRef<FFogMSScreenScatteringCS> Shader(View.ShaderMap, Permutation);
		ClearUnusedGraphResources(Shader, Parameters);
		FComputeShaderUtils::AddPass(GraphBuilder,
			RDG_EVENT_NAME("FogMS SSFS %s %dx%d", Pass == 0 ? TEXT("Prepare") : (Pass == 1 ? TEXT("Pyramid") : TEXT("Composite")), Size.X, Size.Y),
			ERDGPassFlags::Compute, Shader, Parameters, FComputeShaderUtils::GetGroupCount(Size, 8));
	}
}

bool FogMS_AddScreenScattering(FRDGBuilder& GraphBuilder, const FSceneView& InView, const FPostProcessingInputs& Inputs)
{
	const float RawAmount = CVarAmount.GetValueOnRenderThread();
	const float RawRadius = CVarRadius.GetValueOnRenderThread();
	if (CVarSSFS.GetValueOnRenderThread() == 0 || !FMath::IsFinite(RawAmount) || RawAmount <= 0
		|| !FMath::IsFinite(RawRadius) || RawRadius <= 0 || !InView.Family
		|| InView.bIsSceneCapture || InView.bIsReflectionCapture || InView.bIsPlanarReflection
		|| InView.Family->Views.Num() != 1 || !InView.Family->EngineShowFlags.Fog
		|| !InView.Family->EngineShowFlags.VolumetricFog || !Inputs.SceneTextures)
	{
		return false;
	}
	const IConsoleVariable* DebugVariable = IConsoleManager::Get().FindConsoleVariable(TEXT("r.GeneralPurposeTweak"));
	const float DebugValue = DebugVariable ? DebugVariable->GetFloat() : 0.f;
	// Match the shader's positive-value round-to-nearest reserved modes 101..104.
	if (DebugValue >= 100.5f && DebugValue < 104.5f) return false;
	const IConsoleVariable* NativeScattering = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Fog.ScreenSpaceScattering"));
	const IConsoleVariable* NativeComposition = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Fog.SeparateComposition"));
	// Conservative opt-out: never stack two independent screen redistributions.
	if (NativeScattering && NativeComposition && NativeScattering->GetInt() != 0 && NativeComposition->GetInt() != 0) return false;
	const FViewInfo& View = static_cast<const FViewInfo&>(InView);
	if (View.GetShaderPlatform() != SP_PCD3D_SM6 || IsForwardShadingEnabled(View.GetShaderPlatform())) return false;
	FRDGTextureRef Fog = View.VolumetricFogResources.IntegratedLightScatteringTexture;
	FRDGTextureRef SceneColor = (*Inputs.SceneTextures)->SceneColorTexture;
	FRDGTextureRef SceneDepth = (*Inputs.SceneTextures)->SceneDepthTexture;
	if (!Fog || !SceneColor || !SceneDepth || View.ViewRect.IsEmpty()) return false;

	RDG_EVENT_SCOPE(GraphBuilder, "FogMS SSFS");
	const FIntPoint ViewSize = View.ViewRect.Size();
	const float Radius = FMath::Clamp(RawRadius, 1.f, 128.f);
	const IConsoleVariable* FilterVariable = IConsoleManager::Get().FindConsoleVariable(TEXT("r.VolumetricFog.Filtering.Quality"));
	const int32 Filter = FilterVariable ? FMath::Clamp(FilterVariable->GetInt(), 0, 2) : 0;
	const IConsoleVariable* JitterVariable = IConsoleManager::Get().FindConsoleVariable(TEXT("r.VolumetricFog.UpsampleJitterMultiplier"));
	const IConsoleVariable* GridVariable = IConsoleManager::Get().FindConsoleVariable(TEXT("r.VolumetricFog.GridPixelSize"));
	// Match the already-composited native fog sample, not a second jitter sequence.
	const float FogJitter = (JitterVariable ? JitterVariable->GetFloat() : 0.f) * (GridVariable ? GridVariable->GetInt() : 16);
	TArray<FLevel, TInlineAllocator<8>> Levels;
	Levels.Add(CreateLevel(GraphBuilder, ViewSize));
	{
		auto* P = GraphBuilder.AllocParameters<FFogMSScreenScatteringCS::FParameters>();
		P->View = View.ViewUniformBuffer;
		P->SceneColorTexture = SceneColor;
		P->SceneDepthTexture = SceneDepth;
		P->IntegratedFogTexture = Fog;
		P->LinearClampSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
		P->ViewRectMin = View.ViewRect.Min;
		P->ViewSize = ViewSize;
		P->OutputSize = ViewSize;
		P->FogStartDistance = View.VolumetricFogStartDistance;
		P->FogUpsampleJitter = FMath::IsFinite(FogJitter) ? FogJitter : 0.f;
		P->OutRadiance = GraphBuilder.CreateUAV(Levels[0].Radiance);
		P->OutGuide = GraphBuilder.CreateUAV(Levels[0].Guide);
		AddPass(GraphBuilder, View, 0, Filter, P, ViewSize);
	}
	const int32 WantedLevel = FMath::Clamp(FMath::CeilToInt(FMath::Log2(Radius)), 0, 7);
	while (Levels.Num() <= WantedLevel && Levels.Num() < 8)
	{
		const FLevel Previous = Levels.Last();
		if (Previous.Size.X == 1 && Previous.Size.Y == 1) break;
		const FIntPoint Size(FMath::Max(1, (Previous.Size.X + 1) / 2), FMath::Max(1, (Previous.Size.Y + 1) / 2));
		Levels.Add(CreateLevel(GraphBuilder, Size));
		auto* P = GraphBuilder.AllocParameters<FFogMSScreenScatteringCS::FParameters>();
		P->View = View.ViewUniformBuffer;
		P->FirstDownsample = Levels.Num() == 2 ? 1u : 0u;
		P->SourceRadianceTexture = Previous.Radiance;
		P->SourceGuideTexture = Previous.Guide;
		P->InputSize = Previous.Size;
		P->OutputSize = Size;
		P->OutRadiance = GraphBuilder.CreateUAV(Levels.Last().Radiance);
		P->OutGuide = GraphBuilder.CreateUAV(Levels.Last().Guide);
		AddPass(GraphBuilder, View, 1, 0, P, Size);
	}
	const float Lod = FMath::Clamp(FMath::Log2(Radius), 0.f, float(Levels.Num() - 1));
	const int32 A = FMath::FloorToInt(Lod);
	const int32 B = FMath::Min(A + 1, Levels.Num() - 1);
	// Same format as scene color: identity pixels survive without an FP16 roundtrip.
	FRDGTextureRef Output = GraphBuilder.CreateTexture(FRDGTextureDesc::Create2D(ViewSize, SceneColor->Desc.Format,
		FClearValueBinding::None, TexCreate_ShaderResource | TexCreate_UAV), TEXT("FogMS.SSFS.Composite"));
	{
		auto* P = GraphBuilder.AllocParameters<FFogMSScreenScatteringCS::FParameters>();
		P->SceneColorTexture = SceneColor;
		P->SourceRadianceTexture = Levels[0].Radiance;
		P->SourceGuideTexture = Levels[0].Guide;
		P->BlurRadianceA = Levels[A].Radiance;
		P->BlurGuideA = Levels[A].Guide;
		P->BlurRadianceB = Levels[B].Radiance;
		P->BlurGuideB = Levels[B].Guide;
		P->MipASize = Levels[A].Size;
		P->MipBSize = Levels[B].Size;
		P->ViewRectMin = View.ViewRect.Min;
		P->ViewSize = ViewSize;
		P->OutputSize = ViewSize;
		P->Amount = FMath::Clamp(RawAmount, 0.f, 1.f);
		P->LodBlend = Lod - float(A);
		P->OutComposite = GraphBuilder.CreateUAV(Output);
		AddPass(GraphBuilder, View, 2, 0, P, ViewSize);
	}
	FRHICopyTextureInfo Copy;
	Copy.DestPosition = FIntVector(View.ViewRect.Min.X, View.ViewRect.Min.Y, 0);
	Copy.Size = FIntVector(ViewSize.X, ViewSize.Y, 1);
	AddCopyTexturePass(GraphBuilder, Output, SceneColor, Copy);
	return true;
}
