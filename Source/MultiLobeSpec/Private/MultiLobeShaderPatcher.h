#pragma once

#include "CoreMinimal.h"

struct FMLSShaderConfig
{
	/** Any MLS shading policy is active (BRDF, direct micro-shadow, or full indirect visibility). */
	bool  bEnabled = true;
	/** Dual-lobe/rough-diffuse BRDF features, independent from the overlay master gate. */
	bool  bBRDFEnabled = true;
	float Lobe2Weight = 0.22f;
	float EnvLobe2Weight = 0.22f;
	float Lobe2Scale = 2.0f;
	float Lobe2Offset = 0.10f;
	/** w_eff = w * saturate(Roughness * CoreFade): protects the sharp core on near-mirror surfaces. */
	float CoreFade = 6.0f;
	bool  bPatchEnvBRDF = true;
	bool  bPatchEnvBRDFApprox = true;
	/** 0 = Lambert (engine default), 1 = Diffuse_Chan (GGX-consistent, retro-reflection). */
	int32 DiffuseModel = 0;
	/** 0 = engine ACES, 1 = AgX Base, 2 = AgX Punchy (Blender-style look). */
	int32 TonemapMode = 0;
	/** Micro-shadowing of direct lighting from the material AO channel. */
	bool  bMicroShadow = false;
	/** 0..1 art-directed strength; physical default is 1. */
	float MicroShadowDiffuseStrength = 1.0f;
	/** 0..1 art-directed strength; physical default is 1. */
	float MicroShadowSpecularStrength = 1.0f;
	/** EMLSMicroShadowMode numeric value. */
	int32 MicroShadowMode = 0;
	/** Dual-blur of the reflected image in Lumen (stochastic lobe selection at ray generation). */
	bool  bLumenDualBlur = false;
	/** Fraction of reflection rays assigned to the wide lobe. */
	float LumenBlurWeight = 0.22f;
	/** True two-sample IBL for reflection captures/skylight (radiance sampled per lobe). */
	bool  bTwoSampleIBL = true;
	/** Staged cone-aware EnvBRDF for paired captures/skylight; static storage is not compile-admitted. */
	bool  bConeAwareIndirectSpecular = false;
	/** Persisted runtime selector only; excluded from the content-addressed overlay identity. */
	int32 DebugView = 0;
	/** 0 = Direct Only, 1 = Raw Scalar AO legacy, 2 = RGB Interreflection. */
	int32 IndirectMaterialVisibilityMode = 1;

	/**
	 * Every MLS consumer that needs continuous visibility shares this predicate.
	 * Legacy scalar AO keeps the stock GBuffer/sample-occlusion contract.
	 */
	bool NeedsRawMaterialVisibilityTransport() const
	{
		return bMicroShadow
			|| IndirectMaterialVisibilityMode == 2
			|| bConeAwareIndirectSpecular;
	}
};

/**
 * Builds the shader overlay:
 *  - copies <Engine>/Shaders -> OverlayDir (stamped by engine version + patch version)
 *  - clones target BRDF functions verbatim and installs dual-lobe wrappers
 *  - patches the Diffuse_Lambert call site in DefaultLitBxDF (define-switched)
 *  - reroutes FilmToneMap calls in PostProcessCombineLUTs.usf to MLS_FilmToneMap
 *    (define-switched AgX Base / AgX Punchy, engine ACES fallback)
 *  - writes MultiLobeSpecConfig.ush with current preset defines
 *
 * All patches are applied unconditionally on a fresh overlay; runtime behaviour
 * is driven entirely by defines in the config file, so preset switching never
 * requires re-copying — only a config rewrite + RecompileShaders Changed.
 *
 * The patcher never edits engine files in place — only the overlay copy.
 */
class FMultiLobeShaderPatcher
{
public:
	/** Bump when the patch logic changes to force overlay rebuild. */
	static constexpr int32 PatchVersion = 39;

	static bool BuildOverlay(const FString& EngineShaderDir, const FString& OverlayDir,
	                         const FMLSShaderConfig& Cfg, FString& OutError);

	/** Stable Core SHA-1 identity over engine version, patch version, and shader config. */
	static FString GetOverlayBuildId(const FMLSShaderConfig& Cfg);

