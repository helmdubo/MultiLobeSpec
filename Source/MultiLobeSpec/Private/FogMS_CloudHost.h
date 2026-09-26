#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include <atomic>
#include "FogMS_CloudHost.generated.h"

class AActor;
class AFogMSBoxVolume;
class IConsoleVariable;
class UActorComponent;
class UMaterial;
class UMaterialInstanceDynamic;
class UVolumetricCloudComponent;
struct FPropertyChangedEvent;

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
 *     density 0); while a Box renders through a host, other rendering Volumetric Clouds are displaced: whenever one of them is
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
 */
UCLASS()
class UFogMSCloudHostSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	/** W46: number of engine cvars ApplyHostSettings manages (FogMS_CloudHost.cpp FogMS_ManagedCVarNames). */
	static constexpr int32 ManagedCount = 7;

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
	 * instead. Nothing is saved. Afterwards the layer follows the Box (AcquireHost). */
	static AActor* SpawnHost(UWorld* World, const AFogMSBoxVolume* Box, FString& OutMessage);
	/** /MultiLobeSpec/FogMS/M_FogMS_Cloud (null when the asset is missing). */
	static UMaterial* GetCloudHostMaterial();

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
	void EmptyHost(FHost& Host, const TCHAR* Reason);
	/** W46: the managed engine cvars (class comment) for the host a Box renders through; one log line when a value changes. */
	void ApplyHostSettings(const UVolumetricCloudComponent& Host);
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
};
