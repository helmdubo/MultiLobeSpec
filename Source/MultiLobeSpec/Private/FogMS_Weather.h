#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "GameFramework/Actor.h"
#include "FogMS_Weather.generated.h"

class UMaterialInstanceDynamic;
class UMaterialInterface;
class USceneComponent;
class UTexture2D;
class UTextureRenderTarget2D;

/** W48 built-in weather states (FogMS_Weather_Design.md 1.6: oktas / 8, WMO altitudes of the middle latitudes). */
UENUM(BlueprintType)
enum class EFogMSWeatherPreset : uint8
{
	Custom = 0 UMETA(DisplayName="Custom", ToolTip="The values are edited directly."),
	Clear = 1 UMETA(DisplayName="Clear (SKC)", ToolTip="No low or middle clouds: no weather shadows (cirrus is W49's sky)."),
	Scattered = 2 UMETA(DisplayName="Scattered (SCT)", ToolTip="Cumulus mediocris, coverage 0.40 (3 oktas), base 1.0 km, top 2.5 km, extinction 0.05 1/m, wind 8 m/s; no deck."),
	Broken = 3 UMETA(DisplayName="Broken (BKN)", ToolTip="Stratocumulus / cumulus congestus, coverage 0.75 (6 oktas), base 0.8 km, top 3.0 km, extinction 0.07 1/m; altostratus deck 0.30 at 2.5-3.5 km; wind 10 m/s."),
	Overcast = 4 UMETA(DisplayName="Overcast (OVC)", ToolTip="Stratus / stratocumulus sheet, coverage 1.0 (8 oktas), base 0.5 km, top 1.2 km, extinction 0.07 1/m; altostratus deck 0.80 at 2.0-3.5 km; wind 10 m/s. No direct sun on the ground.")
};

/** W48 decision 2 (FogMS_Weather_Design.md 7): how the weather reaches the engine's cloud shadow map through the cloud host. Picked by the
 * phase-2 gate (round 48, owner camera, Overcast): Extended cost +1.04 ms in the visible pass (gate 0.3 ms), Thin +0.14 ms -> Thin is the
 * class default. */
UENUM(BlueprintType)
enum class EFogMSWeatherShadowLayer : uint8
{
	Extended = 0 UMETA(DisplayName="Extended Host Layer", ToolTip="The cloud host's layer grows up to the weather top; the weather density is evaluated in the shadow pass at its true altitude: the ground, the fog, Lumen and the atmosphere (light shafts in the air under the clouds) all get the weather shadow. The visible pass still draws only the hero Boxes but traces a taller layer (empty steps skipped r.FogMS.Weather.SkipSteps at a time): round 48 measured +1.04 ms at the owner camera (gate 0.3 ms), so it is not the default."),
	Thin = 1 UMETA(DisplayName="Thin Layer", ToolTip="Default since round 48 (the visible-pass gate). The host layer stays at the hero Box band (without a Box: a 0.1 km band under the weather base); the whole weather column along the sun (RT_FogMS_WeatherSun) is spread over that band in the shadow pass. The ground below gets the right shadow; the air above the band (light shafts, fog higher up) and terrain above the band get none. No visible-pass cost.")
};

