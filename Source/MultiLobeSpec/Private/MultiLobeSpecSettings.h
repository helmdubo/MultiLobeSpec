#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "MultiLobeShaderPatcher.h"
#include "MultiLobeSpecSettings.generated.h"

UENUM()
enum class EMLSPreset : uint8
{
	/** Vanilla engine shaders. Overlay is not mapped at all (tonemapper/diffuse switches also inactive). */
	Off       UMETA(DisplayName = "Off (vanilla engine)"),
	Subtle    UMETA(DisplayName = "Dual Lobe — Subtle"),
	Cinematic UMETA(DisplayName = "Dual Lobe — Cinematic"),
	Custom    UMETA(DisplayName = "Custom")
};

UENUM()
enum class EMLSTonemap : uint8
{
	EngineACES  UMETA(DisplayName = "Engine (ACES 1.x)"),
	AgX         UMETA(DisplayName = "AgX Base"),
	AgXPunchy   UMETA(DisplayName = "AgX Punchy (Blender-style)"),
	GTUchimura  UMETA(DisplayName = "GT (Uchimura)"),
	GTUchimuraHC UMETA(DisplayName = "GT High Contrast (Uchimura)")
};

UENUM()
enum class EMLSMicroShadowMode : uint8
{
	Off                     UMETA(DisplayName = "Off"),
	ReferenceStep           UMETA(DisplayName = "Reference Step (Part 1)"),
	ActivisionWWII          UMETA(DisplayName = "Activision / CoD:WWII"),
	PZAnalytical            UMETA(DisplayName = "PZ Analytical (Part 1)"),
	GenericVNDFIsotropicLUT UMETA(DisplayName = "Generic VNDF-Based (Isotropic LUT)"),
	ConeOverlapExperimental UMETA(DisplayName = "Cone Overlap (Experimental)")
};

UENUM()
enum class EMLSIndirectMaterialVisibility : uint8
{
	DirectOnly        UMETA(DisplayName = "Direct Only (diagnostic)"),
	RawScalarAO       UMETA(DisplayName = "Raw Scalar AO (legacy)"),
	RGBInterreflection UMETA(DisplayName = "RGB Interreflection (WWII-like)")
};

UENUM()
enum class EMLSDebugView : uint8
{
	None            UMETA(DisplayName = "None"),
	EffectiveWeight UMETA(DisplayName = "Effective Lobe Weight"),
	SecondRoughness UMETA(DisplayName = "Second-Lobe Roughness")
};

UENUM()
enum class EMLSDiffuse : uint8
{
	Lambert UMETA(DisplayName = "Lambert (engine default)"),
	Chan    UMETA(DisplayName = "Rough GGX (Diffuse_GGX_Rough, 5.7)")
};

/**
 * Project Settings -> Plugins -> MultiLobe Specular.
 * Every change rewrites the overlay config .ush and triggers
 * "RecompileShaders Changed" (on-the-fly, no editor restart).
 */
