#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "FogMS_BoxVolume.generated.h"

class UBoxComponent;
class UArrowComponent;
class UMaterialInterface;
class UMaterialInstanceDynamic;
class UStaticMeshComponent;
class UTextureRenderTargetVolume;
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

/** Detail1 channel that erodes the lower edge of the density band (packet row 24.z = texture channel 1..3). */
UENUM(BlueprintType)
enum class EFogMSErosionChannel : uint8
{
	G = 0 UMETA(ToolTip="Texture channel G (Worley FBM in the bundled Perlin-Worley noise)."),
	B = 1 UMETA(ToolTip="Texture channel B."),
	A = 2 UMETA(ToolTip="Texture channel A.")
};

/** Editor-only shortcut: writes the five height-profile values (FogMS_DensityAuthoring_Design.md section 3). */
UENUM(BlueprintType)
enum class EFogMSHeightProfilePreset : uint8
{
	None = 0 UMETA(DisplayName="None", ToolTip="Values are edited directly. Editing any of the five values selects None."),
	Stratus = 1 UMETA(DisplayName="Stratus", ToolTip="Bottom 0.40, Top 0.60, Bottom Softness 0.05, Top Softness 0.10, Anvil 0."),
	Cumulus = 2 UMETA(DisplayName="Cumulus", ToolTip="Flat base, dome: Bottom 0.10, Top 0.70, Bottom Softness 0.02, Top Softness 0.45, Anvil 0."),
	Cumulonimbus = 3 UMETA(DisplayName="Cumulonimbus", ToolTip="Column with an anvil: Bottom 0.05, Top 0.98, Bottom Softness 0.02, Top Softness 0.10, Anvil 0.6."),
	ValleyFog = 4 UMETA(DisplayName="Valley Fog", ToolTip="Bottom 0, Top 0.35, Bottom Softness 0, Top Softness 0.30, Anvil 0.")
};

UENUM(BlueprintType)
enum class EFogMSScatteringMode : uint8
{
	Off = 0 UMETA(DisplayName="Off (A1 Only)"),
	Octaves = 1 UMETA(DisplayName="Octaves"),
	SpatialPreview = 2 UMETA(DisplayName="Spatial (Experimental)"),
	WorldSpace = 3 UMETA(DisplayName="World (Current Frame)"),
	Transport = 4 UMETA(DisplayName="Transport (B2)"),
	AngularTransport = 5 UMETA(DisplayName="Transport (B3 Angular)")
};

UENUM(BlueprintType)
enum class EFogMSAngularQuality : uint8
{
	Balanced48 = 0 UMETA(DisplayName="48 Directions"),
	High96 = 1 UMETA(DisplayName="96 Directions"),
	Low16 = 2 UMETA(DisplayName="16 Directions"),
	Medium24 = 3 UMETA(DisplayName="24 Directions")
};

/** Transport quality tier: writes Angular Quality, Transport Iterations and Transport Tolerance. */
UENUM(BlueprintType)
enum class EFogMSTransportPreset : uint8
{
	Production = 0 UMETA(DisplayName="Production", ToolTip="16 directions, up to 16 iterations, tolerance 1e-6."),
	High = 1 UMETA(DisplayName="High", ToolTip="48 directions, up to 16 iterations, tolerance 1e-8."),
	Cinematic = 2 UMETA(DisplayName="Cinematic", ToolTip="96 directions, 64 iterations, tolerance 1e-14."),
	Custom = 3 UMETA(DisplayName="Custom", ToolTip="Angular Quality, Transport Iterations and Transport Tolerance are edited directly.")
};

/** Transport: surface radiance where a boundary ray hits geometry. Packet row 5.z (0 Auto, 1 Off). */
UENUM(BlueprintType)
enum class EFogMSLumenBounce : uint8
{
	Auto = 0 UMETA(DisplayName="Auto", ToolTip="Lumen surface-cache radiance at ray hits when this engine build supports it (exact engine version 5.8.2) and the cache is ready; otherwise the fallback, without disabling Transport."),
	Off = 1 UMETA(DisplayName="Off", ToolTip="Always the fallback: hit surfaces lit by the sun (ray-traced shadow, attenuated by this Box's medium) plus SH sky irradiance, times the Box's Fallback Ground Albedo.")
};

UENUM(BlueprintType)
enum class EFogMSDensityMotionMode : uint8
{
	LegacyVectors = 0 UMETA(DisplayName="Legacy Velocity Vectors"),
	Directional = 1 UMETA(DisplayName="Directional Wind")
};