/** The numbers of one weather state (physical scale; AFogMSWeather::WeatherScale scales them for a 'diorama'). */
USTRUCT(BlueprintType)
struct FFogMSWeatherValues
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Low Layer (L0)", meta=(DisplayName="Coverage", ClampMin="0.0", ClampMax="1.0", UIMin="0.0", UIMax="1.0", ToolTip="Fraction of the sky the low clouds cover, oktas / 8 (METAR: FEW 1-2, SCT 3-4, BKN 5-7, OVC 8). It is an exact area fraction of the weather map (the pattern channels are histogram-equalised); coverage grows by threshold, so clouds appear from their cores. 0 = no low layer."))
	float Coverage = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Low Layer (L0)", meta=(DisplayName="Cloud Type", ClampMin="0.0", ClampMax="1.0", UIMin="0.0", UIMax="1.0", ToolTip="0 stratus, 0.25 stratocumulus, 0.5 cumulus, 0.75 cumulus congestus, 1 cumulonimbus: picks the vertical profile in the cloud-type LUT and blends the stratiform (rolls, sheets) and cumuliform (heaps) fields of the weather pattern."))
	float CloudType = 0.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Low Layer (L0)", meta=(DisplayName="Base", ClampMin="0.0", UIMin="0.0", UIMax="5.0", Units="km", ToolTip="Cloud base above the SkyAtmosphere ground (km, physical scale)."))
	float BaseKm = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Low Layer (L0)", meta=(DisplayName="Top", ClampMin="0.0", UIMin="0.0", UIMax="12.0", Units="km", ToolTip="Top of the low layer (km). The type's profile spans Base..Top (stratus: a thin sheet at the base, cumulonimbus: the whole column)."))
	float TopKm = 2.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Low Layer (L0)", meta=(DisplayName="Extinction", ClampMin="0.0", UIMin="0.0", UIMax="0.3", ToolTip="Extinction coefficient sigma of the cloud body [1/m]. Physical clouds 0.02-0.3 (stratus ~0.06, cumulonimbus ~0.25): over a kilometre of cloud that is an optical depth of tens, so the direct sun is fully blocked (the sky light still comes through)."))
	float Extinction = 0.05f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Low Layer (L0)", meta=(DisplayName="Detail Strength", ClampMin="0.0", ClampMax="1.0", UIMin="0.0", UIMax="1.0", ToolTip="How much the detail noise erodes the cloud body (Nubis: density = saturate(noise - (1 - profile))). 0 = smooth blobs from the coverage and the profile only; 1 = full erosion."))
	float DetailStrength = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Middle Deck (L1)", meta=(DisplayName="Deck Coverage", ClampMin="0.0", ClampMax="1.0", UIMin="0.0", UIMax="1.0", ToolTip="Area fraction of the altostratus / nimbostratus deck. 0 = no deck."))
	float DeckCoverage = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Middle Deck (L1)", meta=(DisplayName="Deck Base", ClampMin="0.0", UIMin="0.0", UIMax="8.0", Units="km"))
	float DeckBaseKm = 2.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Middle Deck (L1)", meta=(DisplayName="Deck Top", ClampMin="0.0", UIMin="0.0", UIMax="10.0", Units="km"))
	float DeckTopKm = 3.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Middle Deck (L1)", meta=(DisplayName="Deck Extinction", ClampMin="0.0", UIMin="0.0", UIMax="0.2", ToolTip="Extinction of the deck [1/m] (altostratus ~0.02-0.05)."))
	float DeckExtinction = 0.03f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Wind", meta=(DisplayName="Wind Speed", ClampMin="0.0", UIMin="0.0", UIMax="40.0", Units="m/s", ToolTip="Speed of the cloud field (both layers in this version)."))
	float WindSpeed = 5.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Wind", meta=(DisplayName="Wind Direction", UIMin="-180.0", UIMax="180.0", Units="Degrees", ToolTip="The direction the clouds move TOWARD, as a yaw around +Z (0 = +X, 90 = +Y)."))
	float WindDirectionDeg = 30.0f;

	/** Component-wise blend (wind as a vector). */
	static FFogMSWeatherValues Lerp(const FFogMSWeatherValues& A, const FFogMSWeatherValues& B, float T);
	bool Equals(const FFogMSWeatherValues& Other) const;
	bool HasLowLayer() const { return Coverage > 0.0f && Extinction > 0.0f && TopKm > BaseKm; }
	bool HasDeck() const { return DeckCoverage > 0.0f && DeckExtinction > 0.0f && DeckTopKm > DeckBaseKm; }
};

/**
 * W48 weather state (FogMS_Weather_Design.md 3.3, 3.5): a primary data asset holding one set of FFogMSWeatherValues. The plugin ships
 * DA_FogMS_Weather_Clear / _Scattered / _Broken / _Overcast in /MultiLobeSpec/FogMS/Weather (created by matedit_weather.py with
 * ApplyPreset); make your own with the content browser (Miscellaneous > Data Asset > FogMS Weather State) or duplicate one.
 */
UCLASS(BlueprintType, meta=(DisplayName="FogMS Weather State"))
class MULTILOBESPEC_API UFogMSWeatherState : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="FogMS Weather", meta=(ToolTip="Picking a preset writes its values below; editing a value afterwards selects Custom."))
	EFogMSWeatherPreset Preset = EFogMSWeatherPreset::Custom;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="FogMS Weather", meta=(ShowOnlyInnerProperties))
	FFogMSWeatherValues Values;

	/** Writes the preset's values (Custom: nothing) and remembers the preset. */
	UFUNCTION(BlueprintCallable, Category="FogMS Weather")
	void ApplyPreset(EFogMSWeatherPreset InPreset);

	/** The built-in numbers of a preset (Custom: the struct defaults). */
	static FFogMSWeatherValues GetPresetValues(EFogMSWeatherPreset InPreset);

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif
};

