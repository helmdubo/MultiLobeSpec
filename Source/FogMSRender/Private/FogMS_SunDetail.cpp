// W41 Sun Detail Shadow: per Box and frame, a sun-space transmittance map of the Box's fine density and its sigma-weighted mean per
// transport cell (FogMS_SunDetail.usf). Declarations and the map layout: FogMS_WorldLighting.h (FFogMSSunDetailBasis).
#include "FogMS_WorldLighting.h"

#include "DataDrivenShaderPlatformInfo.h"
#include "GlobalShader.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RHI.h"
#include "RHIStaticStates.h"
#include "SceneView.h"
#include "ShaderParameterStruct.h"
#include "ShaderPlatformConfig.h"

namespace
{
	// The transport grid (TransportGridSize in FogMS_Transport.cpp): the cell texture holds one texel per solver cell.
	constexpr int32 SunCellGrid = 32;

	// Win64 D3D12 SM6 only, like every FogMS producer. The pass itself needs neither ray tracing nor bindless: the density atlas is
	// an ordinary bound SRV (FOGMS_BOUND_DENSITY_ATLAS) and the Box rows are uniforms.
	bool SupportsSunDetail(EShaderPlatform Platform)
	{
		return Platform == SP_PCD3D_SM6 && FShaderPlatformConfig::IsValid(Platform);
	}

	class FFogMSSunDetailCS : public FGlobalShader
	{
		DECLARE_GLOBAL_SHADER(FFogMSSunDetailCS);
		SHADER_USE_PARAMETER_STRUCT(FFogMSSunDetailCS, FGlobalShader);
	public:
		// 0: the march (one thread per sun ray), 1: the cell average (one thread per transport cell).
		class FPass : SHADER_PERMUTATION_INT("FOGMS_SUNMAP_PASS", 2);
		using FPermutationDomain = TShaderPermutationDomain<FPass>;
		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
			SHADER_PARAMETER_ARRAY(FVector4f, BoxRows, [FogMSRender::BoxRowCount])
			SHADER_PARAMETER(FVector3f, SunMapOrigin)
			SHADER_PARAMETER(FVector3f, SunMapAxisU)
			SHADER_PARAMETER(FVector3f, SunMapAxisV)
			SHADER_PARAMETER(FVector3f, SunMapStep)
			SHADER_PARAMETER(float, SunMapStepLength)
			SHADER_PARAMETER(FVector4f, SunMapRowU)
			SHADER_PARAMETER(FVector4f, SunMapRowV)
			SHADER_PARAMETER(FVector4f, SunMapRowW)
			SHADER_PARAMETER(FUintVector3, SunMapSize)
			SHADER_PARAMETER(int32, SunMapDepthAxis)
			// The Box density atlas, raw RHI texture resident in SRV state (not tracked by RDG), as transport pass 0.
			SHADER_PARAMETER_TEXTURE(Texture2D<float4>, FogMSDensityAtlas)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture3D<float2>, OutSunMap)
			SHADER_PARAMETER_RDG_TEXTURE(Texture3D<float2>, SunMap)
			SHADER_PARAMETER_SAMPLER(SamplerState, SunMapSampler)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture3D<float4>, OutSunCell)
		END_SHADER_PARAMETER_STRUCT()
		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return SupportsSunDetail(Parameters.Platform);
		}
		static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& Environment)
		{
			FGlobalShader::ModifyCompilationEnvironment(Parameters, Environment);
			// The producer view of FogMS_Indirect.ush (as FogMS_Transport.cpp TransportEnvironment, without ray tracing).
			Environment.SetDefine(TEXT("FOGMS_PRODUCER"), 1);
			Environment.SetDefine(TEXT("FOGMS_ENABLED"), 1);
			Environment.SetDefine(TEXT("FOGMS_BOX_MODE"), 1);
			Environment.SetDefine(TEXT("FOGMS_DEBUG_VIEWS"), 0);
			Environment.SetDefine(TEXT("FOGMS_BOX_DATA_ROWS"), FogMSRender::BoxRowCount);
			Environment.SetDefine(TEXT("FOGMS_BOUND_DENSITY_ATLAS"), 1);
		}
	};
	IMPLEMENT_GLOBAL_SHADER(FFogMSSunDetailCS, "/Plugin/FogMS/Private/FogMS_SunDetail.usf", "SunDetailCS", SF_Compute);

	bool IsFiniteRow(const FVector4f& Row)
	{
		return FMath::IsFinite(Row.X) && FMath::IsFinite(Row.Y) && FMath::IsFinite(Row.Z) && FMath::IsFinite(Row.W);
	}
}