/** Saved reference for the current piecewise-linear motion segment, in world cm. */
USTRUCT(BlueprintType)
struct FFogMSDensityMotionReference
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="FogMS")
	bool bInitialized = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="FogMS", meta=(Units="s"))
	double Time = 0.0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="FogMS", meta=(Units="cm"))
	FVector Displacement0 = FVector::ZeroVector;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="FogMS", meta=(Units="cm"))
	FVector Displacement1 = FVector::ZeroVector;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="FogMS", meta=(Units="cm"))
	FVector Displacement2 = FVector::ZeroVector;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="FogMS", meta=(Units="cm/s"))
	FVector Velocity0 = FVector::ZeroVector;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="FogMS", meta=(Units="cm/s"))
	FVector Velocity1 = FVector::ZeroVector;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="FogMS", meta=(Units="cm/s"))
	FVector Velocity2 = FVector::ZeroVector;

	bool IsFinite() const;
	bool Equals(const FFogMSDensityMotionReference& Other) const;
};

inline bool FogMS_IsTransportMode(EFogMSScatteringMode Mode)
{
	return Mode == EFogMSScatteringMode::Transport || Mode == EFogMSScatteringMode::AngularTransport;
}

/** Defines the live fog region, directional/spatial scattering and authored volume density. */
UCLASS(BlueprintType, Blueprintable, ClassGroup=(FogMS), meta=(DisplayName="FogMS Box Volume"))
class MULTILOBESPEC_API AFogMSBoxVolume : public AActor
{
	GENERATED_BODY()

public:
	AFogMSBoxVolume();
	virtual void Serialize(FArchive& Ar) override;
	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void PostLoad() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void BeginPlay() override;
	virtual void Destroyed() override;
	virtual bool ShouldTickIfViewportsOnly() const override;
#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
	virtual void PostEditMove(bool bFinished) override;
#endif