	/** Only rewrite the config .ush (fast path for preset switching). */
	static bool WriteConfigFile(const FString& OverlayDir, const FMLSShaderConfig& Cfg, FString& OutError);

private:
	static bool EnsureOverlayCopy(const FString& EngineShaderDir, const FString& OverlayDir,
	                              const FMLSShaderConfig& Cfg, bool& bOutFreshCopy, FString& OutError);
	static bool WriteCapabilityManifest(const FString& OverlayDir, const FMLSShaderConfig& Cfg, FString& OutError);

	static int32 PatchFunctionDualLobe(FString& Source, const FString& FunctionName,
	                                   const FString& WeightExpr,
	                                   const TCHAR* GuardExpr = TEXT("MLS_BRDF_ENABLED"),
	                                   const TCHAR* RequiredParam = nullptr,
	                                   const TCHAR* PerLobeMulTemplate = nullptr);

	/** Exact DefaultLit non-anisotropic call-site adapter with per-lobe micro-shadowing. */
	static int32 PatchDefaultLitDualSpecular(FString& Source);

	/** Specialized Rect LTC adapter: honest mean direction + per-lobe energy (no generic inout). */
	static int32 PatchRectAdapter(FString& Source);

	/** Replace Diffuse_Lambert(...) inside DefaultLitBxDF with the MLS macro. Returns count. */
	static int32 PatchDiffuseCallSite(FString& Source);

	/** Inject the define-switched micro-shadow multiplier before 'return Lighting;' in DefaultLitBxDF. */
	static int32 PatchMicroShadow(FString& Source);

	/** Per-lobe energy terms: mix EnergyPreservation/Conservation over both lobes inside DefaultLitBxDF. */
	static int32 PatchEnergyMix(FString& Source);

	/** Lumen resolve: reconstruction kernel/effective roughness follow the mixture roughness. */
	static bool PatchLumenResolve(const FString& OverlayDir, FString& OutWarning);

	/** Paired per-lobe IBL at the legacy composite call-site (radiance x response per lobe). */
	static bool PatchPairedIBL(const FString& OverlayDir, FString& OutWarning);

	/** Reflection captures/skylight: separate material visibility from screen/geometric AO. */
	static bool PatchIndirectSpecularMaterialVisibility(const FString& OverlayDir, FString& OutWarning);

	/** Write the isolated CARD-09 packed RG16 sampler used only by reflection composite shaders. */
	static bool WriteConeEnvBRDFRuntimeFile(const FString& OverlayDir, FString& OutError);

	/** Indirect material AO -> interreflection visibility (DiffuseIndirectComposite, non-Lumen branch). */
	static bool PatchIndirectAO(const FString& OverlayDir, FString& OutWarning);

	/** Lumen SPG: configure material visibility separately from geometric occlusion. */
	static bool PatchLumenIndirectAO(const FString& OverlayDir, FString& OutWarning);

	/** Skylight diffuse: separate RGB material visibility from Screen/DFAO geometry visibility. */
	static bool PatchSkyLightIndirectAO(const FString& OverlayDir, FString& OutWarning);

	/** Preserve continuous raw MaterialAO for direct micro-shadowing. */
	static bool PatchRawMaterialVisibilityTransport(const FString& OverlayDir, FString& OutError);

	/** Patch PostProcessCombineLUTs.usf: inject AgX include, reroute FilmToneMap calls. */
	static bool PatchTonemapper(const FString& OverlayDir, FString& OutWarning);

	/** Write the self-contained AgX implementation file into the overlay. */
	static bool WriteAgXFile(const FString& OverlayDir, FString& OutError);

	/** Patch Lumen reflection ray generation: reroute ImportanceSampleVisibleGGX to the dual-lobe wrapper. */
	static bool PatchLumenDualBlur(const FString& OverlayDir, FString& OutWarning);

	/** Write the dual-lobe GGX sampling wrapper into the overlay. */
	static bool WriteLumenDualBlurFile(const FString& OverlayDir, FString& OutError);

	/** Identifier-boundary rename of call sites From( -> To(. Returns count. */
	static int32 ReplaceIdentifierCalls(FString& Source, const FString& From, const FString& To);

	static void EnsureConfigInclude(FString& Source);

	struct FFuncDef
	{
		int32 Start = 0;
		int32 NamePos = 0;
		int32 ParamsOpen = 0;
		int32 ParamsClose = 0;
		int32 BodyOpen = 0;
		int32 BodyClose = 0;
	};

	static bool FindNextFunctionDef(const FString& Src, const FString& Name, int32 SearchFrom, FFuncDef& Out);
	static void ParseParamNames(const FString& ParamList, TArray<FString>& OutNames);
};