/**
 * W48 FogMS Weather actor v0 (FogMS_Weather_Design.md 3.3-3.8, 4.1, 6 'W48'). One per level; placing it is the opt-in. Data flow per tick
 * (game thread, editor and game; nothing changes without this actor):
 *   state: the Weather State (or the target of SetWeather) blended from the previous values over the transition time (smoothstep):
 *     coverage grows by threshold, the type shifts along the LUT, altitudes / extinction / wind blend;
 *   RT_FogMS_WeatherMap (512^2 RGBA16F, wrap, one pattern tile = the weather domain of Domain Size km around this actor): cleared to
 *     (0,0,0,1) and drawn by M_FogMS_WeatherCompose (DrawMaterialToRenderTarget, Alpha Composite: RGB = emissive, A = 1 - opacity) from
 *     T_FogMS_WeatherPattern: R coverage L0 (exact area fraction), G type L0, B storm/precipitation (0 until W52), A deck L1. Redrawn only
 *     when the blended state changes: the wind moves it at lookup (the host shifts the lookup by the wind displacement), so a static state
 *     costs nothing per frame. A one-time readback of a calibration draw checks that the renderer writes all four channels;
 *   thin layer only: RT_FogMS_WeatherSun (512^2 R16F) = the optical depth of the weather column along the sun per ground point, drawn by
 *     M_FogMS_WeatherSun (24 samples of the same density function) whenever the map or the sun direction (> 0.05 deg) changes;
 *   UFogMSCloudHostSubsystem::FeedWeather: textures + domain / wind / layer vectors (+ the altitude envelope, rounded outward to 100 m
 *     x scale over both states of a transition). The subsystem (after the actor ticks) writes them into the cloud host's MID; M_FogMS_Cloud v3
 *     adds the weather density ONLY in the cloud shadow pass (Shadow Pass Switch), and extends the host layer (FogMS_CloudHost.h, W48);
 *   no cloud host in the level and Create Cloud Host ticked: SpawnHost (the FogMS.CloudHost.Create path); the actor remembers it and deletes
 *     it with itself when no Box renders through it. Without a Box the host is 'shadows only' (empty view trace);
 *   the engine's cloud shadow map (sun: Cast Cloud Shadows, extent/resolution, FogMS.Weather.SetupShadows) then shadows the ground, the
 *     fog, Lumen and the atmosphere. Not in W48: visible weather clouds (W49 sky dome), the weather inside the hero clouds and the solver
 *     (W50), storms and lightning (W52).
 * Status: WeatherStatus (Details), GetWeatherStatus, console FogMS.Weather.Status. Console FogMS.Weather.Set <preset|asset> [seconds].
 */
