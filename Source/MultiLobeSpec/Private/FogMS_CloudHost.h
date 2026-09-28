#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "UObject/StrongObjectPtr.h"
#include <atomic>
#include "FogMS_CloudHost.generated.h"

class AActor;
class AFogMSBoxVolume;
class IConsoleVariable;
class UActorComponent;
class UDirectionalLightComponent;
class UMaterial;
class UMaterialInstanceDynamic;
class UMaterialInterface;
class UTexture;
class UVolumetricCloudComponent;
class UWorld;
struct FPropertyChangedEvent;

/** W48 (FogMS_Weather_Design.md 3.3-3.6, 4.1): what a FogMS Weather actor (FogMS_Weather.h) hands the cloud host every tick. The weather's
 * density exists ONLY in the host's cloud shadow pass (M_FogMS_Cloud v3, Shadow Pass Switch): the engine's cloud shadow map (Beer shadow
 * map) then shadows the ground, the fog, Lumen and the atmosphere with the weather; the visible pass keeps drawing the hero Boxes only. */
struct FFogMSWeatherFeed
{
	bool bLightLocalClouds = false;
	/** W51 prototype: sample physical weather in the host's visible cloud pass. Off preserves W50. */
	bool bViewWeather = false;
	float ViewTraceDistanceKm = 6.0f;
	TWeakObjectPtr<const AActor> Owner;
	/** Some weather density exists (a layer with coverage > 0 and extinction > 0, and the weather map passed its RGBA write check). False
	 * (Clear, or the map is not usable): the shadow branch is off and nothing of the host changes. */
	bool bActive = false;
	/** Decision 2 fallback (AFogMSWeather::ShadowLayer = Thin): the host layer is NOT extended; the whole weather column along the sun
	 * (RT_FogMS_WeatherSun) is spread over the host layer in the shadow pass. */
	bool bThinLayer = false;
	/** Altitude envelope [cm above the SkyAtmosphere ground] of the active weather layers (both states of a transition, rounded outward):
	 * the extended host layer covers it. BaseCm = the lowest active base (the thin band without a hero Box sits just under it). */
	double BottomCm = 0.0;
	double TopCm = 0.0;
	double BaseCm = 0.0;
	/** Shadow-pass textures: RT_FogMS_WeatherMap (RGBA16F), RT_FogMS_WeatherSun (R16F, thin layer), the cloud-type LUT, the pattern
	 * (detail noise), the 2D curl. */
	TWeakObjectPtr<UTexture> Map;
	TWeakObjectPtr<UTexture> SunMap;
	TWeakObjectPtr<UTexture> TypeLUT;
	TWeakObjectPtr<UTexture> Pattern;
	TWeakObjectPtr<UTexture> Curl;
	/** M_FogMS_Cloud v3 parameters (FogMS_WeatherFn): origin of the weather domain [cm]; (1 / domain [1/cm], 1 / detail tile [1/cm], curl
	 * strength, detail mip); wind displacement [cm] (xy); L0 = (base, top [cm], sigma [1/m], detail strength); L1 = (deck base, top [cm],
	 * sigma [1/m], 0). FogMS_WeatherSunDir (toward the sun, host layer height) is written by the subsystem. */
	FLinearColor Origin = FLinearColor(0, 0, 0, 0);
	FLinearColor Domain = FLinearColor(0, 0, 0, 0);
	FLinearColor Wind = FLinearColor(0, 0, 0, 0);
	FLinearColor L0 = FLinearColor(0, 0, 0, 0);
	FLinearColor L1 = FLinearColor(0, 0, 0, 0);
	uint64 FedFrame = 0;
};

/** P2 Render Path = Cloud Host: where a Box's density can be (FogMS_PerPixelClouds_Design.md 3.9, layer check / fit). The density
 * cube's transform (cube space -50..50, the LocalPosition of M_FogMS_Density / M_FogMS_Cloud) and the cube-space z range that can
 * hold density (the height-profile band when the profile provably zeroes the rest, else the whole cube). */
struct FFogMSCloudBoxGeometry
{
	FTransform CubeToWorld = FTransform::Identity;
	float LocalZMin = -50.0f;
	float LocalZMax = 50.0f;
};

