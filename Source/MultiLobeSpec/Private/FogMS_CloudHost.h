#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include <atomic>
#include "FogMS_CloudHost.generated.h"

class AActor;
class AFogMSBoxVolume;
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
 * P2 cloud host (FogMS_PerPixelClouds_Design.md sections 3-4): a Box with Render Path = Cloud Host renders through the engine's
 * Volumetric Cloud instead of the volumetric-fog froxels. Data flow per Box update (game thread, AFogMSBoxVolume::UpdateDensity):
 *   AcquireHost(Box, band) -> the host = a rendering UVolumetricCloudComponent of this world whose material's base material is
 *     /MultiLobeSpec/FogMS/M_FogMS_Cloud (the plugin never creates one on its own: only one Volumetric Cloud renders per scene and a
 *     silent host would displace the sky cloud; 'Create Cloud Host' / FogMS.CloudHost.Create do it on request);
 *   checks: material (Volume domain, not Unlit, 'Used with Volumetric Cloud', editor: Volumetric Advanced Output octaves 0), host
 *     layer covers the Box's density band (altitude above the SkyAtmosphere ground), one Box per host (P3 adds a table);
 *   the host's material is wrapped once in a MID (outer = the cloud component, so a level saved with the host keeps it and a
 *     reload reuses it); the Box writes the same FogMS_* parameters as its own M_FogMS_Density MID into it every update, plus the
 *     cube placement (FogMS_CloudBoxCenter, FogMS_CloudWorldToLocal0..2), and sets its own FogMS_FroxelWeight 0.
 *   Tick (after the actor ticks): a host whose Box stopped feeding it goes empty (FogMS_Density 0 = no density, conservative
 *     density 0); while a Box renders through a host, other rendering Volumetric Clouds are displaced: whenever one of them is
 *     added, shown or re-registered (MarkRenderStateDirty, editor property edits), the host's render state is marked dirty the
 *     next frame so the engine's cloud stack has the host on top again (FScene renders the most recently added cloud).
 *   Step settings (r.FogMS.CloudHost.StepSettings 1): while a Box renders through a host, r.VolumetricCloud.DistanceToSampleMaxCount
 *     = the host's Tracing Max Distance (uniform step = distance / samples) and r.VolumetricCloud.SampleMinCount 8, at game-setting
 *     priority (an explicit project/ini/console value wins and is reported); the previous values come back when no Box uses a host.
 */
UCLASS()
class UFogMSCloudHostSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
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
	 * x8, sun march 0.25 km x 32, not in real-time sky captures). An existing host is returned instead. Nothing is saved. */
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
	};

	/** Once per frame: rendering cloud components of this world, split into hosts (base material M_FogMS_Cloud) and others. */
	void Scan();
	FString ValidateHostMaterial(const UVolumetricCloudComponent& Component) const;
	FString CheckLayer(const UVolumetricCloudComponent& Component, const FFogMSCloudBoxGeometry& Geometry) const;
	FHost* FindBinding(const UVolumetricCloudComponent* Component);
	void EmptyHost(FHost& Host, const TCHAR* Reason);
	void ApplyStepSettings(const UVolumetricCloudComponent* Host);
	void RestoreStepSettings();
	void OnMarkRenderStateDirty(UActorComponent& Component);
#if WITH_EDITOR
	void OnObjectPropertyChanged(UObject* Object, FPropertyChangedEvent& Event);
#endif

	TArray<FHost> Hosts;
	uint64 ScanFrame = MAX_uint64;
	TArray<TWeakObjectPtr<UVolumetricCloudComponent>> ScanHosts;
	TArray<TWeakObjectPtr<UVolumetricCloudComponent>> ScanOthers;
	/** Other rendering clouds when the last claim ran; a different set (or a dirty event) re-claims the render. */
	TArray<TWeakObjectPtr<UVolumetricCloudComponent>> ClaimedOver;
	std::atomic<bool> bOthersDirty{ false };
	bool bStepSettingsApplied = false;
	float PreviousDistanceToSampleMaxCount = 15.0f;
	int32 PreviousSampleMinCount = 2;
	FDelegateHandle MarkDirtyHandle;
	FDelegateHandle PropertyChangedHandle;
};
