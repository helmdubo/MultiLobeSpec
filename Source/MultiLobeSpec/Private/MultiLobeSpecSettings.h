#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "MultiLobeShaderPatcher.h"
#include "MultiLobeSpecSettings.generated.h"

UENUM()
enum class EMLSPreset : uint8
{
	Off UMETA(DisplayName = "Off (vanilla engine)"),
	Subtle UMETA(DisplayName = "Dual Lobe — Subtle"),
	Cinematic UMETA(DisplayName = "Dual Lobe — Cinematic"),
	Custom UMETA(DisplayName = "Custom")
};

UENUM()
enum class EMLSTonemap : uint8
{
	EngineACES UMETA(DisplayName = "Engine (ACES 1.x)"),
	AgX UMETA(DisplayName = "AgX Base"),
	AgXPunchy UMETA(DisplayName = "AgX Punchy (Blender-style)"),
	GTUchimura UMETA(DisplayName = "GT (Uchimura)"),
	GTUchimuraHC UMETA(DisplayName = "GT High Contrast (Uchimura)")
};

UENUM()
enum class EMLSMicroShadowMode : uint8
{
	Off UMETA(DisplayName = "Off / Legacy Material AO"),
	ReferenceStep UMETA(DisplayName = "Reference Step (Part 1)"),
	ActivisionWWII UMETA(DisplayName = "Activision / CoD:WWII (Recommended)"),
	PZAnalytical UMETA(DisplayName = "PZ Analytical (Part 1)"),
	GenericVNDFIsotropicLUT UMETA(DisplayName = "Generic VNDF LUT (Experimental — Research Only)"),
	ConeOverlapExperimental UMETA(DisplayName = "Cone Overlap (Experimental)")
};

UENUM()
enum class EMLSIndirectMaterialVisibility : uint8
{
	DirectOnly UMETA(DisplayName = "Direct Only / Micro-Shadow Only"),
	RawScalarAO UMETA(DisplayName = "UE Legacy Material AO"),
	RGBInterreflection UMETA(DisplayName = "RGB Interreflection (WWII full-stack experiment)")
};

UENUM()
enum class EMLSDebugView : uint8
{
	None UMETA(DisplayName = "None"),
	EffectiveWeight UMETA(DisplayName = "Effective Lobe Weight"),
	SecondRoughness UMETA(DisplayName = "Second-Lobe Roughness"),
	RawMaterialVisibility UMETA(DisplayName = "Raw Material Visibility (Lighting-weighted)"),
	DirectMicroShadow UMETA(DisplayName = "Final Direct Diffuse Micro-Shadow Multiplier (Lighting-weighted)"),
	DirectNoL UMETA(DisplayName = "Direct N dot L (Lighting-weighted)")
};

UENUM()
enum class EMLSDiffuse : uint8
{
	Lambert UMETA(DisplayName = "Lambert (engine default)"),
	Chan UMETA(DisplayName = "Rough GGX (Diffuse_GGX_Rough, 5.7)")
};

UCLASS(config = Engine, defaultconfig, meta = (DisplayName = "MultiLobe Specular"))
class UMultiLobeSpecSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UMultiLobeSpecSettings();
	virtual FName GetCategoryName() const override { return FName(TEXT("Plugins")); }

	UPROPERTY(EditAnywhere, config, Category = "Preset")
	EMLSPreset Preset = EMLSPreset::Off;

	UPROPERTY(EditAnywhere, config, Category = "Preset")
	EMLSTonemap Tonemapper = EMLSTonemap::EngineACES;

	UPROPERTY(EditAnywhere, config, Category = "Preset")
	EMLSDiffuse DiffuseModel = EMLSDiffuse::Lambert;

	UPROPERTY(EditAnywhere, config, Category = "Custom", meta = (ClampMin = "0.0", ClampMax = "0.6", EditCondition = "Preset == EMLSPreset::Custom"))
	float Lobe2Weight = 0.22f;

	UPROPERTY(EditAnywhere, config, Category = "Custom", meta = (ClampMin = "0.0", ClampMax = "0.6", EditCondition = "Preset == EMLSPreset::Custom"))
	float EnvLobe2Weight = 0.22f;

	UPROPERTY(EditAnywhere, config, Category = "Custom", meta = (ClampMin = "0.25", ClampMax = "4.0", EditCondition = "Preset == EMLSPreset::Custom"))
	float Lobe2RoughnessScale = 2.0f;

	UPROPERTY(EditAnywhere, config, Category = "Custom", meta = (ClampMin = "0.0", ClampMax = "0.5", EditCondition = "Preset == EMLSPreset::Custom"))
	float Lobe2RoughnessOffset = 0.10f;

	UPROPERTY(EditAnywhere, config, Category = "Custom", meta = (ClampMin = "1.0", ClampMax = "20.0", EditCondition = "Preset == EMLSPreset::Custom", DisplayName = "Haze Onset (near-mirror fade)"))
	float CoreFade = 6.0f;

	UPROPERTY(EditAnywhere, config, Category = "Scope")
	bool bPatchEnvBRDF = true;

	/** Any non-Off mode treats Material AO as direct micro-visibility only. */
	UPROPERTY(EditAnywhere, config, Category = "Micro Shadowing")
	EMLSMicroShadowMode MicroShadowMode = EMLSMicroShadowMode::ActivisionWWII;

	UPROPERTY(EditAnywhere, config, Category = "Micro Shadowing", meta = (ClampMin = "0.0", ClampMax = "1.0", EditCondition = "MicroShadowMode != EMLSMicroShadowMode::Off", DisplayName = "Diffuse Strength"))
	float MicroShadowDiffuseStrength = 1.0f;

	UPROPERTY(EditAnywhere, config, Category = "Micro Shadowing", meta = (ClampMin = "0.0", ClampMax = "1.0", EditCondition = "MicroShadowMode != EMLSMicroShadowMode::Off", DisplayName = "Specular Strength"))
	float MicroShadowSpecularStrength = 1.0f;

	UPROPERTY(EditAnywhere, config, AdvancedDisplay, Category = "Micro Shadowing", meta = (DisplayName = "Legacy Indirect Material AO", EditCondition = "MicroShadowMode == EMLSMicroShadowMode::Off"))
	EMLSIndirectMaterialVisibility IndirectMaterialVisibility = EMLSIndirectMaterialVisibility::RawScalarAO;

	UPROPERTY(EditAnywhere, config, Category = "Lumen Dual Blur")
	bool bLumenDualBlur = false;

	UPROPERTY(EditAnywhere, config, Category = "Scope", meta = (DisplayName = "Paired Two-Lobe IBL"))
	bool bTwoSampleIBL = true;

	UPROPERTY(EditAnywhere, config, Category = "Micro Shadowing", meta = (DisplayName = "Cone-Aware Indirect Specular (Storage Blocked)", EditCondition = "bTwoSampleIBL && bPatchEnvBRDF"))
	bool bConeAwareIndirectSpecular = false;

	/** Session-only: this selector never participates in the overlay identity. */
	UPROPERTY(EditAnywhere, Transient, Category = "Debug")
	EMLSDebugView DebugView = EMLSDebugView::None;

	UFUNCTION(CallInEditor, Category = "Preset")
	void ApplyChanges();

	void FillConfig(FMLSShaderConfig& Out) const;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& Event) override;
#endif
};