	/** Several Boxes: any number of enabled Transport Boxes with Emissive Injection run at once, each solving into its own
	 * field (r.FogMS.MaxBoxesPerFrame solves per view and frame, the rest hold or queue); at most one enabled Box without
	 * Emissive Injection (the overlay path). Limits: a Box's rays and sun transmittance ignore every other Box's medium;
	 * overlapping Boxes add their injected emissive (double lighting where they overlap); the overlay delivery and its
	 * features (A1d/A1e, shadow cache, SSFS sky disk, FogMS.DumpSpatial) serve one Box only. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="FogMS", meta=(ToolTip="Enable this box as the live FogMS region. Changes apply live after Enable Live Box has compiled the shaders (automatic for Emissive Injection without -BindlessAll). Several Boxes: any number of enabled Transport Boxes with Emissive Injection run at once, each solving into its own field (r.FogMS.MaxBoxesPerFrame solves per view and frame; the others hold their last solve or wait, status 'Queued'); at most one enabled Box without Emissive Injection (the overlay path). Limits: a Box's rays and its sun transmittance do not see other Boxes' density; overlapping Boxes add their injected light (double lighting in the overlap, avoid overlaps); the overlay delivery and its features (authored/surface sun shadow, shadow cache, SSFS sky disk, FogMS.DumpSpatial) serve one Box only."))
	bool bEnabled = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="FogMS", meta=(ClampMin="0.0", UIMin="0.0", Units="cm", ToolTip="Feather distance in world centimetres. Changes apply live after Enable Live Box has compiled the shaders."))
	float FeatherDistance = 200.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="FogMS", meta=(ToolTip="Shared bounds of the live A1 region and optional texture density. Move, rotate or resize the box to update them live."))
	TObjectPtr<UBoxComponent> BoxComponent;

	UBoxComponent* GetBoxComponent() const { return BoxComponent.Get(); }

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="FogMS|Scattering", meta=(ToolTip="Off keeps A1 self-shadowing. Octaves approximate extra directional scattering. Spatial uses previous camera fog. World computes primary lighting and three extra orders in the current frame. B2 uses six transport directions; B3 uses 48 or 96 with a finite-volume solver. Both Transport modes are isotropic, without artistic damping or fog history; inspect convergence diagnostics. World and Transport use native Lumen surface-cache coverage. Changes apply live after Enable Live Box."))
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

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="FogMS|Scattering", meta=(EditCondition="ScatteringMode == EFogMSScatteringMode::Transport || ScatteringMode == EFogMSScatteringMode::AngularTransport", ToolTip="Transport quality tier (frozen-scene measurements, warm start on). Production: 16 directions, up to 16 iterations, tolerance 1e-6 (about 2.3-3.5 ms). High: 48 directions, up to 16 iterations, tolerance 1e-8. Cinematic: 96 directions, 64 iterations, tolerance 1e-14. A preset writes Angular Quality, Transport Iterations and Transport Tolerance; Custom edits them directly. Editing one of them (for example from Python) switches to Custom. B2 ignores the direction count."))
	EFogMSTransportPreset TransportPreset = EFogMSTransportPreset::Production;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="FogMS|Scattering", meta=(EditCondition="(ScatteringMode == EFogMSScatteringMode::Transport || ScatteringMode == EFogMSScatteringMode::AngularTransport) && TransportPreset == EFogMSTransportPreset::Custom", ClampMin="1", ClampMax="64", UIMin="1", UIMax="64", ToolTip="Iterations per frame of isotropic transport over the full box. With warm start (r.FogMS.Transport.WarmStart) the solution continues across frames; 2-4 is a production budget, 64 fully converges within one frame. Requires Scattering Distribution 0 and hardware ray tracing. Spatial Strength, Spatial Distance and Indirect Shadow Strength do not control this mode."))
	int32 TransportIterations = 24;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="FogMS|Scattering", meta=(EditCondition="ScatteringMode == EFogMSScatteringMode::AngularTransport && TransportPreset == EFogMSTransportPreset::Custom", ToolTip="Positive paired angular quadrature for B3. 16 and 24 are production budgets (best with warm start), 48 balanced, 96 reference. Fewer directions smear light across the axes; more directions cost linearly."))
	EFogMSAngularQuality AngularQuality = EFogMSAngularQuality::Balanced48;

	/** Negative (default -1) keeps r.FogMS.Transport.Tolerance, so actors saved before this property render unchanged. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category="FogMS|Scattering", meta=(EditCondition="(ScatteringMode == EFogMSScatteringMode::Transport || ScatteringMode == EFogMSScatteringMode::AngularTransport) && TransportPreset == EFogMSTransportPreset::Custom", ClampMin="-1.0", ClampMax="1.0", UIMin="-1.0", UIMax="0.001", ToolTip="b-relative convergence tolerance of the transport solver (PCG rho against the cold-start rho): the frame's remaining matrix passes are skipped below it. 0 runs all iterations. Negative (-1) uses the global r.FogMS.Transport.Tolerance; a value in [0,1] overrides it for this Box. Changing it keeps the warm start."))
	float TransportTolerance = -1.0f;

	/** Packet row 5.z in the Transport modes (0 Auto, 1 Off; FFogMSWorldRequest::bLumenBounce). Default Auto keeps row 5.z 0. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="FogMS|Scattering", meta=(DisplayName="Lumen Bounce", EditCondition="ScatteringMode == EFogMSScatteringMode::Transport || ScatteringMode == EFogMSScatteringMode::AngularTransport", ToolTip="Surface radiance where a transport boundary ray hits geometry. Auto: radiance from the Lumen surface cache at the hit when the engine build supports it (exact engine version 5.8.2) and the cache is ready; otherwise the fallback, and Transport stays active. Off: always the fallback: the hit surface lit by the sun with a ray-traced shadow and this Box's medium transmittance toward the sun (r.FogMS.World.FallbackMedium), plus SH sky irradiance, times Fallback Ground Albedo. One bounce, no emissive. The status line reports which one the field used."))
	EFogMSLumenBounce LumenBounce = EFogMSLumenBounce::Auto;

	/** Packet row 6.xyz in the Transport modes (FFogMSWorldRequest::FallbackGroundAlbedo); the overlay reads row 6.xyz only
	 * under Octaves. Default 0.3 grey reproduces the former global r.FogMS.World.FallbackAlbedo default. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="FogMS|Scattering", meta=(EditCondition="ScatteringMode == EFogMSScatteringMode::Transport || ScatteringMode == EFogMSScatteringMode::AngularTransport", HideAlphaChannel, ClampMin="0.0", ClampMax="1.0", UIMin="0.0", UIMax="1.0", ToolTip="Diffuse albedo assumed for the surfaces hit by the solver's boundary rays when the Lumen surface cache is not used (Lumen Bounce Off, or Auto while the cache is unavailable). Set it to this location's ground: fresh snow about 0.8, grass about 0.15, bare soil or rock about 0.2-0.3. An artist input per location, not a guess. Ignored while the status line reports bounce: Lumen. Changing it re-solves the field and resets fog history once."))
	FLinearColor FallbackGroundAlbedo = FLinearColor(0.3f, 0.3f, 0.3f, 1.0f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="FogMS|Scattering", meta=(EditCondition="ScatteringMode == EFogMSScatteringMode::Transport || ScatteringMode == EFogMSScatteringMode::AngularTransport", ToolTip="Experimental: deliver the transport field to the native volumetric fog through this Box's Volume material (Emissive = sigma_s * J) instead of the bindless overlay. Native voxelization then owns density, jitter and history for this Box; the overlay skips its density/source injection. Requires the material to read FogMS_TransportField / FogMS_InjectionMode."))
	bool bEmissiveInjection = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="FogMS|Scattering", meta=(DisplayName="Hybrid Single Scattering", EditCondition="bEmissiveInjection && (ScatteringMode == EFogMSScatteringMode::Transport || ScatteringMode == EFogMSScatteringMode::AngularTransport)", ToolTip="Experimental Emissive Injection split (v2): native volumetric fog renders the direct SUN single scattering (shadow maps, froxel resolution, fog Scattering Distribution phase); the field carries everything else: total incident J minus the uncollided sun term (sky with its aureole, local lights and all multiple scattering stay in the solver, with per-direction self-shadowing). Field alpha = 0.5 + 0.5 * T_sun * k, where T_sun is the solver's per-cell sun transmittance (medium + ray-traced geometry) and k the sun's share of the uncollided light, so native single scattering (which always adds sun + sky + local lights) contributes about the sun part only. At night k -> 0 and the Box behaves like full-field injection. Approximation: k is a luminance ratio; a local light inside the cloud is also scaled by T_sun*k. Requires the material to implement FogMS_InjectionMode 2."))
	bool bHybridSingleScattering = false;

	/** Game worlds only (packaged or -game; not PIE/editor): BeginPlay of an enabled Transport/World Box sets the solver's
	 * renderer requirements at ECVF_SetByGameSetting priority, below every project/ini/command-line value. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="FogMS", meta=(ToolTip="In a game world (packaged or -game; not PIE or the editor), BeginPlay of an enabled Transport/World Box sets r.RayTracing.Culling 0 and r.Lumen.AsyncCompute 0, which the transport solver requires (engine defaults 3 and 1), and logs the previous values. Uses game-setting priority: a value set by the project ini, device profile or command line is kept (and reported). Off = the project owns these cvars. The editor keeps Enable Indirect Preview."))
	bool bApplyRequiredRenderSettings = true;

	/** Transport field for Emissive Injection: 32^3 PF_FloatRGBA, scene-linear, not pre-exposed.
	 * Texel (x,y,z) is Box-local cell (x,y,z) along the Box rotation axes, cell centres at half texels:
	 * uvw = (Local + Extent) / (2 * Extent). A = 0 when cleared (no current field: the material falls back
	 * to native albedo lighting); the material treats A >= 0.5 as a valid current field.
	 * Full field (FogMS_InjectionMode 1, row 23.w 5): RGB = total incident radiance J, A = 1.
	 * Hybrid v2 (FogMS_InjectionMode 2, row 23.w 6): RGB = max(total - uncollided SUN term, 0): everything but the
	 * sun's uncollided light (sky with its aureole, local lights, all multiple scattering); A = 0.5 + 0.5 * T_sun * k,
	 * T_sun the cell's transmittance toward the atmosphere sun, k the sun's share of the uncollided light (luminance);
	 * no sun: k = 0. Material: S = saturate(2A - 1), BaseColor = Albedo * lerp(1, S, valid): native single scattering
	 * (sun + sky + local) then contributes about the sun part only. */
	UPROPERTY(Transient, DuplicateTransient, VisibleAnywhere, AdvancedDisplay, Category="FogMS|Scattering", meta=(ToolTip="Transport field for Emissive Injection (32^3 FloatRGBA, scene-linear). Texel (x,y,z) = Box-local cell (x,y,z) along the Box axes; uvw = (Local + Extent) / (2 * Extent), cell centres at half texels. Alpha 0 = cleared (fall back to native albedo lighting), alpha >= 0.5 = valid. Full field: RGB total J, alpha 1. Hybrid: RGB = J minus the uncollided sun term, alpha 0.5 + 0.5 * T_sun * k (k = sun share of the uncollided light)."))
	TObjectPtr<UTextureRenderTargetVolume> TransportField;