UCLASS(BlueprintType, Blueprintable, ClassGroup=(FogMS), hidecategories=(Collision, Physics, Input, HLOD, Replication, Networking, LevelInstance, Cooking), meta=(DisplayName="FogMS Weather"))
class MULTILOBESPEC_API AFogMSWeather : public AActor
{
	GENERATED_BODY()

public:
	AFogMSWeather();
	virtual void Tick(float DeltaSeconds) override;
	virtual bool ShouldTickIfViewportsOnly() const override { return true; }
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void Destroyed() override;
#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="FogMS Weather", meta=(ToolTip="Off: the weather stops; the cloud host's weather branch, layer and settings go back as before (log line)."))
	bool bEnabled = true;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="FogMS Weather", meta=(DisplayName="Weather State", ToolTip="The weather state this actor shows (DA_FogMS_Weather_Clear / _Scattered / _Broken / _Overcast in /MultiLobeSpec/FogMS/Weather, or your own FogMS Weather State asset). Changing it here switches over Editor Transition Seconds; from Blueprint use Set Weather. Empty = Clear."))
	TObjectPtr<UFogMSWeatherState> WeatherState;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="FogMS Weather", meta=(ClampMin="0.0", UIMin="0.0", UIMax="120.0", Units="s", ToolTip="Transition time when Weather State is changed in the Details panel (0 = immediate)."))
	float EditorTransitionSeconds = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="FogMS Weather", meta=(DisplayName="Shadow Layer", ToolTip="Owner decision 2, picked by the round-48 gate: Thin Layer (default: no visible-pass cost; the weather column is spread over the hero band, the ground gets the right shadow, the air above the band none) or Extended Host Layer (the weather at its true altitude, all consumers incl. light shafts in the air; +1.04 ms in the visible pass at the owner camera, round 48)."))
	EFogMSWeatherShadowLayer ShadowLayer = EFogMSWeatherShadowLayer::Thin;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="FogMS Weather", meta=(ClampMin="0.01", ClampMax="10.0", UIMin="0.05", UIMax="2.0", ToolTip="Owner decision 4 (pending): 1 = physical scale (bases 0.5-2.5 km, extinction 0.02-0.1 1/m, domain 20 km). A 'diorama' value below 1 multiplies every length (altitudes, domain, detail tile, wind speed) by it and divides the extinction by it, so optical depths and shadow darkness stay the same (0.1: clouds at 100-250 m, a 2 km domain)."))
	float WeatherScale = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="FogMS Weather", meta=(DisplayName="Create Cloud Host", ToolTip="Without a cloud host in the level, create one (the FogMS.CloudHost.Create path) for the weather shadows. Only one Volumetric Cloud renders per scene: the host displaces sky clouds. A host created here is deleted with this actor unless a Box renders through it."))
	bool bCreateCloudHost = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category="FogMS Weather", meta=(ClampMin="2.0", ClampMax="64.0", Units="km", ToolTip="Side of the weather domain (one tile of the weather pattern, centred on this actor; it repeats beyond). 512 texels: 20 km = 39 m per texel."))
	float DomainSizeKm = 20.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category="FogMS Weather", meta=(ClampMin="0.2", ClampMax="20.0", Units="km", ToolTip="Tile of the detail noise (the weather pattern at a higher frequency, curl-warped by height), rounded so it divides the domain."))
	float DetailTileKm = 2.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category="FogMS Weather", meta=(ClampMin="0.0", ClampMax="0.5", ToolTip="Height-dependent curl warp of the detail noise, in detail tiles over the layer height."))
	float CurlStrength = 0.05f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category="FogMS Weather", meta=(ClampMin="0.005", ClampMax="0.2", ToolTip="Width of the coverage threshold band in pattern units (the soft edge of the cloud field in the map)."))
	float CoverageEdge = 0.04f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category="FogMS Weather", meta=(ClampMin="1.0", ClampMax="200.0", Units="km", ToolTip="Cloud Shadow Extent the button / FogMS.Weather.SetupShadows gives the sun (the map's radius around the camera); the status warns when the sun's extent is smaller."))
	float ShadowExtentKm = 10.0f;

	/** Button: the sun's Cast Cloud Shadows on, Cloud Shadow Extent = Shadow Extent Km, resolution x2 (1024), shadow ray samples x1 (Thin)
	 * or x4 (Extended); one log line with the previous values, one undo step (UFogMSCloudHostSubsystem::SetupSunShadows). */
	UFUNCTION(BlueprintCallable, CallInEditor, Category="FogMS Weather", meta=(DisplayName="Setup Sun Shadows", ToolTip="Sets the atmosphere sun up for the weather shadows: Cast Cloud Shadows on, Cloud Shadow Extent = Shadow Extent Km (10), Cloud Shadow Map Resolution Scale 2 (1024 texels: 19.5 m at 10 km), Cloud Shadow Ray Sample Count Scale 1 with the Thin layer (the engine's 16 samples through the hero band) or 4 with the Extended layer (64 through the tall layer; the shadow pass costs in proportion). One log line with the previous values; Ctrl+Z undoes it. Nothing is saved."))
	void SetupSunShadows();

	/** Switches to NewState over Seconds (<= 0: immediate). One log line 'weather: A -> B (N s)'. */
	UFUNCTION(BlueprintCallable, Category="FogMS Weather")
	void SetWeather(UFogMSWeatherState* NewState, float Seconds);

	UFUNCTION(BlueprintCallable, Category="FogMS Weather")
	void SetWeatherImmediate(UFogMSWeatherState* NewState) { SetWeather(NewState, 0.0f); }

	UFUNCTION(BlueprintPure, Category="FogMS Weather")
	FString GetWeatherStatus() const { return WeatherStatus; }

	/** The plugin's preset asset (DA_FogMS_Weather_<Preset>), or a transient state with the built-in values when it is missing. */
	static UFogMSWeatherState* FindPresetState(EFogMSWeatherPreset Preset);

	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Transient, Category="FogMS Weather", meta=(DisplayName="Weather Status"))
	FString WeatherStatus;

	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Transient, Category="FogMS Weather", meta=(DisplayName="Current Values", ToolTip="The blended state in effect this tick (physical scale, before Weather Scale)."))
	FFogMSWeatherValues CurrentValues;

	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Transient, AdvancedDisplay, Category="FogMS Weather", meta=(ToolTip="Wind displacement of the weather field [cm], wrapped to the domain."))
	FVector2D WindOffset = FVector2D::ZeroVector;

	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Transient, AdvancedDisplay, Category="FogMS Weather", meta=(ToolTip="How often RT_FogMS_WeatherMap was drawn (it is redrawn only when the blended state changes)."))
	int32 MapDrawCount = 0;

	UPROPERTY(Transient, DuplicateTransient, VisibleInstanceOnly, AdvancedDisplay, Category="FogMS Weather", meta=(ToolTip="RT_FogMS_WeatherMap: R coverage L0, G type L0, B storm, A deck L1 over the weather domain."))
	TObjectPtr<UTextureRenderTarget2D> WeatherMap;

	UPROPERTY(Transient, DuplicateTransient, VisibleInstanceOnly, AdvancedDisplay, Category="FogMS Weather", meta=(ToolTip="RT_FogMS_WeatherSun (thin layer): optical depth of the weather column along the sun per ground point."))
	TObjectPtr<UTextureRenderTarget2D> WeatherSunMap;

	/** The cloud host this actor spawned (saved with the level), deleted with the actor when no Box renders through it. */
	UPROPERTY(VisibleInstanceOnly, AdvancedDisplay, Category="FogMS Weather")
	TObjectPtr<AActor> CreatedCloudHost;

	/** Optional overrides of the plugin assets (empty = /MultiLobeSpec/FogMS/Weather/...). */
	UPROPERTY(EditAnywhere, AdvancedDisplay, Category="FogMS Weather|Assets")
	TObjectPtr<UMaterialInterface> ComposeMaterial;
	UPROPERTY(EditAnywhere, AdvancedDisplay, Category="FogMS Weather|Assets")
	TObjectPtr<UMaterialInterface> SunMaterial;
	UPROPERTY(EditAnywhere, AdvancedDisplay, Category="FogMS Weather|Assets")
	TObjectPtr<UTexture2D> PatternTexture;
	UPROPERTY(EditAnywhere, AdvancedDisplay, Category="FogMS Weather|Assets")
	TObjectPtr<UTexture2D> CurlTexture;
	UPROPERTY(EditAnywhere, AdvancedDisplay, Category="FogMS Weather|Assets")
	TObjectPtr<UTexture2D> TypeLUT;

