#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "FogMS_BoxVolume.generated.h"

class UBoxComponent;
class UMaterialInterface;
class UMaterialInstanceDynamic;
class UStaticMeshComponent;
class UVolumeTexture;
class FTextureResource;

UENUM(BlueprintType)
enum class EFogMSDensityChannel : uint8
{
	R,
	G,
	B,
	A
};

UENUM(BlueprintType)
enum class EFogMSScatteringMode : uint8
{
	Off = 0 UMETA(DisplayName="Off (A1 Only)"),
	Octaves = 1 UMETA(DisplayName="Octaves"),
	SpatialPreview = 2 UMETA(DisplayName="Spatial (Experimental)"),
	WorldSpace = 3 UMETA(DisplayName="World (Current Frame)")
};

/** Defines the live A1 region, optional directional scattering octaves and independent native volume density. */
UCLASS(BlueprintType, Blueprintable, ClassGroup=(FogMS), meta=(DisplayName="FogMS Box Volume"))
class MULTILOBESPEC_API AFogMSBoxVolume : public AActor
{
	GENERATED_BODY()

public:
	AFogMSBoxVolume();
	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void PostLoad() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual bool ShouldTickIfViewportsOnly() const override;
#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
	virtual void PostEditMove(bool bFinished) override;
#endif

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="FogMS", meta=(ToolTip="Enable this box as the live A1 region. Changes apply live after Enable Live Box has compiled the shaders. Use one enabled box per world."))
	bool bEnabled = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="FogMS", meta=(ClampMin="0.0", UIMin="0.0", Units="cm", ToolTip="Feather distance in world centimetres. Changes apply live after Enable Live Box has compiled the shaders."))
	float FeatherDistance = 200.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="FogMS", meta=(ToolTip="Shared bounds of the live A1 region and optional texture density. Move, rotate or resize the box to update them live."))
	TObjectPtr<UBoxComponent> BoxComponent;

	UBoxComponent* GetBoxComponent() const { return BoxComponent.Get(); }

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="FogMS|Scattering", meta=(ToolTip="Off keeps A1 self-shadowing. Octaves approximate extra directional scattering. Spatial uses the previous camera fog field. World computes primary lighting and three spatial scattering orders in the current frame, without fog history; native Lumen surface-cache coverage still applies. Changes apply live after Enable Live Box; density is independent."))
	EFogMSScatteringMode ScatteringMode = EFogMSScatteringMode::Off;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="FogMS|Scattering", meta=(EditCondition="ScatteringMode == EFogMSScatteringMode::Octaves", ClampMin="1", ClampMax="2", UIMin="1", UIMax="2", ToolTip="Number of added directional scattering octaves, beyond the native first order. One or two; changes apply live."))
	int32 ExtraOctaves = 2;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="FogMS|Scattering", meta=(DisplayName="MS Contribution", EditCondition="ScatteringMode == EFogMSScatteringMode::Octaves", ClampMin="0.0", ClampMax="1.0", UIMin="0.0", UIMax="1.0", ToolTip="Contribution of the added directional octaves. Zero keeps the A1-only path. This is an artistic approximation, not an energy-conserving spatial solver. Changes apply live."))
	float MSContribution = 0.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="FogMS|Scattering", meta=(DisplayName="MS Occlusion", EditCondition="ScatteringMode == EFogMSScatteringMode::Octaves", ClampMin="0.0", ClampMax="1.0", UIMin="0.0", UIMax="1.0", ToolTip="Optical-depth multiplier for added octaves. Lower values reduce their medium self-shadowing; geometric shadows remain unchanged. Changes apply live."))
	float MSOcclusion = 0.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="FogMS|Scattering", meta=(DisplayName="MS Eccentricity", EditCondition="ScatteringMode == EFogMSScatteringMode::Octaves", ClampMin="0.0", ClampMax="1.0", UIMin="0.0", UIMax="1.0", ToolTip="Phase blend for added octaves: zero is isotropic, one retains the current fog phase. The native first-order phase is unchanged. Changes apply live."))
	float MSEccentricity = 0.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="FogMS|Scattering", meta=(EditCondition="ScatteringMode == EFogMSScatteringMode::SpatialPreview || ScatteringMode == EFogMSScatteringMode::WorldSpace", ClampMin="0.0", ClampMax="0.5", ToolTip="Per-order damping of isotropic spatial transport. Both spatial modes require Scattering Distribution 0 and hardware ray tracing. Spatial uses previous camera fog history. World computes three extra orders in the current frame; zero keeps its primary indirect lighting active. World requires Lumen GI, graphics compute and RayTracing.Culling=0."))
	float SpatialStrength = 0.35f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="FogMS|Scattering", meta=(EditCondition="ScatteringMode == EFogMSScatteringMode::SpatialPreview || ScatteringMode == EFogMSScatteringMode::WorldSpace", ClampMin="10.0", ClampMax="2000.0", Units="cm", ToolTip="Maximum world distance over which neighbouring fog contributes scattered light. Solid geometry stops each transport ray."))
	float SpatialDistance = 500.0f;

	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Transient, Category="FogMS|Scattering")
	FString SpatialStatus = TEXT("Off");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="FogMS|Sun", meta=(DisplayName="Authored Sun Shadow", ToolTip="Use this box's authored density and height fog for directional medium self-shadowing, including density outside the camera. Ignores other local media's self-shadowing; native geometry shadows remain unchanged. Requires one enabled live box and valid density. Off preserves A1. Independent of Indirect Preview; changes apply live."))
	bool bAuthoredSunShadow = false;

	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Transient, Category="FogMS|Sun", meta=(ToolTip="Reports authored sun shadow availability or the reason for falling back to A1."))
	FString SunShadowStatus = TEXT("Off");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="FogMS|Sun", meta=(DisplayName="Cast Sun Shadow", ToolTip="Attenuate direct directional lighting on surfaces through this box's authored density. Requires one enabled live box and valid density. Independent of fog self-shadowing and Indirect Preview; changes apply live."))
	bool bCastSunShadow = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="FogMS|Sun", meta=(EditCondition="bCastSunShadow", ClampMin="0.0", ClampMax="1.0", UIMin="0.0", UIMax="1.0", ToolTip="Blend between native surface sunlight and attenuation through this box's density. Zero preserves native surface sunlight; changes apply live."))
	float SurfaceShadowStrength = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="FogMS|Sun", meta=(EditCondition="bCastSunShadow", ClampMin="1", ClampMax="64", UIMin="1", UIMax="64", ToolTip="Density samples along each surface-to-sun ray through the box. More samples improve thin/noisy shadows at increased GPU cost."))
	int32 SurfaceShadowSteps = 32;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="FogMS|Sun", meta=(EditCondition="bCastSunShadow", ToolTip="Prepare a light-space transmittance volume and filter it before surface lighting. Uses the primary atmosphere sun; other directional lights retain the reference march. Changes apply live."))
	bool bFilteredSunShadow = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="FogMS|Sun", meta=(EditCondition="bCastSunShadow && bFilteredSunShadow", ClampMin="0.0", ClampMax="1000.0", Units="cm", ToolTip="World-space standard deviation of cloud shadow filtering. This is an artistic softness control, independent of the sun's Source Angle and geometry shadows. Zero keeps the unfiltered cache."))
	float ShadowFilterSigma = 100.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="FogMS|Sun", meta=(EditCondition="bAuthoredSunShadow && bCastSunShadow && bFilteredSunShadow", ToolTip="Reuse the filtered sun visibility inside this volume. Softens narrow light shafts without animated noise; Shadow Filter Sigma controls softness for both the volume and surfaces. Uses the primary sun and falls back to the density march if the cache is unavailable. Off restores unfiltered medium shadows. Changes apply live."))
	bool bFilterSunInsideVolume = false;

	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Transient, Category="FogMS|Sun", meta=(ToolTip="Reports surface sun shadow availability or the reason for bypassing it."))
	FString SurfaceShadowStatus = TEXT("Off");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="FogMS|Indirect", meta=(DisplayName="Indirect Shadowing", ToolTip="Experimental attenuation of incoming Lumen lighting by this box's density, up to each ray's surface hit. Requires Enable Indirect Preview for the supported renderer settings. Does not add multiple scattering. Toggle live for comparison with A1b under identical renderer settings."))
	bool bIndirectShadowing = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="FogMS|Indirect", meta=(EditCondition="bIndirectShadowing", ClampMin="0.0", ClampMax="1.0", UIMin="0.0", UIMax="1.0", ToolTip="Blend between native Lumen fog illumination and attenuation through the local density. Zero preserves native illumination; changes apply live."))
	float IndirectShadowStrength = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="FogMS|Indirect", meta=(EditCondition="bIndirectShadowing", ClampMin="1", ClampMax="64", UIMin="4", UIMax="32", ToolTip="Density samples per incoming Lumen ray. More samples improve thin/noisy density at increased GPU cost."))
	int32 IndirectShadowSteps = 16;

	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Transient, Category="FogMS|Indirect", meta=(ToolTip="Reports the last rendered view family's experimental indirect-shadow status."))
	FString IndirectShadowStatus = TEXT("Off");

	UFUNCTION(CallInEditor, Category="FogMS|Indirect", meta=(DisplayName="Enable Indirect Preview", ToolTip="Enable experimental indirect shadows with fixed 8x8 angular sampling, without Lumen radiance cache, spatial filter or depth offset. Uses graphics compute and disables camera ray-tracing instance culling for World scattering. These settings also affect native Lumen. Restore Standard Lumen restores their previous session values."))
	void EnableIndirectPreview();

	UFUNCTION(CallInEditor, Category="FogMS|Indirect", meta=(DisplayName="Restore Standard Lumen", ToolTip="Disable experimental indirect shadows and restore the renderer settings captured by Enable Indirect Preview. Directional FogMS and density remain enabled."))
	void RestoreStandardLumen();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="FogMS|Density", meta=(DisplayName="Density Enabled", ToolTip="Add density from the 3D texture through native Volumetric Fog. Independent of A1 Enabled and Box Mode; changes apply live without FogMS.Apply."))
	bool bDensityEnabled = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="FogMS|Density", meta=(EditCondition="bDensityEnabled", ToolTip="Existing 3D texture. Missing or invalid textures disable this density source; no uniform replacement is used."))
	TObjectPtr<UVolumeTexture> DensityTexture;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="FogMS|Density", meta=(EditCondition="bDensityEnabled"))
	EFogMSDensityChannel DensityChannel = EFogMSDensityChannel::R;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="FogMS|Density", meta=(EditCondition="bDensityEnabled", ToolTip="Keep the 3D density texture aligned to world axes at a fixed world size. Moving, rotating or resizing the box changes its bounds without carrying the noise with it. Tile Scale is ignored in this mode."))
	bool bWorldAlignedTexture = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="FogMS|Density", meta=(EditCondition="bDensityEnabled && bWorldAlignedTexture", ClampMin="1.0", ClampMax="100000000.0", UIMin="1.0", UIMax="100000000.0", Units="cm", ToolTip="World-space size of one repetition of the base 3D texture. Uses the same texture channel and detail controls as local density; changes apply live."))
	float WorldTextureSize = 2000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="FogMS|Density", meta=(EditCondition="bDensityEnabled && !bWorldAlignedTexture", ClampMin="0.0001", UIMin="0.0001", ToolTip="Positive repetitions of the 3D texture along the box local axes. One means one texture across the box. Ignored when World Aligned Texture is enabled."))
	FVector TileScale = FVector::OneVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="FogMS|Density", meta=(EditCondition="bDensityEnabled", ClampMin="0.0", ClampMax="1.0", UIMin="0.0", UIMax="1.0"))
	float Threshold = 0.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="FogMS|Density", meta=(EditCondition="bDensityEnabled", ClampMin="0.0", ClampMax="1.0", UIMin="0.0", UIMax="1.0", ToolTip="Full transition width around Threshold. Zero uses a sharp threshold."))
	float Softness = 0.1f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="FogMS|Density", meta=(EditCondition="bDensityEnabled", ClampMin="0.0", ClampMax="1.0", UIMin="0.0", UIMax="1.0", ToolTip="Strength of finer density detail sampled from the same Volume Texture. Zero preserves the original density; changes apply live."))
	float DetailStrength = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="FogMS|Density", meta=(EditCondition="bDensityEnabled", ClampMin="0.1", ClampMax="32.0", UIMin="0.1", UIMax="32.0", ToolTip="Frequency multiplier for detail sampled from the same Volume Texture. Changes apply live."))
	float DetailScale = 4.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="FogMS|Density", meta=(EditCondition="bDensityEnabled", ClampMin="0.0", ClampMax="1.0", UIMin="0.0", UIMax="1.0", ToolTip="Relative weight of the second, finer detail octave from the same Volume Texture. Changes apply live."))
	float DetailSecondOctave = 0.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="FogMS|Density", meta=(EditCondition="bDensityEnabled", ClampMin="0.0", UIMin="0.0", ToolTip="Maximum added extinction in inverse metres (1/m). Zero adds no density. This does not subtract the existing height fog."))
	float Density = 0.1f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="FogMS|Density", meta=(EditCondition="bDensityEnabled", ClampMin="0.0", ClampMax="1.0", HideAlphaChannel, ToolTip="Scattering albedo of the added density; each RGB channel must be between zero and one."))
	FLinearColor DensityAlbedo = FLinearColor::White;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="FogMS|Density", meta=(EditCondition="bDensityEnabled", ClampMin="0.0", UIMin="0.0", Units="cm", ToolTip="Density feather inside the box edge, in world centimetres. Independent of the A1 Feather Distance."))
	float DensityEdgeFeather = 100.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="FogMS|Density", meta=(ToolTip="Native volume-material cube. Its bounds follow Box Component; density is controlled by the Density properties."))
	TObjectPtr<UStaticMeshComponent> DensityComponent;

	/** Synchronizes the native volume source on the game thread; unchanged values do not update the MID. */
	UFUNCTION(BlueprintCallable, Category="FogMS|Density")
	void UpdateDensity();

	uint64 GetDensityRevision() const { return DensityRevision; }
	bool IsDensitySourceActive() const { return LastDensityState.bActive; }
	const FString& GetDensityProblem() const { return LastDensityProblem; }
	/** Shares the validated float mapping used by the MID with the authored-density shader packet. */
	void GetDensityWorldMapping(FVector4f& OutFrequenciesAndMode, FVector3f& OutPhase0, FVector3f& OutPhase1, FVector3f& OutPhase2) const;

	UFUNCTION(CallInEditor, Category="FogMS", meta=(DisplayName="Enable Live Box", ToolTip="Enable FogMS A1 inside the live box and apply the shader overlay. Initial shader compilation is required. Afterwards movement, size, rotation, Feather Distance and Enabled changes apply live."))
	void EnableLiveBox();

	UFUNCTION(CallInEditor, Category="FogMS", meta=(DisplayName="Use Global A1", ToolTip="Enable FogMS A1 for the full fog volume and apply the shader overlay. Initial shader compilation is required. Enable Live Box again to use live box movement, size, rotation, Feather Distance and Enabled controls."))
	void UseGlobalA1();