/** Result of UFogMSCloudHostSubsystem::AcquireHost for one Box update. */
struct FFogMSCloudHostBinding
{
	/** The host's MID this Box writes its parameters into; null = no usable host (the Box stays in the froxels, see Status). */
	UMaterialInstanceDynamic* MID = nullptr;
	/** Status suffix: "[render: cloud host] [...]", "[cloud host: none, froxel fallback]" or "[cloud host: <problem>, froxel fallback]". */
	FString Status;
};

/**
 * P2 cloud host (FogMS_PerPixelClouds_Design.md sections 3-4): a Box with Render Path = Cloud Host (the class default since W46)
 * renders through the engine's Volumetric Cloud instead of the volumetric-fog froxels. Data flow per Box update (game thread,
 * AFogMSBoxVolume::UpdateDensity: every Box tick and every view family):
 *   AcquireHost(Box, band) -> the host = a rendering UVolumetricCloudComponent of this world whose material's base material is
 *     /MultiLobeSpec/FogMS/M_FogMS_Cloud (the plugin never creates one on its own: only one Volumetric Cloud renders per scene and a
 *     silent host would displace the sky cloud; 'Create Cloud Host' / FogMS.CloudHost.Create do it on request; without a host the
 *     status tells the user to click it);
 *   checks: material (Volume domain, not Unlit, 'Used with Volumetric Cloud', editor: Volumetric Advanced Output octaves 0), one Box
 *     per host (P3 adds a table);
 *   the host's material is wrapped once in a MID (outer = the cloud component, so a level saved with the host keeps it and a
 *     reload reuses it); the Box writes the same FogMS_* parameters as its own M_FogMS_Density MID into it every update (so the
 *     cube placement FogMS_CloudBoxCenter / FogMS_CloudWorldToLocal0..2, FogMS_WorldExtent and the height-profile band follow any
 *     move, scale or edit of the Box on the next update), plus FogMS_CloudPrefilter / FogMS_PrefilterWavelengths (W46 anti-dither),
 *     and sets its own FogMS_FroxelWeight 0; the subsystem writes FogMS_CloudStep (the host's nominal ray-march step, cm);
 *   W46 host settings owned by the plugin, applied in AcquireHost (CPU only): View Sample Count Scale = r.FogMS.CloudHost.ViewSampleScale
 *     (> 0), and the layer (Layer Bottom Altitude / Layer Height) refitted to the Box's density band +-10 m (the Create Cloud Host
 *     rule) whenever the target differs from the current layer by more than 5 m (hysteresis: a Box moved or scaled by less keeps the
 *     layer, and the band always stays >= 5 m inside it); r.FogMS.CloudHost.FitLayer 0 = the author owns the layer (check only);
 *     then the layer must cover the band (altitude above the SkyAtmosphere ground), else froxel fallback with the reason.
 *   Tick (after the actor ticks): a host whose Box stopped feeding it goes empty (FogMS_Density 0 = no density, conservative
 *     density 0). In editor worlds a frame gap first revalidates the owner through UpdateDensity: an idle/background viewport
 *     keeps its binding and settings; hidden, disabled or invalid Boxes still release it. Game/PIE retain the frame heartbeat.
 *     While a Box renders through a host, other rendering Volumetric Clouds are displaced: whenever one of them is
 *     added, shown or re-registered (MarkRenderStateDirty, editor property edits), the host's render state is marked dirty the
 *     next frame so the engine's cloud stack has the host on top again (FScene renders the most recently added cloud).
 *   Engine settings (ApplyHostSettings, every tick while a Box renders through a host; each at game-setting priority, an explicit
 *     project/ini/console value wins and is reported as 'kept'; the previous values come back when no Box uses a host). W46
 *     defaults = the owner's anti-dither combination (2026-09-26, judged by eye); every one is a plugin cvar, negative = leave the
 *     engine cvar alone:
 *     r.FogMS.CloudHost.StepSettings 1: r.VolumetricCloud.DistanceToSampleMaxCount = the host's Tracing Max Distance (uniform step =
 *       distance / samples) and r.VolumetricCloud.ViewRaySampleMaxCount = 96 x View Sample Count Scale when that exceeds 768;
 *     near settings (camera inside or within r.FogMS.CloudHost.NearDistanceKm of a hosted Box; 0 = always near):
 *       r.VolumetricRenderTarget.Mode = r.FogMS.CloudHost.RTMode (3: full-resolution tracing, no reconstruction),
 *       r.VolumetricCloud.SampleMinCount = r.FogMS.CloudHost.SampleMinCount (32);
 *     far settings (beyond the distance, 10 % hysteresis): .FarRTMode (1: half resolution), .FarSampleMinCount (8);
 *     always: r.VolumetricRenderTarget.UpsamplingMode = .UpsamplingMode (2; the engine forces 2 in modes 2/3 anyway),
 *       .ReprojectionBoxConstraint = .ReprojectionBoxConstraint (1), .MinimumDistanceKmToEnableReprojection = .ReprojectionMinKm (4)
 *       (the reprojection pair only acts in modes 0/2, which reconstruct over frames).
 *     The camera = the world's view locations rendered last frame (UWorld::ViewLocationsRenderedLastFrame: every perspective
 *     editor viewport / game view), distance to the Box's density band box.
 *   W47 cloud shadow map (P4 part 1, FogMS_Weather_Design.md 4.1): the host is a Volumetric Cloud, so with the atmosphere sun's
 *     Cast Cloud Shadows on the engine builds its Beer shadow map from the host's material = the Box's density, and the ground,
 *     the height fog / froxel media, Lumen and the atmosphere get the Box's shadow (the FogMS solver keeps shadowing its sun with the
 *     Box's own medium only: FogMS_WorldSources.cpp explains why nothing is counted twice). While a Box renders through a host AND
 *     that sun casts cloud shadows (Cast Cloud Shadows, Cloud Shadow Strength > 0), ApplyHostSettings also keeps
 *       r.VolumetricCloud.ShadowMap.SpatialFiltering = r.FogMS.CloudHost.ShadowSpatialFiltering (2: blur iterations, the soft edge),
 *       r.VolumetricCloud.ShadowMap.SnapLength = r.FogMS.CloudHost.ShadowSnapFraction (0.25) x the sun's Cloud Shadow Extent (km,
 *         at most the engine's 20 km: the map is centred on the camera in steps of this length, so with a small extent the camera
 *         must never be more than a quarter of it off centre) and r.VolumetricCloud.ShadowMap.SnapToPixelGrid 1 (no shimmer with
 *         the small snap),
 *     same priority and restore rules as above. The host status gets ' [cloud shadow: extent E km, res R, texel T m, filter F]'
 *     (texel = 2 x extent / resolution, resolution = 512 x Cloud Shadow Map Resolution Scale up to
 *     r.VolumetricCloud.ShadowMap.MaxResolution), a hint when the sun's Cast Cloud Shadows is off and a warning when the texel is
 *     larger than the Box. Console FogMS.CloudHost.SetupShadows [ExtentKm 5] [ResolutionScale 2] (SetupSunShadows) sets the sun up
 *     on request, one log line with the previous values (an undo step in the editor); the plugin never edits the sun on its own.
 *   W48 weather (FogMS_Weather_Design.md 3.2-3.6, 4.1; AFogMSWeather, FogMS_Weather.h): the weather actor feeds FeedWeather every tick.
 *     Tick (TickWeather): the weather uses the host a Box renders through, else the first rendering host; none -> WeatherNeedsHost (the
 *     actor spawns one with SpawnHost, the FogMS.CloudHost.Create path, when its Create Cloud Host is ticked). Its MID (the same wrap as a
 *     Box's) gets the M_FogMS_Cloud v3 weather parameters every tick; the material adds the weather density to the extinction and the
 *     conservative density ONLY in the cloud shadow pass (Shadow Pass Switch; SHADOW_DEPTH_SHADER 1 only in FVolumetricCloudShadowPS), so
 *     the engine's cloud shadow map (and sky AO) carries hero Boxes + weather and the visible pass the hero Boxes only.
 *     Extended layer: the host layer = the Box's band +-10 m UNION the weather envelope (FitLayer); while a Box renders through that
 *     extended host, r.VolumetricCloud.StepSizeOnZeroConservativeDensity = r.FogMS.Weather.SkipSteps (8; empty view steps are skipped 8
 *     at a time) and FogMS_CloudSkipMargin = that value x the host step widens the Box's conservative region by as much (a skip never
 *     jumps over the Box's entry; the sample grid is unchanged). Round 48 measured +1.04 ms in the visible pass at the owner camera
 *     (gate 0.3 ms), so the actor's default is the Thin layer (decision 2): the layer stays at the Box's band (without a Box: a 0.1 km
 *     band right under the weather base) and the shadow pass spreads the weather column's optical depth along the sun
 *     (RT_FogMS_WeatherSun) over it: the ground below gets exp(-OD), the air above the band gets no weather shadow.
 *     No Box renders through the weather's host: 'shadows only': FogMS_Density 0 (no hero), the layer fitted to the weather (thin: the
 *     band under its base), Tracing Start Distance = Tracing Max Distance (the view trace is empty: minimal visible cost; a Box binding
 *     puts 0 back), and of the managed cvars only the W47 cloud-shadow-map ones (while the sun casts cloud shadows). A host the weather
 *     took without a Box keeps its previous layer and start distance in FHost and gets them back when the weather stops (actor deleted,
 *     disabled, another world); a hero host just refits to its Box. Clear (no active layer): the weather branch is off and nothing of the
 *     host changes (identical to W47). One weather actor per world (ClaimWeather: the first one feeding wins).
 */