UCLASS(config = Engine, defaultconfig, meta = (DisplayName = "MultiLobe Specular"))
class UMultiLobeSpecSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UMultiLobeSpecSettings();

	virtual FName GetCategoryName() const override { return FName(TEXT("Plugins")); }

	UPROPERTY(EditAnywhere, config, Category = "Preset")
	EMLSPreset Preset = EMLSPreset::Off;

	/** Independent of the BRDF preset (but requires Preset != Off, since it lives in the same overlay). */
	UPROPERTY(EditAnywhere, config, Category = "Preset")
	EMLSTonemap Tonemapper = EMLSTonemap::GTUchimura;

	/** Diffuse term used by DefaultLit. Chan adds grazing retro-reflection on rough surfaces. */
	UPROPERTY(EditAnywhere, config, Category = "Preset")
	EMLSDiffuse DiffuseModel = EMLSDiffuse::Lambert;

	UPROPERTY(EditAnywhere, config, Category = "Custom", meta = (ClampMin = "0.0", ClampMax = "0.6", EditCondition = "Preset == EMLSPreset::Custom"))
	float Lobe2Weight = 0.22f;

	UPROPERTY(EditAnywhere, config, Category = "Custom", meta = (ClampMin = "0.0", ClampMax = "0.6", EditCondition = "Preset == EMLSPreset::Custom"))
	float EnvLobe2Weight = 0.22f;

	/** Second lobe roughness = saturate(R * Scale + Offset). Scale > 1 widens the halo. */
	UPROPERTY(EditAnywhere, config, Category = "Custom", meta = (ClampMin = "0.25", ClampMax = "4.0", EditCondition = "Preset == EMLSPreset::Custom"))
	float Lobe2RoughnessScale = 2.0f;

	UPROPERTY(EditAnywhere, config, Category = "Custom", meta = (ClampMin = "0.0", ClampMax = "0.5", EditCondition = "Preset == EMLSPreset::Custom"))
	float Lobe2RoughnessOffset = 0.10f;

	/**
	 * Core protection: effective weight = Weight * saturate(R * CoreFade).
	 * Near-mirror surfaces keep their sharp core; haze fades in with roughness.
	 * Higher = haze appears earlier. 6.0 => full weight from R ~0.17.
	 */
	UPROPERTY(EditAnywhere, config, Category = "Custom", meta = (ClampMin = "1.0", ClampMax = "20.0", EditCondition = "Preset == EMLSPreset::Custom", DisplayName = "Haze Onset (near-mirror fade)"))
	float CoreFade = 6.0f;

	/** Include EnvBRDF/EnvBRDFApprox (IBL / reflections response) in the dual lobe. */
	UPROPERTY(EditAnywhere, config, Category = "Scope")
	bool bPatchEnvBRDF = true;

	/**
	 * Direct material micro-shadowing model. Generic VNDF is enabled only when
	 * its generated LUT has passed validation and is present in the overlay.
	 */
	UPROPERTY(EditAnywhere, config, Category = "Micro Shadowing")
	EMLSMicroShadowMode MicroShadowMode = EMLSMicroShadowMode::Off;

	/** Art-directed override. The physically authored default is 1.0. */
	UPROPERTY(EditAnywhere, config, Category = "Micro Shadowing", meta = (ClampMin = "0.0", ClampMax = "1.0", EditCondition = "MicroShadowMode != EMLSMicroShadowMode::Off", DisplayName = "Diffuse Strength (art-directed override)"))
	float MicroShadowDiffuseStrength = 1.0f;

	/** Art-directed override. The physically authored default is 1.0. */
	UPROPERTY(EditAnywhere, config, Category = "Micro Shadowing", meta = (ClampMin = "0.0", ClampMax = "1.0", EditCondition = "MicroShadowMode != EMLSMicroShadowMode::Off", DisplayName = "Specular Strength (art-directed override)"))
	float MicroShadowSpecularStrength = 1.0f;

	/**
	 * EXPERIMENTAL. Dual-blur of the reflected image in Lumen: a fraction of
	 * reflection rays is stochastically assigned to the widened lobe at ray
	 * generation; temporal accumulation integrates the mix into sharp
	 * reflection + soft halo. Needs Lumen temporal accumulation (default on).
	 */
	UPROPERTY(EditAnywhere, config, Category = "Lumen Dual Blur")
	bool bLumenDualBlur = false;

	/**
	 * PAIRED two-lobe IBL (captures/skylight): radiance gathered per lobe and
	 * multiplied by that lobe's own response (EnergyTerms.E or EnvBRDF), summed
	 * by weight. Cost when active: second capture/skylight gather for non-mirror
	 * pixels. Lumen/SSR buffer keeps a mixture response (lobe-ID pairing pending).
	 */
	UPROPERTY(EditAnywhere, config, Category = "Scope", meta = (DisplayName = "Paired Two-Lobe IBL"))
	bool bTwoSampleIBL = true;

	/**
	 * Apply the offline-generated 32x32x8 cone-aware EnvBRDF to reflection
	 * captures/skylight. Each lobe uses its own adjusted cone response and
	 * matching radiance gather. Lumen/SSR remains explicitly unpaired.
	 * Experimental staging only: the overlay static-data representation failed
	 * the UE 5.7 SM6 compile-cost gate and is rejected until a bound texture path
	 * is implemented.
	 */
	UPROPERTY(EditAnywhere, config, Category = "Micro Shadowing", meta = (DisplayName = "Cone-Aware Indirect Specular (Storage Blocked)", EditCondition = "bTwoSampleIBL && bPatchEnvBRDF"))
	bool bConeAwareIndirectSpecular = false;

	/** Material visibility semantics for indirect diffuse. RGB interreflection is the production default. */
	UPROPERTY(EditAnywhere, config, Category = "Micro-Shadowing", meta = (DisplayName = "Indirect Material Visibility"))
	EMLSIndirectMaterialVisibility IndirectMaterialVisibility = EMLSIndirectMaterialVisibility::RGBInterreflection;

	/** Diagnostic renders: visualize the model instead of lighting. */
	UPROPERTY(EditAnywhere, config, Category = "Debug")
	EMLSDebugView DebugView = EMLSDebugView::None;

	/** Rebuild the overlay from current settings and recompile changed shaders. */
	UFUNCTION(CallInEditor, Category = "Preset")
	void ApplyChanges();

	void FillConfig(FMLSShaderConfig& Out) const;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& Event) override;
#endif
};