private:
	struct FDensityState
	{
		bool bActive = false;
		TWeakObjectPtr<UVolumeTexture> Texture;
		const FTextureResource* TextureResource = nullptr;
		FLinearColor ChannelMask = FLinearColor::Black;
		FLinearColor TileScaleValue = FLinearColor::Black;
		bool bWorldAligned = false;
		FLinearColor WorldFrequencies = FLinearColor(0, 0, 0, 0);
		FLinearColor WorldPhase0 = FLinearColor(0, 0, 0, 0);
		FLinearColor WorldPhase1 = FLinearColor(0, 0, 0, 0);
		FLinearColor WorldPhase2 = FLinearColor(0, 0, 0, 0);
		float ThresholdValue = 0.0f;
		float SoftnessValue = 0.0f;
		float DetailStrengthValue = 0.0f;
		float DetailScaleValue = 4.0f;
		float DetailSecondOctaveValue = 0.5f;
		float DensityValue = 0.0f;
		FLinearColor Albedo = FLinearColor::Black;
		FLinearColor WorldExtent = FLinearColor::Black;
		float Feather = 0.0f;
		FTransform WorldTransform = FTransform::Identity;

		bool HasSameMaterialParameters(const FDensityState& Other) const;
		bool HasSameEffect(const FDensityState& Other) const;
	};

	UPROPERTY()
	TObjectPtr<UMaterialInterface> DensityMaterial;

	UPROPERTY(Transient, DuplicateTransient)
	TObjectPtr<UMaterialInstanceDynamic> DensityMID;

	FDensityState LastDensityState;
	FDensityState LastMaterialState;
	uint64 DensityRevision = 0;
	bool bHasMaterialState = false;
	bool bUpdatingDensity = false;
	FString LastDensityProblem;
};