UCLASS()
class UFogMSCloudHostSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	/** W46: number of engine cvars ApplyHostSettings manages (FogMS_CloudHost.cpp FogMS_ManagedCVarNames); W47 adds the three
	 * cloud-shadow-map cvars, W48 r.VolumetricCloud.StepSizeOnZeroConservativeDensity. */
	static constexpr int32 ManagedCount = 11;

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool IsTickableInEditor() const override { return true; }

	/** Game thread, from AFogMSBoxVolume::UpdateDensity (several times a frame: tick and every view family). */
	FFogMSCloudHostBinding AcquireHost(const AFogMSBoxVolume& Box, const FFogMSCloudBoxGeometry& Geometry);
	/** The Box no longer renders through a host (Render Path, fallback, disabled, destroyed): its host goes empty now. */
	void ReleaseBox(const AFogMSBoxVolume& Box);

	/** Editor button 'Create Cloud Host' and console FogMS.CloudHost.Create: spawns an AVolumetricCloud labelled 'FogMS Cloud Host'
	 * with MI_FogMS_Cloud and the P1 settings (layer fitted to Box's density band +-10 m, trace 2 km from the camera, view samples
	 * r.FogMS.CloudHost.ViewSampleScale (8), sun march 0.25 km x 32, not in real-time sky captures). An existing host is returned
	 * instead. bOutSpawned distinguishes newly owned actors from borrowed existing hosts. Nothing is saved. Afterwards the layer follows
	 * the Box (AcquireHost). */
	static AActor* SpawnHost(UWorld* World, const AFogMSBoxVolume* Box, FString& OutMessage, bool* bOutSpawned = nullptr);
	/** /MultiLobeSpec/FogMS/M_FogMS_Cloud (null when the asset is missing). */
	static UMaterial* GetCloudHostMaterial();
	/** W47 console FogMS.CloudHost.SetupShadows: the atmosphere sun of World (the engine's choice: the brightest visible directional
	 * light with Atmosphere Sun Light, index 0) gets Cast Cloud Shadows on, Cloud Shadow Extent = ExtentKm (the map's radius around
	 * the camera) and Cloud Shadow Map Resolution Scale = ResolutionScale, as one undo step in the editor. OutMessage = the one log
	 * line: sun, previous -> new values, map resolution and texel size. False when there is no such sun (nothing changed). */
	static bool SetupSunShadows(UWorld* World, float ExtentKm, float ResolutionScale, FString& OutMessage, float RaySampleScale = -1.0f);
	/** W47: the world's atmosphere sun as the renderer picks it (FScene::AtmosphereLights[0]: the brightest visible, world-affecting
	 * directional light with Atmosphere Sun Light and index 0); OutCount = how many qualify. Null when none. */
	static UDirectionalLightComponent* FindAtmosphereSun(const UWorld* World, int32* OutCount = nullptr);
	/** Planet shared by the native cloud layer and the weather lighting snapshot (cm). */
	static void GetWeatherPlanet(const UWorld* World, FVector& OutCenter, double& OutRadius);

	/** W48: one weather actor per world. True when Owner drives the weather (it already does, or nobody fed in the last frame);
	 * OutOther = the label of the actor that does otherwise. */
	bool ClaimWeather(const AActor& Owner, FString& OutOther);
	/** W48, the weather actor's tick (game thread): this frame's feed (FFogMSWeatherFeed); applied by the subsystem tick. */
	void FeedWeather(const FFogMSWeatherFeed& Feed);
	/** W48: the weather actor stops (deleted, disabled, end of play): the weather branch goes off, a host it took without a Box gets its
	 * layer and start distance back now, the cvars on the next tick. One log line. */
	void StopWeather(const AActor& Owner, const TCHAR* Reason);
	/** W48: the fed weather found no cloud host in this world (none exists, not even a hidden one). */
	bool WeatherNeedsHost() const { return bWeatherNeedsHost; }
	/** True only after a live host has accepted the W51 visible-weather feed. */
	bool IsWeatherViewActive() const { return bWeatherHostUsable && WeatherFeed.bActive && WeatherFeed.bViewWeather; }
	/** W48: the host part of the weather actor's status (from the last tick). */
	const FString& GetWeatherNote() const { return WeatherNote; }
	/** W48: a Box renders through the cloud component of HostActor (its hero part is in use). */
	bool IsHostBoundToBox(const AActor* HostActor) const;