// Map layout (FFogMSSunDetailBasis): region R = the Box, restricted along Local Z to [BandMinZ, BandMaxZ]; centre c, half sizes h.
// l = the sun direction in Box axes. Depth axis A = argmax |l_i| / h_i: the slab of A gives every sun ray through R the shortest
// chord 2 h_A / |l_A| (each ray lies inside all three slabs, so no other parametrization with one depth range per ray is shorter).
// A ray enters on the sun-facing A face (Local[A] = Entry = c_A + sign(l_A) h_A) and moves by -l per unit distance, so over the
// chord it drifts laterally by D_B = l_B * chord (|D_B| <= 2 h_B because A maximises |l|/h). Columns: u = entry coordinate along
// B = (A+1)%3 over [c_B - h_B + min(0, D_B), c_B + h_B + max(0, D_B)] (every ray that meets R), v likewise along C = (A+2)%3.
// Forward map of a point x: t = (Entry - x_A) / l_A (its distance to the entry plane along the ray), w = t / chord,
// u = (x_B + l_B t - U0) / (U1 - U0), v = (x_C + l_C t - V0) / (V1 - V0): all affine in x (the rows).
FFogMSSunDetailBasis FogMS_MakeSunDetailBasis(const FVector3f& DirectionToSun, const FVector3f& AxisX, const FVector3f& AxisY,
	const FVector3f& AxisZ, const FVector3f& Extent, float BandMinZ, float BandMaxZ, int32 Slices)
{
	FFogMSSunDetailBasis Basis;
	if (Slices < 1 || DirectionToSun.ContainsNaN() || AxisX.ContainsNaN() || AxisY.ContainsNaN() || AxisZ.ContainsNaN()
		|| Extent.ContainsNaN() || Extent.GetMin() <= 0.0f || !FMath::IsFinite(BandMinZ) || !FMath::IsFinite(BandMaxZ))
		return Basis;
	const FVector3d Sun = FVector3d(DirectionToSun).GetSafeNormal();
	// No sun, or the sun well below the horizon (night): no map. The hybrid's sun share k is ~0 there anyway.
	if (Sun.IsNearlyZero() || Sun.Z < -0.1) return Basis;
	const FVector3d Axes[3] = { FVector3d(AxisX), FVector3d(AxisY), FVector3d(AxisZ) };
	const double L[3] = { Sun | Axes[0], Sun | Axes[1], Sun | Axes[2] };
	const double ZLo = FMath::Clamp(static_cast<double>(FMath::Min(BandMinZ, BandMaxZ)), -static_cast<double>(Extent.Z), static_cast<double>(Extent.Z));
	const double ZHi = FMath::Clamp(static_cast<double>(FMath::Max(BandMinZ, BandMaxZ)), -static_cast<double>(Extent.Z), static_cast<double>(Extent.Z));
	if (!(ZHi - ZLo > 1.0)) return Basis; // a band thinner than 1 cm holds no medium worth a map
	const double Center[3] = { 0.0, 0.0, 0.5 * (ZLo + ZHi) };
	const double Half[3] = { Extent.X, Extent.Y, 0.5 * (ZHi - ZLo) };
	int32 A = -1;
	double Best = 0.0;
	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		const double Q = FMath::Abs(L[Axis]) / Half[Axis];
		if (Q > Best) { Best = Q; A = Axis; }
	}
	if (A < 0 || FMath::Abs(L[A]) < 1.0e-4) return Basis;
	const int32 B = (A + 1) % 3, C = (A + 2) % 3;
	const double SignA = L[A] >= 0.0 ? 1.0 : -1.0;
	const double Chord = 2.0 * Half[A] / FMath::Abs(L[A]);
	const double Entry = Center[A] + SignA * Half[A];
	const double DriftB = L[B] * Chord, DriftC = L[C] * Chord;
	const double U0 = Center[B] - Half[B] + FMath::Min(0.0, DriftB), U1 = Center[B] + Half[B] + FMath::Max(0.0, DriftB);
	const double V0 = Center[C] - Half[C] + FMath::Min(0.0, DriftC), V1 = Center[C] + Half[C] + FMath::Max(0.0, DriftC);
	const double DU = U1 - U0, DV = V1 - V0;
	if (!FMath::IsFinite(Chord) || !FMath::IsFinite(DU) || !FMath::IsFinite(DV) || DU <= 0.0 || DV <= 0.0) return Basis;
	double RowU[4] = { 0, 0, 0, 0 }, RowV[4] = { 0, 0, 0, 0 }, RowW[4] = { 0, 0, 0, 0 };
	RowW[A] = -SignA / (2.0 * Half[A]);
	RowW[3] = SignA * Entry / (2.0 * Half[A]);
	RowU[B] = 1.0 / DU;
	RowU[A] = -(L[B] / L[A]) / DU;
	RowU[3] = (L[B] * Entry / L[A] - U0) / DU;
	RowV[C] = 1.0 / DV;
	RowV[A] = -(L[C] / L[A]) / DV;
	RowV[3] = (L[C] * Entry / L[A] - V0) / DV;
	double Origin[3] = { 0, 0, 0 }, AxisU[3] = { 0, 0, 0 }, AxisV[3] = { 0, 0, 0 };
	Origin[A] = Entry; Origin[B] = U0; Origin[C] = V0;
	AxisU[B] = DU;
	AxisV[C] = DV;
	const double StepLength = Chord / Slices;
	Basis.DepthAxis = A;
	Basis.RowU = FVector4f(static_cast<float>(RowU[0]), static_cast<float>(RowU[1]), static_cast<float>(RowU[2]), static_cast<float>(RowU[3]));
	Basis.RowV = FVector4f(static_cast<float>(RowV[0]), static_cast<float>(RowV[1]), static_cast<float>(RowV[2]), static_cast<float>(RowV[3]));
	Basis.RowW = FVector4f(static_cast<float>(RowW[0]), static_cast<float>(RowW[1]), static_cast<float>(RowW[2]), static_cast<float>(RowW[3]));
	Basis.Origin = FVector3f(static_cast<float>(Origin[0]), static_cast<float>(Origin[1]), static_cast<float>(Origin[2]));
	Basis.AxisU = FVector3f(static_cast<float>(AxisU[0]), static_cast<float>(AxisU[1]), static_cast<float>(AxisU[2]));
	Basis.AxisV = FVector3f(static_cast<float>(AxisV[0]), static_cast<float>(AxisV[1]), static_cast<float>(AxisV[2]));
	// Away from the sun: -l per unit distance.
	Basis.Step = FVector3f(static_cast<float>(-L[0] * StepLength), static_cast<float>(-L[1] * StepLength), static_cast<float>(-L[2] * StepLength));
	Basis.StepLength = static_cast<float>(StepLength);
	Basis.Slices = Slices;
	Basis.bValid = !Basis.RowU.ContainsNaN() && !Basis.RowV.ContainsNaN() && !Basis.RowW.ContainsNaN() && !Basis.Origin.ContainsNaN()
		&& !Basis.Step.ContainsNaN() && Basis.StepLength > 0.0f;
	return Basis;
}