private:
	UPROPERTY(VisibleAnywhere, Category="FogMS Weather")
	TObjectPtr<USceneComponent> Root;
	UPROPERTY(Transient, DuplicateTransient)
	TObjectPtr<UMaterialInstanceDynamic> ComposeMID;
	UPROPERTY(Transient, DuplicateTransient)
	TObjectPtr<UMaterialInstanceDynamic> SunMID;

	void UpdateWeather(float DeltaSeconds);
	/** Materials, textures, MIDs, render targets, the RGBA write check. False with OutProblem when something is missing (retried on disk
	 * at most every 2 s). */
	bool EnsureResources(FString& OutProblem);
	bool EnsureResourcesInner(FString& OutProblem);
	double LastResourceAttempt = -1.0e9;
	FString LastResourceProblem;
	/** The world stops seeing this weather (disabled, deleted, end of play); a host it created without a Box is deleted. */
	void StopFeeding(const TCHAR* Reason, bool bDeleteCreatedHost);
	FString StateName(const UFogMSWeatherState* State) const;

	/** Transition: From (a snapshot of the blend when it started) -> the live values of the target state. */
	FFogMSWeatherValues FromValues;
	FString FromName;
	FString ToName;
	float TransitionElapsed = 0.0f;
	float TransitionDuration = 0.0f;
	bool bHaveValues = false;
	bool bFeeding = false;
	/** The last drawn map inputs (redraw on change), and the layers / domain / sun direction of the last sun map. */
	FVector4f DrawnCompose = FVector4f(-1.0f, -1.0f, -1.0f, -1.0f);
	bool bMapValid = false;
	FVector DrawnSunDirection = FVector::ZeroVector;
	FLinearColor DrawnSunL0 = FLinearColor(-1.0f, -1.0f, -1.0f, -1.0f);
	FLinearColor DrawnSunL1 = FLinearColor(-1.0f, -1.0f, -1.0f, -1.0f);
	FLinearColor DrawnSunDomain = FLinearColor(-1.0f, -1.0f, -1.0f, -1.0f);
	bool bSunMapValid = false;
	/** The actor spawned (or tried to spawn) a cloud host in this session: never twice (a user who deletes it keeps it deleted). */
	bool bTriedSpawn = false;
	/** RGBA write check of the map: 0 not done, 1 passed, -1 failed (MapCheckProblem). */
	int32 MapCheck = 0;
	FString MapCheckProblem;
	FString LastLoggedProblem;
};