private:
	struct FHost
	{
		TWeakObjectPtr<UVolumetricCloudComponent> Component;
		TWeakObjectPtr<UMaterialInstanceDynamic> MID;
		TWeakObjectPtr<const AFogMSBoxVolume> Owner;
		uint64 FedFrame = 0;
		/** A Box renders through this host (its MID carries that Box's parameters); false = emptied (FogMS_Density 0). */
		bool bBound = false;
		/** W46 layer refits: wall-clock time of the last log line and refits since then (one line per second at most). */
		double LastRefitLogTime = -1.0e9;
		int32 RefitsSinceLog = 0;
		/** W46: the bound Box's density band box of its last AcquireHost (camera distance for the near/far settings). */
		FFogMSCloudBoxGeometry Geometry;
		/** W47: the cloud-shadow part of the last status (logged once whenever it changes). */
		FString LastShadowNote;
		/** W48: the weather writes its shadow-pass parameters into this host's MID. */
		bool bWeather = false;
		/** W48: this host renders weather shadows only (no Box): its Tracing Start Distance is the plugin's 'empty view trace'. */
		bool bShadowsOnly = false;
		/** W48: the layer and start distance the host had before the weather managed it without a Box (restored when the weather stops). */
		bool bWeatherSaved = false;
		/** Weather-only claims use a private MID so even a borrowed dynamic material keeps all authored overrides unchanged.
		 * FHost is not a reflected struct: this strong reference explicitly keeps the original alive until restore or Box adoption. */
		TStrongObjectPtr<UMaterialInterface> SavedWeatherMaterial;
		TWeakObjectPtr<UMaterialInstanceDynamic> WeatherMID;
		/** A user's replacement suspends automatic weather material acquisition until weather stops/Clear or a Box adopts the host. */
		bool bWeatherMaterialReplaced = false;
		/** W51: restore settings borrowed from a host when the visible-weather experiment ends. */
		bool bWeatherViewSaved = false;
		bool bSavedCaptureVisibility = false;
		float SavedViewTraceDistanceKm = 0.0f;
		/** W51c: the visible-weather sun-march scale belongs only to this host; restore the authored value on exit. */
		float SavedShadowViewSampleCountScale = 0.0f;
		float LastWeatherShadowViewSampleCountScale = 0.0f;
		bool bWeatherShadowScaleManaged = false;
		bool bWeatherShadowScaleUserOverride = false;
		float SavedLayerBottomKm = 0.0f;
		float SavedLayerHeightKm = 0.1f;
		float SavedStartDistanceKm = 0.0f;
	};

	/** W46: one managed engine cvar (ApplyHostSettings): its value before the first write, the value last asked for. */
	struct FManagedSetting
	{
		bool bApplied = false;
		float Previous = 0.0f;
		float Wanted = 0.0f;
	};

	/** Once per frame: cloud components of this world: rendering hosts (base material M_FogMS_Cloud), registered hosts that do not
	 * render (hidden, invisible: named in the fallback status) and other rendering clouds. */
	void Scan();
	FString ValidateHostMaterial(const UVolumetricCloudComponent& Component) const;
	/** W46: fits the host layer to the Box's density band (r.FogMS.CloudHost.FitLayer, hysteresis 5 m; sets bInOutDirty when it changed
	 * the component) and checks that it covers the band. Returns the problem (empty = covered); OutNote = the layer for the status. */
	FString FitLayer(UVolumetricCloudComponent& Component, const FFogMSCloudBoxGeometry& Geometry, const AFogMSBoxVolume& Box, FHost& Host,
		bool& bInOutDirty, FString& OutNote);
	FHost* FindBinding(const UVolumetricCloudComponent* Component);
	/** W47: ' [cloud shadow: ...]' for the host status of a bound Box (Geometry: the Box's size for the texel warning), from ScanSun
	 * and the engine's cloud-shadow-map cvars; empty when there is nothing to say. */
	FString CloudShadowNote(const FFogMSCloudBoxGeometry& Geometry) const;
	void EmptyHost(FHost& Host, const TCHAR* Reason);
	/** W46: the managed engine cvars (class comment) for the host a Box renders through (bHero) or the weather uses without a Box (only
	 * the W47 cloud-shadow-map cvars then); W48: StepSizeOnZeroConservativeDensity while bHero and the weather extends the layer. One log
	 * line when a value changes. Writes FogMS_CloudSkipMargin (the Box's conservative-region margin for that skip) into the host MID. */
	void ApplyHostSettings(UVolumetricCloudComponent& Host, bool bHero, bool bWeatherActive, bool bWeatherExtended);
	/** The host's MID: its material when that already is a MID, else a new MID of it (outer = the component) set on it. Null (with
	 * OutProblem) when that fails. Context = who asks, for the log line. */
	UMaterialInstanceDynamic* EnsureHostMID(UVolumetricCloudComponent& Component, const FString& Context, FString& OutProblem);
	/** W48: the fresh weather feed that extends a host layer (active, extended mode), else null. */
	const FFogMSWeatherFeed* ExtendingWeather() const;
	/** W48 subsystem tick part: host choice, MID parameters, layer and 'shadows only' state without a Box. Returns the weather's host. */
	UVolumetricCloudComponent* TickWeather();
	/** W48: writes the weather parameters (bOn false: the branch off, textures kept) into a host MID. */
	void WriteWeatherParameters(UMaterialInstanceDynamic& MID, const UVolumetricCloudComponent& Component, bool bOn) const;
	/** W48: the weather leaves Host: branch off, saved layer / start distance back (a host without a Box). Reason for the log line. */
	void ReleaseWeatherHost(FHost& Host, const TCHAR* Reason);
	/** Every managed cvar back to its previous value (only those still at game-setting priority); one log line. */
	void RestoreHostSettings();
	void RestoreSetting(int32 Index, IConsoleVariable* Variable, FString& InOutChanges);
	void OnMarkRenderStateDirty(UActorComponent& Component);
