#include "FogMS_Transport.h"
#include "FogMS_LumenSource.h"
#include "FogMS_WorldSources.h"
#include "FogMS_WorldLighting.h"
#include "GlobalShader.h"
#include "HAL/IConsoleManager.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderUtils.h"
#include "SceneRendering.h"
#include "SceneUniformBuffer.h"
#include "ShaderParameterStruct.h"
#include "ShaderPlatformConfig.h"
#include "SystemTextures.h"
#if RHI_RAYTRACING
#include "RayTracing/RayTracingScene.h"
#endif

namespace
{
    constexpr int32 TransportGridSize = 32;
    constexpr int32 Cells = TransportGridSize * TransportGridSize * TransportGridSize;
    TAutoConsoleVariable<int32> SkipConverged(TEXT("r.FogMS.Transport.SkipConverged"), 1,
        TEXT("B3: skip terminal matrix sweeps after all RGB channels reach the existing PCG threshold. 0 runs the complete comparison path."), ECVF_RenderThreadSafe);
    TAutoConsoleVariable<int32> TestMode(TEXT("r.FogMS.Transport.Test"), 0,
        TEXT("Diagnostic inputs: 0 scene; 1 uniform sigma; 2 two slabs separated by vacuum. Reset to 0 after testing."), ECVF_RenderThreadSafe);
    TAutoConsoleVariable<float> TestTau(TEXT("r.FogMS.Transport.TestTau"), 4,
        TEXT("Diagnostic optical thickness across full Box X (before optional vacuum gap)."), ECVF_RenderThreadSafe);
    TAutoConsoleVariable<float> TestAlbedo(TEXT("r.FogMS.Transport.TestAlbedo"), 1,
        TEXT("Diagnostic scalar scattering albedo."), ECVF_RenderThreadSafe);
    TAutoConsoleVariable<int32> TestGeometry(TEXT("r.FogMS.Transport.TestGeometry"), 0,
        TEXT("Diagnostic inputs only: 1 retains real RT face barriers, treated as black. Scene mode always uses RT."), ECVF_RenderThreadSafe);
    TAutoConsoleVariable<int32> TestBoundary(TEXT("r.FogMS.Transport.TestBoundary"), 0,
        TEXT("Diagnostic boundary: 0 unit radiance at all Box faces; 1 only negative X face."), ECVF_RenderThreadSafe);
    TAutoConsoleVariable<int32> TestReconstruction(TEXT("r.FogMS.Transport.TestReconstruction"), 0,
        TEXT("Diagnostic readback: replace flux slab with max RGB of the native receiver sampler at eight subcell corners. No flux diagnostics in this mode. Reset to 0."), ECVF_RenderThreadSafe);
    TAutoConsoleVariable<int32> CVarWarmStart(TEXT("r.FogMS.Transport.WarmStart"), 1,
        TEXT("1 continues the PCG solution from the previous frame's atlas (same Box); 0 restarts from zero every frame."), ECVF_RenderThreadSafe);
    TAutoConsoleVariable<float> CVarTolerance(TEXT("r.FogMS.Transport.Tolerance"), 1.e-14f,
        TEXT("Relative rho threshold (against the cold-start rho) below which remaining PCG matrix passes are skipped."), ECVF_RenderThreadSafe);

#if RHI_RAYTRACING
    BEGIN_SHADER_PARAMETER_STRUCT(FTransportParameters, )
        SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
        SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FSceneUniformParameters, Scene)
        SHADER_PARAMETER_STRUCT_INCLUDE(FFogMSLumenSourceParameters, LumenSource)
        SHADER_PARAMETER_STRUCT_INCLUDE(FFogMSWorldSourcesParameters, LightSources)
        SHADER_PARAMETER_RDG_BUFFER_SRV(RaytracingAccelerationStructure, TLAS)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FRayTracingSceneMetadataRecord>, RayTracingSceneMetadata)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, ShadowHitData)
        SHADER_PARAMETER_ARRAY(FVector4f, BoxRows, [24])
        SHADER_PARAMETER(FVector3f, BoxCenterTranslated)
        SHADER_PARAMETER(FVector4f, BoxAxisX)
        SHADER_PARAMETER(FVector4f, BoxAxisY)
        SHADER_PARAMETER(FVector4f, BoxAxisZ)
        SHADER_PARAMETER(FVector3f, CellSize)
        SHADER_PARAMETER(float, SourceTraceDistance)
        SHADER_PARAMETER(int32, GridSize)
        SHADER_PARAMETER(int32, AngularCount)
        SHADER_PARAMETER(int32, SkipConverged)
        SHADER_PARAMETER(float, ConvergenceTolerance)
        SHADER_PARAMETER_ARRAY(FVector4f, Ordinates, [96])
        SHADER_PARAMETER(int32, IndirectEnabled)
        SHADER_PARAMETER(int32, SweepKind)
        SHADER_PARAMETER(int32, GatherKind)
        SHADER_PARAMETER(int32, ReductionKind)
        SHADER_PARAMETER(int32, TestMode)
        SHADER_PARAMETER(int32, TestGeometry)
        SHADER_PARAMETER(int32, TestBoundary)
        SHADER_PARAMETER(float, TestTau)
        SHADER_PARAMETER(float, TestAlbedo)
        SHADER_PARAMETER_RDG_TEXTURE(Texture3D<float4>, Coefficients)
        SHADER_PARAMETER_RDG_TEXTURE(Texture3D<float4>, DirectField)
        SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, ReconstructionAtlas)
        SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, PreviousAtlas)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, Faces)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, DirectionBoundaries)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, Angular)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, Flux)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, InputA)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, InputB)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, InputC)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, Primary)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, Partial)
        SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture3D<float4>, OutCoefficients)
        SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture3D<float4>, OutDirect)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, OutFaces)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, OutDirectionBoundaries)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, OutAngular)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, OutFlux)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, OutA)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, OutB)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, OutC)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, OutPrimary)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, OutPartial)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, Scalars)
        SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutAtlas)
    END_SHADER_PARAMETER_STRUCT()

    bool SupportsTransport(EShaderPlatform Platform)
    {
        return Platform == SP_PCD3D_SM6 && FShaderPlatformConfig::IsValid(Platform)
            && FShaderPlatformConfig::GetBindlessConfiguration(Platform) == ERHIBindlessConfiguration::All
            && IsRayTracingEnabledForProject(Platform) && RHISupportsInlineRayTracing(Platform);
    }
    void TransportEnvironment(FShaderCompilerEnvironment& Environment)
    {
        Environment.CompilerFlags.Add(CFLAG_Wave32);
        Environment.CompilerFlags.Add(CFLAG_InlineRayTracing);
        Environment.SetDefine(TEXT("FOGMS_PRODUCER"), 1);
        Environment.SetDefine(TEXT("FOGMS_ENABLED"), 1);
        Environment.SetDefine(TEXT("FOGMS_BOX_MODE"), 1);
        Environment.SetDefine(TEXT("FOGMS_DEBUG_VIEWS"), 0);
        Environment.SetDefine(TEXT("FOGMS_BOX_DATA_ROWS"), 24);
        Environment.SetDefine(TEXT("FOGMS_ANGULAR_SWEEP"), 0);
    }
    class FTransportCS : public FGlobalShader
    {
        DECLARE_GLOBAL_SHADER(FTransportCS);
        SHADER_USE_PARAMETER_STRUCT(FTransportCS, FGlobalShader);
    public:
        using FParameters = FTransportParameters;
        class FPass : SHADER_PERMUTATION_INT("FOGMS_TRANSPORT_PASS", 17);
        using FPermutationDomain = TShaderPermutationDomain<FPass>;
        static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& P)
        {
            const int32 Pass = FPermutationDomain(P.PermutationId).Get<FPass>();
            return SupportsTransport(P.Platform) && Pass != 5 && Pass != 6;
        }
        static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& P, FShaderCompilerEnvironment& E)
        { FGlobalShader::ModifyCompilationEnvironment(P, E); TransportEnvironment(E); }
    };
    class FAngularSweepCS : public FGlobalShader
    {
        DECLARE_GLOBAL_SHADER(FAngularSweepCS);
        SHADER_USE_PARAMETER_STRUCT(FAngularSweepCS, FGlobalShader);
    public:
        using FParameters = FTransportParameters;
        class FKind : SHADER_PERMUTATION_INT("FOGMS_ANGULAR_KIND", 3);
        using FPermutationDomain = TShaderPermutationDomain<FKind>;
        static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& P) { return SupportsTransport(P.Platform); }
        static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& P, FShaderCompilerEnvironment& E)
        {
            FGlobalShader::ModifyCompilationEnvironment(P, E); TransportEnvironment(E);
            const int32 Kind = FPermutationDomain(P.PermutationId).Get<FKind>();
            E.SetDefine(TEXT("FOGMS_TRANSPORT_PASS"), Kind == 0 ? 3 : (Kind == 1 ? 11 : 12));
            E.SetDefine(TEXT("FOGMS_ANGULAR_SWEEP"), 1);
        }
    };
    class FTransportReduceCS : public FGlobalShader
    {
        DECLARE_GLOBAL_SHADER(FTransportReduceCS);
        SHADER_USE_PARAMETER_STRUCT(FTransportReduceCS, FGlobalShader);
    public:
        using FParameters = FTransportParameters;
        class FFinal : SHADER_PERMUTATION_BOOL("FOGMS_REDUCE_FINAL");
        using FPermutationDomain = TShaderPermutationDomain<FFinal>;
        static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& P) { return SupportsTransport(P.Platform); }
        static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& P, FShaderCompilerEnvironment& E)
        {
            FGlobalShader::ModifyCompilationEnvironment(P, E); TransportEnvironment(E);
            E.SetDefine(TEXT("FOGMS_TRANSPORT_PASS"), FPermutationDomain(P.PermutationId).Get<FFinal>() ? 6 : 5);
        }
    };
    IMPLEMENT_GLOBAL_SHADER(FTransportCS, "/Plugin/FogMS/Private/FogMS_Transport.usf", "TransportCS", SF_Compute);
    IMPLEMENT_GLOBAL_SHADER(FTransportReduceCS, "/Plugin/FogMS/Private/FogMS_Transport.usf", "ReduceCS", SF_Compute);
    IMPLEMENT_GLOBAL_SHADER(FAngularSweepCS, "/Plugin/FogMS/Private/FogMS_Transport.usf", "AngularSweepCS", SF_Compute);