bool FogMS_BuildSunDetailMap(FRDGBuilder& GraphBuilder, const FSceneView& View, const FFogMSSunDetailRequest& Request, FString& OutError)
{
	check(IsInRenderingThread());
	OutError.Reset();
	const FFogMSSunDetailBasis& Basis = Request.Basis;
	if (!Basis.bValid)
	{
		OutError = TEXT("no atmosphere sun above the horizon");
		return false;
	}
	FRHITexture* const Map = Request.MapTexture.GetReference();
	FRHITexture* const Cell = Request.CellTexture.GetReference();
	FRHITexture* const Atlas = Request.DensityAtlas.GetReference();
	if (!Map || !Cell || !Atlas)
	{
		OutError = TEXT("map, cell texture or density atlas missing");
		return false;
	}
	const FRHITextureDesc& MapDesc = Map->GetDesc();
	const FRHITextureDesc& CellDesc = Cell->GetDesc();
	if (!MapDesc.IsTexture3D() || MapDesc.Format != PF_G16R16F || !EnumHasAnyFlags(MapDesc.Flags, TexCreate_UAV)
		|| MapDesc.Extent.X < 8 || MapDesc.Extent.Y < 8 || MapDesc.Depth != Basis.Slices
		|| !CellDesc.IsTexture3D() || CellDesc.Format != PF_FloatRGBA || !EnumHasAnyFlags(CellDesc.Flags, TexCreate_UAV)
		|| CellDesc.Extent != FIntPoint(SunCellGrid, SunCellGrid) || CellDesc.Depth != SunCellGrid
		|| Atlas->GetDesc().Dimension != ETextureDimension::Texture2D)
	{
		OutError = TEXT("unexpected map / cell texture / atlas format");
		return false;
	}
	for (uint32 Row = 0; Row < FogMSRender::BoxRowCount; ++Row)
	{
		if (!IsFiniteRow(Request.BoxRows[Row]))
		{
			OutError = TEXT("non-finite Box packet");
			return false;
		}
	}
	// An active Box with authored density (the rows FogMS_GetIndirectData validates in the shader as well).
	if (Request.BoxRows[0].W < 0.5f || Request.BoxRows[7].W <= 0.0f
		|| Request.BoxRows[2].W <= 0.0f || Request.BoxRows[3].W <= 0.0f || Request.BoxRows[4].W <= 0.0f)
	{
		OutError = TEXT("no active Box density");
		return false;
	}
	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(View.GetShaderPlatform());
	if (!SupportsSunDetail(View.GetShaderPlatform()) || !ShaderMap || !View.ViewUniformBuffer.IsValid())
	{
		OutError = TEXT("shader platform or view uniform buffer unavailable");
		return false;
	}
	FRDGTextureRef MapTexture = RegisterExternalTexture(GraphBuilder, Map, TEXT("FogMS.SunDetailMap"));
	FRDGTextureRef CellTexture = RegisterExternalTexture(GraphBuilder, Cell, TEXT("FogMS.SunDetailCell"));
	GraphBuilder.UseInternalAccessMode(MapTexture);
	GraphBuilder.UseInternalAccessMode(CellTexture);

	FFogMSSunDetailCS::FParameters Common;
	FMemory::Memzero(&Common, sizeof(Common));
	Common.View = View.ViewUniformBuffer;
	for (uint32 Row = 0; Row < FogMSRender::BoxRowCount; ++Row) Common.BoxRows[Row] = Request.BoxRows[Row];
	Common.SunMapOrigin = Basis.Origin;
	Common.SunMapAxisU = Basis.AxisU;
	Common.SunMapAxisV = Basis.AxisV;
	Common.SunMapStep = Basis.Step;
	Common.SunMapStepLength = Basis.StepLength;
	Common.SunMapRowU = Basis.RowU;
	Common.SunMapRowV = Basis.RowV;
	Common.SunMapRowW = Basis.RowW;
	Common.SunMapSize = FUintVector3(static_cast<uint32>(MapDesc.Extent.X), static_cast<uint32>(MapDesc.Extent.Y), static_cast<uint32>(MapDesc.Depth));
	Common.SunMapDepthAxis = Basis.DepthAxis;
	Common.FogMSDensityAtlas = Atlas;
	Common.SunMapSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	// Graphics queue: pass 0 reads the density atlas, a raw bound texture RDG does not track (as transport pass 0).
	{
		auto* Parameters = GraphBuilder.AllocParameters<FFogMSSunDetailCS::FParameters>();
		*Parameters = Common;
		Parameters->OutSunMap = GraphBuilder.CreateUAV(MapTexture);
		FFogMSSunDetailCS::FPermutationDomain Permutation;
		Permutation.Set<FFogMSSunDetailCS::FPass>(0);
		TShaderMapRef<FFogMSSunDetailCS> Shader(ShaderMap, Permutation);
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("FogMS SunDetail march %dx%dx%d", MapDesc.Extent.X, MapDesc.Extent.Y, MapDesc.Depth),
			ERDGPassFlags::Compute, Shader, Parameters,
			FIntVector(FMath::DivideAndRoundUp(MapDesc.Extent.X, 8), FMath::DivideAndRoundUp(MapDesc.Extent.Y, 8), 1));
	}
	{
		auto* Parameters = GraphBuilder.AllocParameters<FFogMSSunDetailCS::FParameters>();
		*Parameters = Common;
		Parameters->SunMap = MapTexture;
		Parameters->OutSunCell = GraphBuilder.CreateUAV(CellTexture);
		FFogMSSunDetailCS::FPermutationDomain Permutation;
		Permutation.Set<FFogMSSunDetailCS::FPass>(1);
		TShaderMapRef<FFogMSSunDetailCS> Shader(ShaderMap, Permutation);
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("FogMS SunDetail cell average 32^3"), ERDGPassFlags::Compute, Shader, Parameters,
			FIntVector(SunCellGrid, SunCellGrid, SunCellGrid)); // one 32-thread group per cell
	}
	// The Box Volume material binding is invisible to RDG: hand both back as SRVs (the injection field's rule).
	GraphBuilder.UseExternalAccessMode(MapTexture, ERHIAccess::SRVMask, ERHIPipeline::Graphics);
	GraphBuilder.UseExternalAccessMode(CellTexture, ERHIAccess::SRVMask, ERHIPipeline::Graphics);
	return true;
}

void FogMS_ClearSunDetailMap(FRDGBuilder& GraphBuilder, const FTextureRHIRef& CellTexture)
{
	check(IsInRenderingThread());
	FRHITexture* const Cell = CellTexture.GetReference();
	if (!Cell || !EnumHasAnyFlags(Cell->GetDesc().Flags, TexCreate_UAV)) return;
	FRDGTextureRef Texture = RegisterExternalTexture(GraphBuilder, Cell, TEXT("FogMS.SunDetailCell"));
	GraphBuilder.UseInternalAccessMode(Texture);
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(Texture), FLinearColor::Transparent);
	GraphBuilder.UseExternalAccessMode(Texture, ERHIAccess::SRVMask, ERHIPipeline::Graphics);
}