#if WITH_EDITOR
	void OnObjectPropertyChanged(UObject* Object, FPropertyChangedEvent& Event);
#endif

	TArray<FHost> Hosts;
	uint64 ScanFrame = MAX_uint64;
	TArray<TWeakObjectPtr<UVolumetricCloudComponent>> ScanHosts;
	TArray<TWeakObjectPtr<UVolumetricCloudComponent>> ScanIdle;
	TArray<TWeakObjectPtr<UVolumetricCloudComponent>> ScanOthers;
	/** W47: this frame's atmosphere sun (FindAtmosphereSun, refreshed by Scan) for the cloud-shadow settings and status. */
	TWeakObjectPtr<UDirectionalLightComponent> ScanSun;
	/** Other rendering clouds when the last claim ran; a different set (or a dirty event) re-claims the render. */
	TArray<TWeakObjectPtr<UVolumetricCloudComponent>> ClaimedOver;
	std::atomic<bool> bOthersDirty{ false };
	FManagedSetting Managed[ManagedCount];
	/** W46 near/far settings state (r.FogMS.CloudHost.NearDistanceKm, hysteresis) and the camera distance [km] it last used. */
	bool bNearSettings = true;
	double NearestViewKm = -1.0;
	/** For the Box status: the render-target mode in effect and why (', rt mode 3 near'), then the managed cvars an explicit value
	 * keeps from their wanted value (', <cvar> <value> kept (<SetBy>)'). */
	FString SettingsNote;
	FDelegateHandle MarkDirtyHandle;
	FDelegateHandle PropertyChangedHandle;
	/** W48 weather state: the last feed (FeedWeather), the tick's result for the actor's status. */
	FFogMSWeatherFeed WeatherFeed;
	bool bWeatherNeedsHost = false;
	/** W48: the last TickWeather bound the weather to a host with the v3 material (so the layer may be extended). */
	bool bWeatherHostUsable = false;
	FString WeatherNote;
	/** W48: the last weather log line of the tick (host choice / problem), logged once whenever it changes. */
	FString LastWeatherLog;
};