#endif
}

FRDGTextureRef FogMS_RenderTransport(FRDGBuilder& GraphBuilder, const FViewInfo& View,
    const FFogMSWorldRequest& Request, const FFogMSLumenSourceParameters& Lumen,
    const FFogMSWorldSourcesParameters& Lights, bool bIndirect, FRDGTextureRef PreviousAtlas)
{
#if RHI_RAYTRACING
    if (!View.LumenHardwareRayTracingHitDataBuffer) return nullptr;
    const bool bWarm = PreviousAtlas != nullptr && CVarWarmStart.GetValueOnRenderThread() != 0;
    FTransportParameters Common;
    FMemory::Memzero(&Common, sizeof(Common));
    Common.PreviousAtlas = PreviousAtlas ? PreviousAtlas : GSystemTextures.GetBlackDummy(GraphBuilder);
    Common.View = View.ViewUniformBuffer;
    Common.Scene = GetSceneUniformBufferRef(GraphBuilder, View);
    Common.LumenSource = Lumen; Common.LightSources = Lights;
    // Fixed 1cm numerical regularizer, independent of Box/grid size. Light radiance
    // is averaged at eight subcell points; changing the grid no longer changes b.
    Common.LightSources.FogMSWorldDistanceBiasSqr = 1.0f;
    Common.TLAS = View.GetRayTracingSceneLayerViewChecked(ERayTracingSceneLayer::Base);
    Common.RayTracingSceneMetadata = GraphBuilder.CreateSRV(View.GetInlineRayTracingBindingDataBuffer());
    Common.ShadowHitData = GraphBuilder.CreateSRV(View.LumenHardwareRayTracingHitDataBuffer);
    for (int32 I = 0; I < 24; ++I) Common.BoxRows[I] = Request.BoxRows[I];
    Common.BoxCenterTranslated = FVector3f(Request.CenterWS + View.ViewMatrices.GetPreViewTranslation());
    Common.BoxAxisX = FVector4f(Request.AxisX, Request.Extent.X);
    Common.BoxAxisY = FVector4f(Request.AxisY, Request.Extent.Y);
    Common.BoxAxisZ = FVector4f(Request.AxisZ, Request.Extent.Z);
    Common.CellSize = Request.Extent * (2.0f / TransportGridSize);
    Common.SourceTraceDistance = FMath::Max(1000.0f, View.FinalPostProcessSettings.LumenMaxTraceDistance);
    Common.GridSize = TransportGridSize; Common.IndirectEnabled = bIndirect ? 1 : 0;
    Common.AngularCount = Request.Directions;
    Common.SkipConverged = SkipConverged.GetValueOnRenderThread() != 0;
    Common.ConvergenceTolerance = FMath::Clamp(CVarTolerance.GetValueOnRenderThread(), 0.0f, 1.0f);
    if (Common.AngularCount != 6)
    {
        // Half-range Gauss-Legendre polar rule, midpoint periodic azimuth.
        // Positive paired weights sum to one; no brightness renormalization.
        const int32 Polar = Common.AngularCount == 96 ? 3 : 2;
        const int32 Azimuth = Common.AngularCount / (2 * Polar); // 16->4, 24->6, 48->12, 96->16
        const double Root = FMath::Sqrt(Polar == 3 ? 3.0 / 5.0 : 1.0 / 3.0);
        for (int32 Hemisphere = 0; Hemisphere < 2; ++Hemisphere)
        for (int32 P = 0; P < Polar; ++P)
        for (int32 A = 0; A < Azimuth; ++A)
        {
            const double Mu = Polar == 2 ? (1.0 + (P == 0 ? -Root : Root)) * .5
                : (P == 1 ? .5 : (1.0 + (P == 0 ? -Root : Root)) * .5);
            const double Weight = Polar == 2 ? .5 : (P == 1 ? 4.0 / 9.0 : 5.0 / 18.0);
            const double Phi = 2.0 * UE_DOUBLE_PI * (A + .5) / Azimuth;
            const double Radius = FMath::Sqrt(1.0 - Mu * Mu);
            Common.Ordinates[Hemisphere * Polar * Azimuth + P * Azimuth + A] = FVector4f(
                float(Radius * FMath::Cos(Phi)), float(Radius * FMath::Sin(Phi)),
                float(Hemisphere == 0 ? -Mu : Mu), float(Weight / (2 * Azimuth)));
        }
    }
    Common.TestMode = FMath::Clamp(TestMode.GetValueOnRenderThread(), 0, 2);
    Common.TestGeometry = TestGeometry.GetValueOnRenderThread() != 0;
    Common.TestBoundary = TestBoundary.GetValueOnRenderThread() != 0;
    Common.TestTau = FMath::Clamp(TestTau.GetValueOnRenderThread(), 0.0f, 128.0f);
    Common.TestAlbedo = FMath::Clamp(TestAlbedo.GetValueOnRenderThread(), -1.0f, 1.0f);
    FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(View.GetShaderPlatform());
    const auto Buffer = [&](const TCHAR* Name, int32 Size = Cells)
    { return GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector4f), Size), Name); };
    const auto Dispatch = [&](int32 Pass, const TCHAR* Name, const FTransportParameters& Params, int32 Threads)
    {
        FTransportCS::FPermutationDomain Permutation; Permutation.Set<FTransportCS::FPass>(Pass);
        TShaderMapRef<FTransportCS> Shader(ShaderMap, Permutation);
        auto* P = GraphBuilder.AllocParameters<FTransportParameters>(); *P = Params;
        FComputeShaderUtils::AddPass(GraphBuilder, FRDGEventName(TEXT("%s"), Name), ERDGPassFlags::Compute,
            Shader, P, FIntVector(FMath::DivideAndRoundUp(Threads, 64), 1, 1));
    };
    FRDGBufferRef State = Buffer(TEXT("FogMS.B2.PCGScalars"), 4);
    Common.Scalars = GraphBuilder.CreateUAV(State);
    const auto Reduce = [&](FRDGBufferRef A, FRDGBufferRef B, int32 Kind)
    {
        FRDGBufferRef Part = Buffer(TEXT("FogMS.B2.Partial"), FMath::DivideAndRoundUp(Cells, 256));
        auto P = Common; P.InputA = GraphBuilder.CreateSRV(A); P.InputB = GraphBuilder.CreateSRV(B);
        P.OutPartial = GraphBuilder.CreateUAV(Part); P.ReductionKind = Kind;
        for (int32 Final = 0; Final < 2; ++Final)
        {
            FTransportReduceCS::FPermutationDomain Permutation; Permutation.Set<FTransportReduceCS::FFinal>(Final != 0);
            TShaderMapRef<FTransportReduceCS> Shader(ShaderMap, Permutation);
            auto* Parameters = GraphBuilder.AllocParameters<FTransportParameters>(); *Parameters = P;
            if (Final) Parameters->Partial = GraphBuilder.CreateSRV(Part);
            FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("FogMS B2 PCG reduce"), ERDGPassFlags::Compute,
                Shader, Parameters, FIntVector(Final ? 1 : FMath::DivideAndRoundUp(Cells, 256), 1, 1));
        }
    };
    const auto Flags = ETextureCreateFlags::ShaderResource | ETextureCreateFlags::UAV;
    auto Desc = FRDGTextureDesc::Create3D(FIntVector(TransportGridSize), PF_A32B32G32R32F, FClearValueBinding::None, Flags);
    FRDGTextureRef Coefficients = GraphBuilder.CreateTexture(Desc, TEXT("FogMS.B2.Coefficients"));
    FRDGTextureRef Direct = GraphBuilder.CreateTexture(Desc, TEXT("FogMS.B2.Direct"));
    FRDGBufferRef Faces = Buffer(TEXT("FogMS.B2.Faces"), 6 * (TransportGridSize + 1) * TransportGridSize * TransportGridSize);
    Common.Coefficients = Coefficients; Common.DirectField = Direct; Common.Faces = GraphBuilder.CreateSRV(Faces);
    // AngularCount is a runtime uniform, so the shared gather shader retains this
    // SRV binding even for B2. That branch never indexes it; provide a valid resource.
    Common.DirectionBoundaries = Common.Faces;
    { auto P = Common; P.OutCoefficients = GraphBuilder.CreateUAV(Coefficients); Dispatch(0, TEXT("FogMS B2 density average"), P, Cells); }
    { auto P = Common; P.OutFaces = GraphBuilder.CreateUAV(Faces); Dispatch(1, TEXT("FogMS B2 reciprocal geometry faces"), P, 3 * (TransportGridSize + 1) * TransportGridSize * TransportGridSize); }
    if (Common.AngularCount > 6)
    {
        const int32 Count = Common.AngularCount * 3 * TransportGridSize * TransportGridSize;
        FRDGBufferRef Boundaries = Buffer(TEXT("FogMS.B3.AngularBoundaries"), Count);
        Common.DirectionBoundaries = GraphBuilder.CreateSRV(Boundaries);
        auto P = Common; P.OutDirectionBoundaries = GraphBuilder.CreateUAV(Boundaries);
        Dispatch(14, TEXT("FogMS B3 directional boundary radiance"), P, Count);
    }
    { auto P = Common; P.OutDirect = GraphBuilder.CreateUAV(Direct); Dispatch(2, TEXT("FogMS B2 direct cell average"), P, Cells); }
    FRDGBufferRef U = Buffer(TEXT("FogMS.B2.U")), R = Buffer(TEXT("FogMS.B2.R")), Pcg = Buffer(TEXT("FogMS.B2.P"));
    FRDGBufferRef Primary = Buffer(TEXT("FogMS.B2.Primary"));
    // One scratch buffer reused by ordered RDG passes; angular quality does not
    // retain a separate large buffer for every PCG iteration.
    FRDGBufferRef AngularScratch = Buffer(TEXT("FogMS.Transport.Angular"), Common.AngularCount * Cells);
    const auto Sweep = [&](FRDGBufferRef Input, int32 Kind, FRDGBufferRef Flux = nullptr)
    {
        FRDGBufferRef Angular = AngularScratch;
        auto P = Common; P.SweepKind = Kind; P.OutAngular = GraphBuilder.CreateUAV(Angular);
        if (Input) P.InputA = GraphBuilder.CreateSRV(Input);
        if (Flux) P.OutFlux = GraphBuilder.CreateUAV(Flux);
        if (Common.AngularCount == 6)
            Dispatch(Kind == 0 ? 3 : (Kind == 1 ? 11 : 12), TEXT("FogMS B2 six full-line sweeps"), P, 6 * TransportGridSize * TransportGridSize);
        else
        {
            FAngularSweepCS::FPermutationDomain Permutation; Permutation.Set<FAngularSweepCS::FKind>(Kind);
            TShaderMapRef<FAngularSweepCS> Shader(ShaderMap, Permutation);
            auto* Parameters = GraphBuilder.AllocParameters<FTransportParameters>(); *Parameters = P;
            FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("FogMS B3 %d ordinate wavefronts", Common.AngularCount),
                ERDGPassFlags::Compute, Shader, Parameters, FIntVector(Common.AngularCount, 1, 1));
        }
        return Angular;
    };
    {
        auto P = Common; P.Angular = GraphBuilder.CreateSRV(Sweep(nullptr, 0)); P.GatherKind = 0;
        P.OutA = GraphBuilder.CreateUAV(U); P.OutB = GraphBuilder.CreateUAV(R); P.OutC = GraphBuilder.CreateUAV(Pcg);
        P.OutPrimary = GraphBuilder.CreateUAV(Primary);
        Dispatch(4, TEXT("FogMS B2 initialize PCG"), P, Cells);
    }
    Reduce(R, Pcg, 0);
    if (bWarm)
    {
        // Krylov continuation: u0 = sqrt(S)*J_prev, r0 = b - A u0. Scalars[3] keeps the cold rho.
        FRDGBufferRef U0 = Buffer(TEXT("FogMS.Transport.WarmSeed"));
        FRDGBufferRef WarmU = Buffer(TEXT("FogMS.Transport.WarmU")), WarmR = Buffer(TEXT("FogMS.Transport.WarmR")), WarmZ = Buffer(TEXT("FogMS.Transport.WarmZ"));
        { auto P = Common; P.OutA = GraphBuilder.CreateUAV(U0); Dispatch(15, TEXT("FogMS transport warm seed"), P, Cells); }
        { auto P = Common; P.Angular = GraphBuilder.CreateSRV(Sweep(U0, 1)); P.InputA = GraphBuilder.CreateSRV(U0); P.InputB = GraphBuilder.CreateSRV(R);
          P.OutA = GraphBuilder.CreateUAV(WarmU); P.OutB = GraphBuilder.CreateUAV(WarmR); P.OutC = GraphBuilder.CreateUAV(WarmZ);
          Dispatch(16, TEXT("FogMS transport warm residual"), P, Cells); }
        U = WarmU; R = WarmR; Pcg = WarmZ;
        Reduce(R, Pcg, 3);
    }
    for (int32 Iteration = 0; Iteration < Request.Iterations; ++Iteration)
    {
        FRDGBufferRef Ap = Buffer(TEXT("FogMS.B2.Ap"));
        { auto P = Common; P.Angular = GraphBuilder.CreateSRV(Sweep(Pcg, 1)); P.GatherKind = 1;
          P.InputA = GraphBuilder.CreateSRV(Pcg); P.OutA = GraphBuilder.CreateUAV(Ap); Dispatch(10, TEXT("FogMS B2 operator"), P, Cells); }
        Reduce(Pcg, Ap, 1);
        FRDGBufferRef NextU = Buffer(TEXT("FogMS.B2.NextU")), NextR = Buffer(TEXT("FogMS.B2.NextR")), Z = Buffer(TEXT("FogMS.B2.Z"));
        { auto P = Common; P.InputA = GraphBuilder.CreateSRV(U); P.InputB = GraphBuilder.CreateSRV(R);
          P.InputC = GraphBuilder.CreateSRV(Pcg); P.Primary = GraphBuilder.CreateSRV(Ap);
          P.OutA = GraphBuilder.CreateUAV(NextU); P.OutB = GraphBuilder.CreateUAV(NextR); P.OutC = GraphBuilder.CreateUAV(Z);
          Dispatch(7, TEXT("FogMS B2 PCG update"), P, Cells); }
        Reduce(NextR, Z, 2);
        FRDGBufferRef NextP = Buffer(TEXT("FogMS.B2.NextP"));
        { auto P = Common; P.InputA = GraphBuilder.CreateSRV(Z); P.InputB = GraphBuilder.CreateSRV(Pcg);
          P.OutA = GraphBuilder.CreateUAV(NextP); Dispatch(8, TEXT("FogMS B2 PCG direction"), P, Cells); }
        U = NextU; R = NextR; Pcg = NextP;
    }
    FRDGBufferRef Flux = Buffer(TEXT("FogMS.B2.Flux"), Common.AngularCount * Cells);
    FRDGBufferRef Angular = Sweep(U, 2, Flux);
    FRDGTextureRef Atlas = GraphBuilder.CreateTexture(FRDGTextureDesc::Create2D(FIntPoint(TransportGridSize, 4 * TransportGridSize * TransportGridSize),
        PF_A32B32G32R32F, FClearValueBinding::None, Flags), TEXT("FogMS.B2.Atlas"));
    { auto P = Common; P.Angular = GraphBuilder.CreateSRV(Angular); P.Flux = GraphBuilder.CreateSRV(Flux);
      P.InputA = GraphBuilder.CreateSRV(U); P.Primary = GraphBuilder.CreateSRV(Primary); P.OutAtlas = GraphBuilder.CreateUAV(Atlas);
      Dispatch(9, TEXT("FogMS B2 publish radiance residual flux"), P, Cells); }
    if (TestReconstruction.GetValueOnRenderThread() != 0)
    {
        FRDGTextureRef Resolved = GraphBuilder.CreateTexture(Atlas->Desc, TEXT("FogMS.B2.ReceiverDiagnostic"));
        AddCopyTexturePass(GraphBuilder, Atlas, Resolved);
        auto P = Common; P.ReconstructionAtlas = Atlas; P.OutAtlas = GraphBuilder.CreateUAV(Resolved);
        Dispatch(13, TEXT("FogMS B2 receiver diagnostic"), P, Cells);
        return Resolved;
    }
    return Atlas;
#else
    return nullptr;
#endif
}