	/** Requested: Emissive Injection is enabled on a Transport/AngularTransport Box. */
	bool UsesEmissiveInjection() const { return bEmissiveInjection && FogMS_IsTransportMode(ScatteringMode); }
	/** Effective state applied to the MID by the last UpdateDensity (enabled Box, active density, valid field). */
	bool IsEmissiveInjectionActive() const { return LastDensityState.bActive && LastDensityState.bEmissiveInjection; }
	/** Effective hybrid split (FogMS_InjectionMode 2, packet row 23.w 6) from the last UpdateDensity; implies IsEmissiveInjectionActive(). */
	bool IsHybridInjectionActive() const { return IsEmissiveInjectionActive() && LastDensityState.bHybridInjection; }

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

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="FogMS|Indirect", meta=(DisplayName="Indirect Shadowing", EditCondition="ScatteringMode != EFogMSScatteringMode::Transport && ScatteringMode != EFogMSScatteringMode::AngularTransport", ToolTip="Experimental attenuation of incoming Lumen lighting by this box's density, up to each ray's surface hit. Requires Enable Indirect Preview for the supported renderer settings. Does not add multiple scattering. Transport uses its own density attenuation and ignores this switch."))
	bool bIndirectShadowing = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="FogMS|Indirect", meta=(EditCondition="bIndirectShadowing && ScatteringMode != EFogMSScatteringMode::Transport && ScatteringMode != EFogMSScatteringMode::AngularTransport", ClampMin="0.0", ClampMax="1.0", UIMin="0.0", UIMax="1.0", ToolTip="Blend between native Lumen fog illumination and attenuation through the local density. Zero preserves native illumination; changes apply live. Ignored by Transport."))
	float IndirectShadowStrength = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="FogMS|Indirect", meta=(EditCondition="bIndirectShadowing && ScatteringMode != EFogMSScatteringMode::Transport && ScatteringMode != EFogMSScatteringMode::AngularTransport", ClampMin="1", ClampMax="64", UIMin="4", UIMax="32", ToolTip="Density samples per incoming Lumen ray. More samples improve thin/noisy density at increased GPU cost. Ignored by Transport."))
	int32 IndirectShadowSteps = 16;

	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Transient, Category="FogMS|Indirect", meta=(ToolTip="Reports the last rendered view family's experimental indirect-shadow status."))
	FString IndirectShadowStatus = TEXT("Off");

	UFUNCTION(CallInEditor, Category="FogMS|Indirect", meta=(DisplayName="Enable Indirect Preview", ToolTip="Enable experimental indirect shadows with fixed 8x8 angular sampling, without Lumen radiance cache, spatial filter or depth offset. Uses graphics compute and disables camera ray-tracing instance culling for World scattering. These settings also affect native Lumen. Restore Standard Lumen restores their previous session values."))
	void EnableIndirectPreview();

	UFUNCTION(CallInEditor, Category="FogMS|Indirect", meta=(DisplayName="Restore Standard Lumen", ToolTip="Disable experimental indirect shadows and restore the renderer settings captured by Enable Indirect Preview. Directional FogMS and density remain enabled."))
	void RestoreStandardLumen();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="FogMS|Density", meta=(DisplayName="Density Enabled", ToolTip="Enable authored density from the 3D texture. An enabled Transport box renders its matching density; other modes use native Volumetric Fog independently of A1 Enabled. Changes apply live without FogMS.Apply."))
	bool bDensityEnabled = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="FogMS|Density", meta=(EditCondition="bDensityEnabled", ToolTip="Existing 3D texture. Missing or invalid textures disable this density source; no uniform replacement is used."))
	TObjectPtr<UVolumeTexture> DensityTexture;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="FogMS|Density", meta=(EditCondition="bDensityEnabled"))
	EFogMSDensityChannel DensityChannel = EFogMSDensityChannel::R;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="FogMS|Density", meta=(EditCondition="bDensityEnabled", ToolTip="Keep the 3D density texture aligned to world axes at a fixed world size. Moving, rotating or resizing the box changes its bounds without carrying the noise with it. Tile Scale is ignored in this mode."))
	bool bWorldAlignedTexture = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="FogMS|Density", meta=(EditCondition="bDensityEnabled && bWorldAlignedTexture", ClampMin="1.0", ClampMax="100000000.0", UIMin="1.0", UIMax="100000000.0", Units="cm", ToolTip="World-space size of one repetition of the base 3D texture. Uses the same texture channel and detail controls as local density; changes apply live."))
	float WorldTextureSize = 2000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="FogMS|Density", meta=(DisplayName="Texture Offset (World)", EditCondition="bDensityEnabled && bWorldAlignedTexture", Units="cm", ToolTip="World XYZ translation of the texture, independent of Box bounds. Positive X moves the pattern towards positive world X. Applies with animation on or off; zero preserves the original static mapping."))
	FVector TextureOffsetWorld = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="FogMS|Density Animation", meta=(DisplayName="Animate Density", ToolTip="Animate world-aligned density in World or Transport mode. Off preserves the static mapping. Uses one CPU world-time snapshot for the native density material and all FogMS lighting/shadow paths; no GPU clock. Spatial, Octaves and Off keep static density."))
	bool bAnimateDensity = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="FogMS|Density Animation", meta=(ToolTip="Existing saved actors retain Legacy Velocity Vectors. Use Directional Motion converts explicitly without changing the current phase. New actors use Directional Wind."))
	EFogMSDensityMotionMode DensityMotionMode = EFogMSDensityMotionMode::Directional;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="FogMS|Density Animation", meta=(ToolTip="Select this component and rotate its arrow to set world wind direction. Its rotation is independent of the Box; its position follows the Box. Used by Directional Wind."))
	TObjectPtr<UArrowComponent> WindDirectionComponent;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="FogMS|Density Animation", meta=(EditCondition="DensityMotionMode == EFogMSDensityMotionMode::Directional", EditConditionHides, ClampMin="0.0", UIMin="0.0", Units="cm/s", ToolTip="Common speed along the independent wind arrow. All three noise octaves share this motion; Edge Flow Speed and advanced relative velocities add detail motion. Speed and arrow edits preserve the current density phase."))
	double WindSpeed = 0.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="FogMS|Density Animation", meta=(EditCondition="DensityMotionMode == EFogMSDensityMotionMode::Directional", EditConditionHides, ClampMin="0.0", UIMin="0.0", Units="cm/s", ToolTip="Speed of slow contour changes on top of the common wind. Requires Animate Density and Detail Strength > 0; Detail Strength controls their amount. The large-scale noise continues to follow Wind Speed. Zero stops relative detail flow. Editing this speed preserves the current pattern."))
	double EdgeFlowSpeed = 0.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category="FogMS|Density Animation", meta=(EditCondition="DensityMotionMode == EFogMSDensityMotionMode::LegacyVectors", EditConditionHides, Units="cm/s", ToolTip="Original serialized world velocity. Legacy mode preserves its exact absolute-time behavior. Use Directional Motion to get a wind arrow, speed and continuous velocity edits."))
	FVector DensityWindVelocity = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category="FogMS|Density Animation", meta=(DisplayName="Relative Detail Velocity", EditCondition="bAnimateDensity", Units="cm/s", ToolTip="Advanced world velocity added to both detail octaves on top of common wind and their automatic Edge Flow. Zero with Edge Flow Speed zero keeps all shapes moving together. Legacy mode retains its original cumulative vector behavior."))
	FVector DensityDetailVelocity = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category="FogMS|Density Animation", meta=(DisplayName="Relative Second Detail Velocity", EditCondition="bAnimateDensity", Units="cm/s", ToolTip="Advanced world velocity added only to the second detail octave. Its total is Wind + Relative Detail + Relative Second Detail, plus its automatic Edge Flow in Directional mode. Zero with the other relative velocity and Edge Flow zero is coherent advection."))
	FVector DensityEvolutionVelocity = FVector::ZeroVector;

	/** Serialized implementation state; accessible through editor reflection for test/recovery snapshots. */
	// BlueprintReadOnly is deliberate: non-editable BlueprintReadWrite actor
	// properties are reset to the CDO by ResetPropertiesForConstruction on edits.
	UPROPERTY(BlueprintReadOnly, Category="FogMS|Density Animation")
	FFogMSDensityMotionReference DensityMotionReference;

	UFUNCTION(BlueprintCallable, Category="FogMS|Density Animation", meta=(ToolTip="Restore a finite saved motion reference after restoring the matching wind controls. Intended for reproducible scripting and recovery; does not add a Details control."))
	bool RestoreDensityMotionReference(const FFogMSDensityMotionReference& Reference);

	UFUNCTION(BlueprintCallable, CallInEditor, Category="FogMS|Density Animation", meta=(ToolTip="Convert the original velocity vectors to the wind arrow and speed, preserving the current phase and advanced relative detail controls. Does not enable animation or change density settings."))
	void UseDirectionalMotion();

	UFUNCTION(BlueprintCallable, Category="FogMS|Density Animation", meta=(ToolTip="Return to the retained original absolute-time velocity controls. This is an explicit phase change, not a continuous conversion back; Directional wind and its saved reference are retained."))
	void UseLegacyMotion();

	UFUNCTION(BlueprintCallable, Category="FogMS|Density Animation", meta=(ToolTip="Directional mode: set all motion displacements to zero at the current animation time. This is an explicit phase reset; wind controls, manual time and Texture Offset are unchanged."))
	void ResetMotionOrigin();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category="FogMS|Density Animation", meta=(EditCondition="bAnimateDensity", ToolTip="Freeze the density at Manual Animation Time plus Time Offset. Editing the manual time is an explicit seek and rejects old fog history once. Live world time respects game pause and time dilation."))
	bool bUseManualAnimationTime = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category="FogMS|Density Animation", meta=(EditCondition="bAnimateDensity && bUseManualAnimationTime", Units="s", ToolTip="Absolute density animation time for a frozen, reproducible frame. May be negative. Time Offset is added after selecting manual or live world time."))
	double ManualAnimationTime = 0.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category="FogMS|Density Animation", meta=(EditCondition="bAnimateDensity", Units="s", ToolTip="Time offset added to manual or live world time. Editing it explicitly seeks the density and rejects old fog history once."))
	double AnimationTimeOffset = 0.0;

	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Transient, AdvancedDisplay, Category="FogMS|Density Animation", meta=(Units="s"))
	double DensityAnimationTime = 0.0;

	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Transient, Category="FogMS|Density Animation")
	FString DensityAnimationStatus = TEXT("Off");

	UFUNCTION(BlueprintCallable, CallInEditor, Category="FogMS|Density Animation", meta=(ToolTip="Freeze at the current density phase without changing the Time Offset."))
	void FreezeDensityAnimation();

	UFUNCTION(BlueprintCallable, CallInEditor, Category="FogMS|Density Animation", meta=(ToolTip="Resume live world time from the frozen phase by adjusting Time Offset."))
	void ResumeDensityAnimation();

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

	/** S1, packet row 24.x. Zero (default) leaves the density bit-identical to the former formula. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="FogMS|Density", meta=(EditCondition="bDensityEnabled", ClampMin="0.0", ClampMax="1.0", UIMin="0.0", UIMax="1.0", ToolTip="Edge erosion: near the lower edge of the Threshold band, subtracts Erosion Channel of the second detail sample (same texture, no extra loads), carving wispy billowy edges while the cores stay solid. Erosion only removes density. Zero is off (original density). Changes apply live."))
	float ErosionStrength = 0.0f;

	/** S1, packet row 24.y: noise-space depth over which erosion fades from the band's lower edge (Lo) to zero. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="FogMS|Density", meta=(EditCondition="bDensityEnabled && ErosionStrength > 0", ClampMin="0.01", ClampMax="1.0", UIMin="0.01", UIMax="1.0", ToolTip="How deep into the cloud erosion reaches, in noise units above Threshold - Softness/2: full erosion at the edge, none from this depth on. Larger values erode deeper."))
	float ErosionDepth = 0.3f;

	/** S1, packet row 24.z (texture channel index 1..3). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="FogMS|Density", meta=(EditCondition="bDensityEnabled && ErosionStrength > 0", ToolTip="Channel of the second detail sample used as the erosion pattern. The bundled Perlin-Worley texture stores Worley FBM in G, B and A."))
	EFogMSErosionChannel ErosionChannel = EFogMSErosionChannel::G;

	/** Editor-only: selecting a preset writes the five values below and enables Height Profile (PostEditChangeProperty). */
	UPROPERTY(EditAnywhere, Category="FogMS|Density", meta=(EditCondition="bDensityEnabled", ToolTip="Writes Height Bottom/Top, Bottom/Top Softness and Anvil Strength from a cloud type and enables Height Profile. Editing any of those values afterwards selects None. Editor only."))
	EFogMSHeightProfilePreset HeightProfilePreset = EFogMSHeightProfilePreset::None;

	/** S2, packet row 26.y. False (default) leaves the density bit-identical to the former formula. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="FogMS|Density", meta=(DisplayName="Height Profile", EditCondition="bDensityEnabled", ToolTip="Shape the density over the Box height (0 = bottom face, 1 = top face, along the Box Z axis): the noise is multiplied by a profile that is 0 below Height Bottom and above Height Top, so only noise peaks survive where the profile is low (flat bases, domes, anvils). Off keeps the original density."))
	bool bHeightProfile = false;

	/** S2, packet row 25.x, fraction of the Box height. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="FogMS|Density", meta=(EditCondition="bDensityEnabled && bHeightProfile", ClampMin="0.0", ClampMax="1.0", UIMin="0.0", UIMax="1.0", ToolTip="Cloud base as a fraction of the Box height (0 bottom face, 1 top face). Must be below Height Top."))
	float HeightBottom = 0.0f;

	/** S2, packet row 25.y, fraction of the Box height. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="FogMS|Density", meta=(EditCondition="bDensityEnabled && bHeightProfile", ClampMin="0.0", ClampMax="1.0", UIMin="0.0", UIMax="1.0", ToolTip="Cloud top as a fraction of the Box height. Must be above Height Bottom."))
	float HeightTop = 1.0f;

	/** S2, packet row 25.z. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="FogMS|Density", meta=(EditCondition="bDensityEnabled && bHeightProfile", ClampMin="0.0", ClampMax="1.0", UIMin="0.0", UIMax="1.0", ToolTip="Height over which the profile rises from 0 at Height Bottom to 1. Small values give a flat base."))
	float BottomSoftness = 0.05f;

	/** S2, packet row 25.w. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="FogMS|Density", meta=(EditCondition="bDensityEnabled && bHeightProfile", ClampMin="0.0", ClampMax="1.0", UIMin="0.0", UIMax="1.0", ToolTip="Height below Height Top over which the profile falls to 0. Large values give a dome."))
	float TopSoftness = 0.1f;

	/** S2, packet row 26.x. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="FogMS|Density", meta=(EditCondition="bDensityEnabled && bHeightProfile", ClampMin="0.0", ClampMax="1.0", UIMin="0.0", UIMax="1.0", ToolTip="Widens coverage in the upper half of the profile (profile up to 1 + Anvil Strength), for a cumulonimbus anvil. Zero is off."))
	float AnvilStrength = 0.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="FogMS|Density", meta=(ToolTip="Native volume-material cube. Its bounds follow Box Component; density is controlled by the Density properties."))
	TObjectPtr<UStaticMeshComponent> DensityComponent;

	/** Synchronizes the native volume source on the game thread; unchanged values do not update the MID. */
	UFUNCTION(BlueprintCallable, Category="FogMS|Density")
	void UpdateDensity();

	uint64 GetDensityRevision() const { return DensityRevision; }
	bool IsDensitySourceActive() const { return LastDensityState.bActive; }
	bool IsDensityAnimationActive() const { return LastDensityState.bAnimationActive; }
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
		bool bUseNativeDensity = true;
		bool bEmissiveInjection = false;
		/** bEmissiveInjection with bHybridSingleScattering: MID FogMS_InjectionMode 2. */
		bool bHybridInjection = false;
		TWeakObjectPtr<UTextureRenderTargetVolume> InjectionField;
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
		/** S1 edge erosion; ErosionMask is one-hot RGBA (MID FogMS_ErosionMask). */
		float ErosionStrengthValue = 0.0f;
		float ErosionDepthValue = 0.3f;
		FLinearColor ErosionMask = FLinearColor(0, 1, 0, 0);
		/** S2 height profile; with bHeightProfile false the five values stay at their neutral defaults. */
		bool bHeightProfile = false;
		float HeightBottomValue = 0.0f;
		float HeightTopValue = 1.0f;
		float BottomSoftnessValue = 0.05f;
		float TopSoftnessValue = 0.1f;
		float AnvilStrengthValue = 0.0f;
		float DensityValue = 0.0f;
		FLinearColor Albedo = FLinearColor::Black;
		FLinearColor WorldExtent = FLinearColor::Black;
		float Feather = 0.0f;
		FTransform WorldTransform = FTransform::Identity;
		FVector TextureOffset = FVector::ZeroVector;
		bool bAnimationActive = false;
		EFogMSDensityMotionMode MotionMode = EFogMSDensityMotionMode::LegacyVectors;
		FFogMSDensityMotionReference MotionReference;
		bool bManualAnimationTime = false;
		FVector WindVelocity = FVector::ZeroVector;
		FVector DetailVelocity = FVector::ZeroVector;
		FVector EvolutionVelocity = FVector::ZeroVector;
		double ManualTime = 0.0;
		double TimeOffset = 0.0;
		double SampleTime = 0.0;

		bool HasSameDensityParameters(const FDensityState& Other) const;
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
	/** Injection-only runtime (no BindlessAll) started by this actor itself (editor tick or BeginPlay). */
	bool bRuntimeAutoStarted = false;
	FString LastDensityProblem;

	/** Injection-only configuration: starts the Box runtime once without user action (no global state changes). */
	void AutoStartRuntime();
	/** Writes AngularQuality/TransportIterations/TransportTolerance from TransportPreset unless it is Custom. */
	void ApplyTransportPreset();
	/** Editor: writes the five height-profile values of HeightProfilePreset and enables it; None writes nothing. */
	void ApplyHeightProfilePreset();

	/** Lazily creates TransportField; returns true when it has a render resource. */
	bool EnsureTransportField();
	/** Drops TransportField, clears the MID overrides (re-applied on the next active update); GC releases the resource. */
	void ReleaseTransportField();

	bool GetDensityMotionVelocities(FVector& Out0, FVector& Out1, FVector& Out2) const;
	bool EvaluateDirectionalMotion(double Time, const FVector& Velocity0, const FVector& Velocity1,
		const FVector& Velocity2, FVector& Out0, FVector& Out1, FVector& Out2);

};
