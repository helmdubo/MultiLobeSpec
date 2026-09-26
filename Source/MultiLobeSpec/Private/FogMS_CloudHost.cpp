#include "FogMS_CloudHost.h"
#include "FogMS_BoxVolume.h"

#include "Components/ActorComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/VolumetricCloudComponent.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "MultiLobeSpec.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectHash.h"
#if WITH_EDITORONLY_DATA
#include "Materials/MaterialExpressionVolumetricAdvancedMaterialOutput.h"
#endif

namespace
{
	const TCHAR* const FogMS_CloudMaterialPath = TEXT("/MultiLobeSpec/FogMS/M_FogMS_Cloud.M_FogMS_Cloud");
	const TCHAR* const FogMS_CloudInstancePath = TEXT("/MultiLobeSpec/FogMS/MI_FogMS_Cloud.MI_FogMS_Cloud");
	const TCHAR* const FogMS_CloudHostLabel = TEXT("FogMS Cloud Host");

	TAutoConsoleVariable<int32> CVarCloudHostStepSettings(TEXT("r.FogMS.CloudHost.StepSettings"), 1,
		TEXT("While a FogMS Box renders through a cloud host (Render Path = Cloud Host): 1 (default) sets r.VolumetricCloud.DistanceToSampleMaxCount ")
		TEXT("to the host's Tracing Max Distance (km) at game-setting priority, so the host marches with a uniform step = distance / samples ")
		TEXT("(2 km / 768 = 2.6 m with the 'Create Cloud Host' settings) instead of the engine's 15 km / samples, and raises ")
		TEXT("r.VolumetricCloud.ViewRaySampleMaxCount to 96 x the host's View Sample Count Scale when that exceeds the engine's 768 ")
		TEXT("(r.FogMS.CloudHost.ViewSampleScale above 8); the previous values come back when no Box uses a host. A project/ini/console value has ")
		TEXT("priority and is kept (the Box status reports it). 0 = leave both cvars to the project. r.VolumetricCloud.SampleMinCount has its own ")
		TEXT("settings since W46 (r.FogMS.CloudHost.SampleMinCount / .FarSampleMinCount)."), ECVF_Default);
	// W46 host settings (FogMS_CloudHost.h class comment). Defaults = the owner's anti-dither combination, judged by eye 2026-09-26.
	TAutoConsoleVariable<int32> CVarCloudHostFitLayer(TEXT("r.FogMS.CloudHost.FitLayer"), 1,
		TEXT("1 (default): the layer of a cloud host (Layer Bottom Altitude / Layer Height) follows the Box it renders: the Box's density band ")
		TEXT("+-10 m above the SkyAtmosphere ground (at least 0.1 km high), refitted on the Box's next update whenever that target differs from the ")
		TEXT("current layer by more than 5 m (moving, scaling or re-profiling the Box; smaller changes keep the layer, the band stays >= 5 m inside ")
		TEXT("it). 0: the author owns the layer; a layer that does not cover the band sends the Box to the froxels with the reason in its status."),
		ECVF_Default);
	TAutoConsoleVariable<float> CVarCloudHostViewSampleScale(TEXT("r.FogMS.CloudHost.ViewSampleScale"), 8.0f,
		TEXT("View Sample Count Scale the plugin gives every cloud host a Box renders through (ray-march samples = 96 x this, step = distance / ")
		TEXT("samples): 8 (default) = 768 samples, 2.6 m at 2 km; 16 = 1536 samples, 1.3 m (less dither, about twice the trace cost; ")
		TEXT("r.VolumetricCloud.ViewRaySampleMaxCount is raised to match while r.FogMS.CloudHost.StepSettings is 1). 0 = keep the host's own value."),
		ECVF_Default);
	TAutoConsoleVariable<int32> CVarCloudHostRTMode(TEXT("r.FogMS.CloudHost.RTMode"), 3,
		TEXT("r.VolumetricRenderTarget.Mode while a Box renders through a cloud host and a camera is near a hosted Box (r.FogMS.CloudHost.")
		TEXT("NearDistanceKm). 3 (default, the owner's anti-dither choice): trace at full resolution, no temporal reconstruction: sharpest, no ")
		TEXT("trail, the most expensive (4x the rays of mode 1, 16x mode 0); 1: half resolution, no reconstruction; 0 (engine default): quarter ")
		TEXT("resolution reconstructed over 4 frames (cheapest; trails and dither on fast moves). Negative = leave the engine cvar alone."), ECVF_Default);
	TAutoConsoleVariable<int32> CVarCloudHostSampleMinCount(TEXT("r.FogMS.CloudHost.SampleMinCount"), 32,
		TEXT("r.VolumetricCloud.SampleMinCount while a Box renders through a cloud host and a camera is near a hosted Box: the least number of ")
		TEXT("ray-march steps per ray, so short rays (steep through a thin layer, the camera inside the cloud) step finer than distance / samples. ")
		TEXT("Default 32 (the owner's anti-dither choice; engine 2, round 45: 8). Negative = leave the engine cvar alone."), ECVF_Default);
	TAutoConsoleVariable<float> CVarCloudHostNearDistanceKm(TEXT("r.FogMS.CloudHost.NearDistanceKm"), 1.0f,
		TEXT("The near settings (r.FogMS.CloudHost.RTMode / .SampleMinCount) apply while a rendered camera is inside a hosted Box or within this ")
		TEXT("many km of its density band box; farther from every hosted Box the far settings (.FarRTMode / .FarSampleMinCount) apply, with 10 % ")
		TEXT("hysteresis (back to near below the distance, to far above 1.1 x it). Default 1 km. 0 = always the near settings."), ECVF_Default);
	TAutoConsoleVariable<int32> CVarCloudHostFarRTMode(TEXT("r.FogMS.CloudHost.FarRTMode"), 1,
		TEXT("r.VolumetricRenderTarget.Mode while every rendered camera is farther than r.FogMS.CloudHost.NearDistanceKm from the hosted Boxes. ")
		TEXT("Default 1 (half resolution, no reconstruction). Negative = leave the engine cvar alone."), ECVF_Default);
	TAutoConsoleVariable<int32> CVarCloudHostFarSampleMinCount(TEXT("r.FogMS.CloudHost.FarSampleMinCount"), 8,
		TEXT("r.VolumetricCloud.SampleMinCount while every rendered camera is far from the hosted Boxes. Default 8 (round 45). Negative = leave it ")
		TEXT("alone."), ECVF_Default);
	TAutoConsoleVariable<int32> CVarCloudHostUpsamplingMode(TEXT("r.FogMS.CloudHost.UpsamplingMode"), 2,
		TEXT("r.VolumetricRenderTarget.UpsamplingMode while a Box renders through a cloud host. Default 2 (nearest + depth test: the owner's choice; ")
		TEXT("render-target modes 2 and 3 force 2 anyway), engine default 4 (bilateral). Negative = leave the engine cvar alone."), ECVF_Default);
	TAutoConsoleVariable<int32> CVarCloudHostBoxConstraint(TEXT("r.FogMS.CloudHost.ReprojectionBoxConstraint"), 1,
		TEXT("r.VolumetricRenderTarget.ReprojectionBoxConstraint while a Box renders through a cloud host: 1 (default) clamps reprojected history ")
		TEXT("to this frame's neighbourhood (less trail). Acts only in the render-target modes that reconstruct over frames (0, 2). Negative = ")
		TEXT("leave the engine cvar alone."), ECVF_Default);
	TAutoConsoleVariable<float> CVarCloudHostReprojectionMinKm(TEXT("r.FogMS.CloudHost.ReprojectionMinKm"), 4.0f,
		TEXT("r.VolumetricRenderTarget.MinimumDistanceKmToEnableReprojection while a Box renders through a cloud host: clouds nearer than this ")
		TEXT("(km) use no history. Default 4 (the host draws within 2 km: no history). Acts only in modes 0 and 2. Negative = leave it alone."),
		ECVF_Default);

	// P1 host settings (round 39, FogMS_Prod_Report.md 'Раунд 39'): trace 2 km from the camera, view samples x8 (768), sun march 0.25 km
	// with 32 samples, stop at transmittance 0.005, layer = the Box's density band +-10 m (at least 0.1 km).
	constexpr float FogMS_HostTraceKm = 2.0f;
	constexpr float FogMS_HostViewSampleScale = 8.0f;
	constexpr float FogMS_HostShadowSampleScale = 3.2f;
	constexpr float FogMS_HostShadowTraceKm = 0.25f;
	constexpr float FogMS_HostStopTransmittance = 0.005f;
	constexpr double FogMS_HostLayerMarginCm = 1000.0;
	constexpr double FogMS_HostLayerMinHeightCm = 1.0e4;
	/** W46 refit hysteresis [cm]: the layer is refitted when its bottom or top is farther than this from the target (band +-10 m), so
	 * the band always stays at least 5 m inside the layer and a Box moved or scaled by less keeps it. */
	constexpr double FogMS_LayerRefitCm = 500.0;
	/** Layer check tolerance [cm]: a fitted host keeps >= 5 m of margin; this only absorbs float rounding. */
	constexpr double FogMS_LayerToleranceCm = 50.0;
	/** The engine's r.VolumetricCloud.ViewRaySampleMaxCount default (VolumetricCloudRendering.cpp). */
	constexpr float FogMS_EngineViewRaySampleMaxCount = 768.0f;

	/** W46: the engine cvars ApplyHostSettings manages, in UFogMSCloudHostSubsystem::Managed order. */
	enum EFogMSManagedCVar : int32
	{
		FogMS_MDistance, FogMS_MSampleMin, FogMS_MSampleMax, FogMS_MMode, FogMS_MUpsampling, FogMS_MBoxConstraint, FogMS_MMinKm, FogMS_MCount
	};
	static_assert(FogMS_MCount == UFogMSCloudHostSubsystem::ManagedCount, "FogMS_ManagedCVarNames and UFogMSCloudHostSubsystem::ManagedCount differ");
	const TCHAR* const FogMS_ManagedCVarNames[FogMS_MCount] =
	{
		TEXT("r.VolumetricCloud.DistanceToSampleMaxCount"),
		TEXT("r.VolumetricCloud.SampleMinCount"),
		TEXT("r.VolumetricCloud.ViewRaySampleMaxCount"),
		TEXT("r.VolumetricRenderTarget.Mode"),
		TEXT("r.VolumetricRenderTarget.UpsamplingMode"),
		TEXT("r.VolumetricRenderTarget.ReprojectionBoxConstraint"),
		TEXT("r.VolumetricRenderTarget.MinimumDistanceKmToEnableReprojection"),
	};

	/** Cached lookups (FindConsoleObject warns about frequent calls; these engine cvars exist from the renderer's start on). */
	IConsoleVariable* FogMS_ManagedCVar(int32 Index)
	{
		static IConsoleVariable* Cache[FogMS_MCount] = {};
		if (!Cache[Index]) Cache[Index] = IConsoleManager::Get().FindConsoleVariable(FogMS_ManagedCVarNames[Index], false);
		return Cache[Index];
	}

	/** The engine's condition for adding a cloud proxy to the scene (UVolumetricCloudComponent::CreateRenderState_Concurrent). */
	bool FogMS_CloudRenders(const UVolumetricCloudComponent* Component)
	{
		return Component && Component->IsRegistered() && Component->IsRenderStateCreated() && Component->GetVisibleFlag()
			&& Component->ShouldComponentAddToScene() && Component->ShouldRender();
	}

	UMaterial* FogMS_BaseMaterial(const UVolumetricCloudComponent* Component)
	{
		UMaterialInterface* Material = Component ? Component->Material.Get() : nullptr;
		return Material ? Material->GetMaterial() : nullptr;
	}

	FString FogMS_Label(const UVolumetricCloudComponent* Component)
	{
		return Component && Component->GetOwner() ? Component->GetOwner()->GetActorNameOrLabel() : FString(TEXT("?"));
	}

	/** Planet centre [cm] and ground radius [cm] the cloud layer is measured from (VolumetricCloudRendering.cpp: the SkyAtmosphere's
	 * planet when there is one, else the cloud's own Planet Radius with the planet top at the world origin; transform modes as
	 * SkyAtmosphereCommonData.cpp). FallbackRadiusKm = the cloud's Planet Radius. */
	void FogMS_CloudPlanet(const UWorld* World, double FallbackRadiusKm, FVector& OutCenter, double& OutRadius)
	{
		const USkyAtmosphereComponent* Sky = nullptr;
		ForEachObjectOfClass(USkyAtmosphereComponent::StaticClass(), [&Sky, World](UObject* Object)
		{
			const USkyAtmosphereComponent* Candidate = static_cast<const USkyAtmosphereComponent*>(Object);
			if (!Sky && Candidate->GetWorld() == World && Candidate->IsRegistered() && Candidate->ShouldRender()) Sky = Candidate;
		}, true, RF_ClassDefaultObject | RF_ArchetypeObject, EInternalObjectFlags::Garbage);
		if (Sky)
		{
			OutRadius = static_cast<double>(Sky->BottomRadius) * 1.0e5;
			const FVector Translation = Sky->GetComponentTransform().GetTranslation();
			switch (Sky->TransformMode)
			{
			case ESkyAtmosphereTransformMode::PlanetTopAtAbsoluteWorldOrigin: OutCenter = FVector(0.0, 0.0, -OutRadius); break;
			case ESkyAtmosphereTransformMode::PlanetTopAtComponentTransform: OutCenter = FVector(0.0, 0.0, -OutRadius) + Translation; break;
			default: OutCenter = Translation; break;
			}
			return;
		}
		OutRadius = FallbackRadiusKm * 1.0e5;
		OutCenter = FVector(0.0, 0.0, -OutRadius);
	}

	/** Altitude range [cm above the ground] of the Box's density band: the farthest point from the planet centre is a corner, the
	 * nearest is the centre clamped into the band box in cube space (the cube's axes are orthogonal: rotation x scale, no shear). */
	void FogMS_BandAltitudes(const FFogMSCloudBoxGeometry& Geometry, const FVector& Center, double Radius, double& OutMin, double& OutMax)
	{
		const FTransform& T = Geometry.CubeToWorld;
		OutMax = TNumericLimits<double>::Lowest();
		for (int32 Corner = 0; Corner < 8; ++Corner)
		{
			const FVector Local((Corner & 1) ? 50.0 : -50.0, (Corner & 2) ? 50.0 : -50.0, (Corner & 4) ? Geometry.LocalZMax : Geometry.LocalZMin);
			OutMax = FMath::Max(OutMax, (T.TransformPosition(Local) - Center).Size() - Radius);
		}
		FVector Nearest = T.InverseTransformPosition(Center);
		Nearest.X = FMath::Clamp(Nearest.X, -50.0, 50.0);
		Nearest.Y = FMath::Clamp(Nearest.Y, -50.0, 50.0);
		Nearest.Z = FMath::Clamp(Nearest.Z, static_cast<double>(Geometry.LocalZMin), static_cast<double>(Geometry.LocalZMax));
		OutMin = (T.TransformPosition(Nearest) - Center).Size() - Radius;
	}

	/** World distance [cm] from Location to the Box's density band box (0 inside); same per-axis clamp in cube space as above. */
	double FogMS_BandDistanceCm(const FFogMSCloudBoxGeometry& Geometry, const FVector& Location)
	{
		const FTransform& T = Geometry.CubeToWorld;
		FVector Local = T.InverseTransformPosition(Location);
		Local.X = FMath::Clamp(Local.X, -50.0, 50.0);
		Local.Y = FMath::Clamp(Local.Y, -50.0, 50.0);
		Local.Z = FMath::Clamp(Local.Z, static_cast<double>(Geometry.LocalZMin), static_cast<double>(Geometry.LocalZMax));
		return (T.TransformPosition(Local) - Location).Size();
	}

	/** The layer [cm above the ground] for a density band, the Create Cloud Host rule: the band +-10 m, bottom not below the ground (no
	 * cloud layer starts there), at least 0.1 km high. */
	void FogMS_TargetLayer(double AltMin, double AltMax, double& OutBottom, double& OutTop)
	{
		OutBottom = FMath::Max(0.0, AltMin - FogMS_HostLayerMarginCm);
		OutTop = FMath::Max(AltMax + FogMS_HostLayerMarginCm, OutBottom + FogMS_HostLayerMinHeightCm);
	}

	FString FogMS_CVarText(const IConsoleVariable* Variable, float Value)
	{
		return Variable && Variable->IsVariableInt() ? FString::FromInt(FMath::RoundToInt(Value)) : FString::SanitizeFloat(Value);
	}

	/** Game-setting priority write that never overrides an explicit (project, ini, device profile, command line, console) value. */
	bool FogMS_SetGameSetting(IConsoleVariable* Variable, float Value)
	{
		if (!Variable) return false;
		if (Variable->GetFloat() == Value) return true;
		const EConsoleVariableFlags SetBy = static_cast<EConsoleVariableFlags>(Variable->GetFlags() & ECVF_SetByMask);
		if (SetBy > ECVF_SetByGameSetting) return false;
		Variable->Set(*FogMS_CVarText(Variable, Value), ECVF_SetByGameSetting);
		return Variable->GetFloat() == Value;
	}

	FString FogMS_SetByName(const IConsoleVariable* Variable)
	{
		return Variable ? FString(GetConsoleVariableSetByName(static_cast<EConsoleVariableFlags>(Variable->GetFlags() & ECVF_SetByMask))) : FString();
	}

	/** Nominal ray-march step [cm] of a host (VolumetricCloud.usf: StepT = TraceLength / max(SampleCountMin, SampleCountMax *
	 * saturate(TraceLength / DistanceToSampleMaxCount)), SampleCountMax = min(96 * View Sample Count Scale, ViewRaySampleMaxCount),
	 * VolumetricCloudRendering.cpp): DistanceToSampleMaxCount / SampleCountMax on rays up to that distance, TraceLength / SampleCountMax
	 * beyond (up to Tracing Max Distance). Short rays below SampleMinCount steps march finer. For the status and FogMS_CloudStep (the
	 * W46 prefilter width, M_FogMS_Cloud node FogMS_CloudFootprint). */
	float FogMS_HostStepCm(const UVolumetricCloudComponent& Host)
	{
		const IConsoleVariable* Distance = FogMS_ManagedCVar(FogMS_MDistance);
		const IConsoleVariable* MaxSamples = FogMS_ManagedCVar(FogMS_MSampleMax);
		const float Samples = FMath::Max(2.0f, FMath::Min(UVolumetricCloudComponent::BaseViewRaySampleCount * FMath::Max(Host.ViewSampleCountScale, 0.05f),
			MaxSamples ? MaxSamples->GetFloat() : FogMS_EngineViewRaySampleMaxCount));
		const float DistanceKm = Distance ? FMath::Max(Distance->GetFloat(), 0.01f) : 15.0f;
		return FMath::Max(DistanceKm, Host.TracingMaxDistance) * 1.0e5f / Samples;
	}
}

UMaterial* UFogMSCloudHostSubsystem::GetCloudHostMaterial()
{
	static TWeakObjectPtr<UMaterial> Cached;
	static double LastAttempt = -1.0e9;
	if (UMaterial* Material = Cached.Get()) return Material;
	UMaterial* Material = FindObject<UMaterial>(nullptr, FogMS_CloudMaterialPath);
	if (!Material)
	{
		// A missing asset (not installed, or not cooked because no level references it) is retried at most once a second, quietly.
		const double Now = FPlatformTime::Seconds();
		if (Now - LastAttempt < 1.0) return nullptr;
		LastAttempt = Now;
		Material = LoadObject<UMaterial>(nullptr, FogMS_CloudMaterialPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
	}
	Cached = Material;
	return Material;
}

void UFogMSCloudHostSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	MarkDirtyHandle = UActorComponent::MarkRenderStateDirtyEvent.AddUObject(this, &UFogMSCloudHostSubsystem::OnMarkRenderStateDirty);
#if WITH_EDITOR
	PropertyChangedHandle = FCoreUObjectDelegates::OnObjectPropertyChanged.AddUObject(this, &UFogMSCloudHostSubsystem::OnObjectPropertyChanged);
#endif
}

void UFogMSCloudHostSubsystem::Deinitialize()
{
	UActorComponent::MarkRenderStateDirtyEvent.Remove(MarkDirtyHandle);
#if WITH_EDITOR
	FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(PropertyChangedHandle);
#endif
	RestoreHostSettings();
	Hosts.Reset();
	Super::Deinitialize();
}

bool UFogMSCloudHostSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE || WorldType == EWorldType::Editor;
}

TStatId UFogMSCloudHostSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UFogMSCloudHostSubsystem, STATGROUP_Tickables);
}

void UFogMSCloudHostSubsystem::Scan()
{
	if (ScanFrame == GFrameCounter) return;
	ScanFrame = GFrameCounter;
	ScanHosts.Reset();
	ScanIdle.Reset();
	ScanOthers.Reset();
	const UWorld* World = GetWorld();
	UMaterial* HostMaterial = GetCloudHostMaterial();
	ForEachObjectOfClass(UVolumetricCloudComponent::StaticClass(), [this, World, HostMaterial](UObject* Object)
	{
		UVolumetricCloudComponent* Component = static_cast<UVolumetricCloudComponent*>(Object);
		if (Component->GetWorld() != World || !Component->IsRegistered()) return;
		const bool bHost = HostMaterial && FogMS_BaseMaterial(Component) == HostMaterial;
		if (FogMS_CloudRenders(Component)) (bHost ? ScanHosts : ScanOthers).Add(Component);
		else if (bHost) ScanIdle.Add(Component);
	}, true, RF_ClassDefaultObject | RF_ArchetypeObject, EInternalObjectFlags::Garbage);
	const auto ByPath = [](const TWeakObjectPtr<UVolumetricCloudComponent>& A, const TWeakObjectPtr<UVolumetricCloudComponent>& B)
	{
		return A.Get() && B.Get() && A->GetPathName() < B->GetPathName();
	};
	ScanHosts.Sort(ByPath);
	ScanOthers.Sort(ByPath);
}

UFogMSCloudHostSubsystem::FHost* UFogMSCloudHostSubsystem::FindBinding(const UVolumetricCloudComponent* Component)
{
	for (FHost& Host : Hosts)
		if (Host.Component.Get() == Component) return &Host;
	return nullptr;
}

FString UFogMSCloudHostSubsystem::ValidateHostMaterial(const UVolumetricCloudComponent& Component) const
{
	const UMaterial* Base = FogMS_BaseMaterial(&Component);
	if (!Base) return TEXT("the host has no material");
	if (Base->MaterialDomain != MD_Volume) return TEXT("host material is not in the Volume domain");
	// VolumetricCloud.usf: an Unlit cloud material gets no albedo and no extinction.
	if (Base->GetShadingModels().IsUnlit()) return TEXT("host material is Unlit (the cloud would have no albedo and no extinction)");
	if (!Base->GetUsageByFlag(MATUSAGE_VolumetricCloud)) return TEXT("host material lacks 'Used with Volumetric Cloud'");
#if WITH_EDITORONLY_DATA
	// VolumetricCloud.usf adds Emissive once per multiple-scattering octave: with octaves the solver's field would be counted N + 1
	// times. The cooked game has no expressions; the editor check covers the authored asset.
	for (const TObjectPtr<UMaterialExpression>& Expression : Base->GetExpressions())
	{
		const UMaterialExpressionVolumetricAdvancedMaterialOutput* Output = Cast<UMaterialExpressionVolumetricAdvancedMaterialOutput>(Expression.Get());
		if (Output && Output->MultiScatteringApproximationOctaveCount != 0)
			return FString::Printf(TEXT("host material has %u multiple-scattering octaves (must be 0, else the field's light is added once per octave)"),
				Output->MultiScatteringApproximationOctaveCount);
	}
#endif
	return FString();
}

FString UFogMSCloudHostSubsystem::FitLayer(UVolumetricCloudComponent& Component, const FFogMSCloudBoxGeometry& Geometry, const AFogMSBoxVolume& Box,
	FHost& Host, bool& bInOutDirty, FString& OutNote)
{
	FVector Center;
	double Radius = 0.0;
	FogMS_CloudPlanet(GetWorld(), Component.PlanetRadius, Center, Radius);
	double AltMin = 0.0, AltMax = 0.0;
	FogMS_BandAltitudes(Geometry, Center, Radius, AltMin, AltMax);
	double Bottom = static_cast<double>(Component.LayerBottomAltitude) * 1.0e5;
	double Top = Bottom + static_cast<double>(FMath::Max(Component.LayerHeight, 0.1f)) * 1.0e5;
	const bool bFit = CVarCloudHostFitLayer.GetValueOnGameThread() != 0;
	if (bFit)
	{
		// W46: the host follows the Box (moved, scaled, height profile edited): refit only when the target moved by more than the
		// hysteresis, so the render state (MarkRenderStateDirty by the caller) is not re-created every frame for small changes.
		double WantBottom = 0.0, WantTop = 0.0;
		FogMS_TargetLayer(AltMin, AltMax, WantBottom, WantTop);
		if (FMath::Abs(Bottom - WantBottom) > FogMS_LayerRefitCm || FMath::Abs(Top - WantTop) > FogMS_LayerRefitCm)
		{
			const double OldBottom = Bottom, OldTop = Top;
			Component.LayerBottomAltitude = static_cast<float>(WantBottom * 1.0e-5);
			Component.LayerHeight = static_cast<float>((WantTop - WantBottom) * 1.0e-5);
			Bottom = static_cast<double>(Component.LayerBottomAltitude) * 1.0e5;
			Top = Bottom + static_cast<double>(Component.LayerHeight) * 1.0e5;
			bInOutDirty = true;
			++Host.RefitsSinceLog;
			const double Now = FPlatformTime::Seconds();
			if (Now - Host.LastRefitLogTime >= 1.0)
			{
				UE_LOG(LogMultiLobeSpec, Display, TEXT("FogMS cloud host '%s': layer %.3f-%.3f -> %.3f-%.3f km above the ground, fitted to Box '%s' (density band %.3f-%.3f km +-%.0f m; %d refit(s) since the previous line)."),
					*FogMS_Label(&Component), OldBottom * 1.0e-5, OldTop * 1.0e-5, Bottom * 1.0e-5, Top * 1.0e-5, *Box.GetActorNameOrLabel(),
					AltMin * 1.0e-5, AltMax * 1.0e-5, FogMS_HostLayerMarginCm * 0.01, Host.RefitsSinceLog);
				Host.LastRefitLogTime = Now;
				Host.RefitsSinceLog = 0;
			}
		}
	}
	OutNote = FString::Printf(TEXT("layer %.3f-%.3f km%s"), Bottom * 1.0e-5, Top * 1.0e-5,
		bFit ? TEXT(" (fitted to the Box)") : TEXT(" (r.FogMS.CloudHost.FitLayer 0)"));
	if (AltMin >= Bottom - FogMS_LayerToleranceCm && AltMax <= Top + FogMS_LayerToleranceCm) return FString();
	if (AltMin < -FogMS_LayerToleranceCm)
		return FString::Printf(TEXT("the Box's density band %.3f-%.3f km reaches below the ground of the cloud layer (altitude 0 of the SkyAtmosphere planet), where no cloud layer can start: move the Box up or use Render Path Froxel Fog"),
			AltMin * 1.0e-5, AltMax * 1.0e-5);
	return FString::Printf(TEXT("layer %.3f-%.3f km does not cover the Box's density band %.3f-%.3f km above the ground%s"),
		Bottom * 1.0e-5, Top * 1.0e-5, AltMin * 1.0e-5, AltMax * 1.0e-5, bFit ? TEXT("") : TEXT(" (r.FogMS.CloudHost.FitLayer 0: fit Layer Bottom Altitude / Layer Height by hand)"));
}

FFogMSCloudHostBinding UFogMSCloudHostSubsystem::AcquireHost(const AFogMSBoxVolume& Box, const FFogMSCloudBoxGeometry& Geometry)
{
	FFogMSCloudHostBinding Result;
	const auto Fallback = [&Result](const FString& Problem)
	{
		Result.MID = nullptr;
		Result.Status = FString::Printf(TEXT(" [cloud host: %s, froxel fallback]"), *Problem);
		return Result;
	};
	if (!GetCloudHostMaterial()) return Fallback(TEXT("none (M_FogMS_Cloud is missing: run matedit_cloud.py)"));
	Scan();
	if (ScanHosts.IsEmpty())
	{
		// W46: Render Path defaults to Cloud Host, so a Box without a host says what to do; a host that exists but does not render
		// (hidden, not visible) is named instead (Create Cloud Host would only return it).
		for (const TWeakObjectPtr<UVolumetricCloudComponent>& Idle : ScanIdle)
			if (Idle.IsValid())
				return Fallback(FString::Printf(TEXT("'%s' exists but does not render (hidden or not visible): show it to render this Box per pixel"),
					*FogMS_Label(Idle.Get())));
		return Fallback(TEXT("none: click 'Create Cloud Host' on this Box (or console FogMS.CloudHost.Create) to render it per pixel"));
	}
	// The host this Box already uses, else the first host no other Box fed in the last frame (P2: one Box per host).
	UVolumetricCloudComponent* Chosen = nullptr;
	const AFogMSBoxVolume* Busy = nullptr;
	for (const TWeakObjectPtr<UVolumetricCloudComponent>& Weak : ScanHosts)
	{
		UVolumetricCloudComponent* Component = Weak.Get();
		if (!Component) continue;
		const FHost* Binding = FindBinding(Component);
		const AFogMSBoxVolume* Owner = Binding ? Binding->Owner.Get() : nullptr;
		if (Owner == &Box) { Chosen = Component; break; }
		const bool bOwnerActive = Owner && Binding->FedFrame + 1 >= GFrameCounter;
		if (!bOwnerActive) { if (!Chosen) Chosen = Component; }
		else if (!Busy) Busy = Owner;
	}
	if (!Chosen)
		return Fallback(Busy ? FString::Printf(TEXT("busy with Box '%s' (one Box per host in this version)"), *Busy->GetActorNameOrLabel())
			: FString(TEXT("none")));
	const FString MaterialProblem = ValidateHostMaterial(*Chosen);
	if (!MaterialProblem.IsEmpty()) return Fallback(FString::Printf(TEXT("'%s': %s"), *FogMS_Label(Chosen), *MaterialProblem));

	// The host's MID: its material when that is already a MID (ours, also one saved with the level), else a new MID of it with the
	// cloud component as outer. SetMaterial re-creates the component's render state, which puts the host on top of the scene's
	// cloud stack.
	UMaterialInterface* Current = Chosen->Material.Get();
	UMaterialInstanceDynamic* MID = Cast<UMaterialInstanceDynamic>(Current);
	if (!MID)
	{
		MID = UMaterialInstanceDynamic::Create(Current, Chosen,
			MakeUniqueObjectName(Chosen, UMaterialInstanceDynamic::StaticClass(), TEXT("MID_FogMS_CloudHost")));
		if (MID) Chosen->SetMaterial(MID);
		if (!MID || Chosen->Material.Get() != MID)
			return Fallback(FString::Printf(TEXT("'%s': cannot assign a dynamic material (a Static-mobility host in a game world?)"), *FogMS_Label(Chosen)));
		UE_LOG(LogMultiLobeSpec, Display, TEXT("FogMS cloud host '%s': material %s wrapped in %s for Box '%s'."), *FogMS_Label(Chosen),
			*GetNameSafe(Current), *MID->GetName(), *Box.GetActorNameOrLabel());
	}
	FHost* Host = FindBinding(Chosen);
	if (!Host)
	{
		Host = &Hosts.AddDefaulted_GetRef();
		Host->Component = Chosen;
	}

	// W46 host settings the plugin owns (CPU only; one render-state update when something changed): View Sample Count Scale and the
	// layer fitted to this Box's density band. The component is movable here (it took a MID above), as a spawned host always is.
	bool bDirty = false;
	const float WantScale = CVarCloudHostViewSampleScale.GetValueOnGameThread();
	if (FMath::IsFinite(WantScale) && WantScale > 0.0f)
	{
		const float Scale = FMath::Clamp(WantScale, 0.05f, 64.0f);
		if (Chosen->ViewSampleCountScale != Scale)
		{
			UE_LOG(LogMultiLobeSpec, Display, TEXT("FogMS cloud host '%s': View Sample Count Scale %g -> %g (r.FogMS.CloudHost.ViewSampleScale)."),
				*FogMS_Label(Chosen), Chosen->ViewSampleCountScale, Scale);
			Chosen->ViewSampleCountScale = Scale;
			bDirty = true;
		}
	}
	FString LayerNote;
	const FString LayerProblem = FitLayer(*Chosen, Geometry, Box, *Host, bDirty, LayerNote);
	if (bDirty) Chosen->MarkRenderStateDirty();
	if (!LayerProblem.IsEmpty()) return Fallback(FString::Printf(TEXT("'%s': %s"), *FogMS_Label(Chosen), *LayerProblem));

	if (!Host->bBound || Host->Owner.Get() != &Box || Host->MID.Get() != MID)
	{
		// New binding: claim the render from any other rendering cloud on the next tick; one log line.
		if (!ScanOthers.IsEmpty()) bOthersDirty = true;
		UE_LOG(LogMultiLobeSpec, Display, TEXT("FogMS cloud host '%s' renders Box '%s' (Render Path = Cloud Host); the Box's froxel copy is off (FogMS_FroxelWeight 0)."),
			*FogMS_Label(Chosen), *Box.GetActorNameOrLabel());
	}
	Host->MID = MID;
	Host->Owner = &Box;
	Host->FedFrame = GFrameCounter;
	Host->bBound = true;
	Host->Geometry = Geometry;
	// W46 prefilter (M_FogMS_Cloud v2 node FogMS_CloudFootprint): the host's nominal ray-march step; the Box writes its own strength.
	const float StepCm = FogMS_HostStepCm(*Chosen);
	MID->SetScalarParameterValue(TEXT("FogMS_CloudStep"), StepCm);
	Result.MID = MID;
	FString Displaced;
	for (const TWeakObjectPtr<UVolumetricCloudComponent>& Other : ScanOthers)
		if (Other.IsValid()) Displaced += (Displaced.IsEmpty() ? TEXT("'") : TEXT(", '")) + FogMS_Label(Other.Get()) + TEXT("'");
	Result.Status = FString::Printf(TEXT(" [render: cloud host] [cloud host '%s': %s, step ~%.1f m, trace %.1f km, view samples x%g%s%s]"),
		*FogMS_Label(Chosen), *LayerNote, StepCm * 0.01f, Chosen->TracingMaxDistance, Chosen->ViewSampleCountScale, *SettingsNote,
		Displaced.IsEmpty() ? TEXT("") : *FString::Printf(TEXT("; displaces Volumetric Cloud %s (one cloud renders per scene)"), *Displaced));
	return Result;
}

void UFogMSCloudHostSubsystem::EmptyHost(FHost& Host, const TCHAR* Reason)
{
	// FogMS_Density 0: the extinction is 0 and the conservative density 0, so the cloud has nothing to march (M_FogMS_Cloud).
	if (UMaterialInstanceDynamic* MID = Host.MID.Get())
	{
		MID->SetScalarParameterValue(TEXT("FogMS_Density"), 0.0f);
		MID->SetScalarParameterValue(TEXT("FogMS_InjectionMode"), 0.0f);
	}
	if (Host.bBound)
		UE_LOG(LogMultiLobeSpec, Display, TEXT("FogMS cloud host '%s' is empty: %s."), *FogMS_Label(Host.Component.Get()), Reason);
	Host.Owner.Reset();
	Host.bBound = false;
}

void UFogMSCloudHostSubsystem::ReleaseBox(const AFogMSBoxVolume& Box)
{
	for (FHost& Host : Hosts)
		if (Host.bBound && Host.Owner.Get() == &Box) EmptyHost(Host, TEXT("its Box no longer renders through it"));
}

void UFogMSCloudHostSubsystem::Tick(float DeltaTime)
{
	const UVolumetricCloudComponent* SettingsHost = nullptr;
	// W46 near/far settings: the nearest camera rendered last frame (every perspective editor viewport / game view; the list is filled
	// after this world's previous tick and reset at the end of this one) to any hosted Box's density band box.
	const UWorld* World = GetWorld();
	double NearestCm = TNumericLimits<double>::Max();
	for (int32 Index = Hosts.Num() - 1; Index >= 0; --Index)
	{
		FHost& Host = Hosts[Index];
		if (!Host.Component.IsValid())
		{
			Hosts.RemoveAtSwap(Index);
			continue;
		}
		// A Box that stopped feeding without ReleaseBox (destroyed, hidden, density invalid): one frame of slack for tick order.
		if (Host.bBound && (!Host.Owner.IsValid() || Host.FedFrame + 1 < GFrameCounter))
			EmptyHost(Host, Host.Owner.IsValid() ? TEXT("its Box stopped using it") : TEXT("its Box is gone"));
		if (!Host.bBound) continue;
		SettingsHost = Host.Component.Get();
		if (World)
			for (const FVector& View : World->ViewLocationsRenderedLastFrame) NearestCm = FMath::Min(NearestCm, FogMS_BandDistanceCm(Host.Geometry, View));
	}
	if (!SettingsHost)
	{
		ClaimedOver.Reset();
		bOthersDirty = false;
		bNearSettings = true;
		NearestViewKm = -1.0;
		RestoreHostSettings();
		return;
	}
	// Displacement: FScene renders the most recently added cloud. When the set of other rendering clouds changes, or one of them was
	// re-created (MarkRenderStateDirty, an editor property edit), re-create the bound hosts' render state now, so they are on top again
	// from this frame's end-of-frame update on.
	Scan();
	bool bChanged = ScanOthers.Num() != ClaimedOver.Num();
	for (int32 Index = 0; !bChanged && Index < ScanOthers.Num(); ++Index) bChanged = ScanOthers[Index] != ClaimedOver[Index];
	if (bOthersDirty.exchange(false) || bChanged)
	{
		if (!ScanOthers.IsEmpty())
		{
			for (FHost& Host : Hosts)
			{
				if (!Host.bBound) continue;
				Host.Component->MarkRenderStateDirty();
				UE_LOG(LogMultiLobeSpec, Display, TEXT("FogMS cloud host '%s' takes the render over %d other Volumetric Cloud(s), e.g. '%s' (one cloud renders per scene)."),
					*FogMS_Label(Host.Component.Get()), ScanOthers.Num(), *FogMS_Label(ScanOthers[0].Get()));
			}
		}
		ClaimedOver = ScanOthers;
	}
	// Near or far settings (r.FogMS.CloudHost.NearDistanceKm, 10 % hysteresis). No camera rendered last frame (minimized, a non-realtime
	// viewport): keep the state.
	const float NearKm = CVarCloudHostNearDistanceKm.GetValueOnGameThread();
	const bool bWasNear = bNearSettings;
	if (!(FMath::IsFinite(NearKm) && NearKm > 0.0f))
		bNearSettings = true;
	else if (NearestCm < TNumericLimits<double>::Max())
	{
		NearestViewKm = NearestCm * 1.0e-5;
		if (bNearSettings && NearestViewKm > 1.1 * NearKm) bNearSettings = false;
		else if (!bNearSettings && NearestViewKm < NearKm) bNearSettings = true;
	}
	if (bWasNear != bNearSettings)
		UE_LOG(LogMultiLobeSpec, Display, TEXT("FogMS cloud host: camera %.2f km from the nearest hosted Box -> %s settings (r.FogMS.CloudHost.NearDistanceKm %g)."),
			NearestViewKm, bNearSettings ? TEXT("near") : TEXT("far"), NearKm);
	ApplyHostSettings(*SettingsHost);
}

void UFogMSCloudHostSubsystem::ApplyHostSettings(const UVolumetricCloudComponent& Host)
{
	float Want[ManagedCount] = {};
	bool bWant[ManagedCount] = {};
	const bool bStep = CVarCloudHostStepSettings.GetValueOnGameThread() != 0;
	bWant[FogMS_MDistance] = bStep;
	Want[FogMS_MDistance] = FMath::Max(Host.TracingMaxDistance, 0.1f);
	// View Sample Count Scale above 8 needs a higher cap than the engine's 768 samples (VolumetricCloudRendering.cpp SampleCountMax).
	const float Samples = FMath::CeilToFloat(UVolumetricCloudComponent::BaseViewRaySampleCount * FMath::Max(Host.ViewSampleCountScale, 0.05f));
	bWant[FogMS_MSampleMax] = bStep && Samples > FogMS_EngineViewRaySampleMaxCount;
	Want[FogMS_MSampleMax] = Samples;
	const int32 Mode = bNearSettings ? CVarCloudHostRTMode.GetValueOnGameThread() : CVarCloudHostFarRTMode.GetValueOnGameThread();
	bWant[FogMS_MMode] = Mode >= 0;
	Want[FogMS_MMode] = static_cast<float>(FMath::Min(Mode, 3));
	const int32 MinCount = bNearSettings ? CVarCloudHostSampleMinCount.GetValueOnGameThread() : CVarCloudHostFarSampleMinCount.GetValueOnGameThread();
	bWant[FogMS_MSampleMin] = MinCount >= 0;
	Want[FogMS_MSampleMin] = static_cast<float>(FMath::Min(MinCount, 4096));
	const int32 Upsampling = CVarCloudHostUpsamplingMode.GetValueOnGameThread();
	bWant[FogMS_MUpsampling] = Upsampling >= 0;
	Want[FogMS_MUpsampling] = static_cast<float>(FMath::Min(Upsampling, 4));
	const int32 Constraint = CVarCloudHostBoxConstraint.GetValueOnGameThread();
	bWant[FogMS_MBoxConstraint] = Constraint >= 0;
	Want[FogMS_MBoxConstraint] = Constraint > 0 ? 1.0f : 0.0f;
	const float MinKm = CVarCloudHostReprojectionMinKm.GetValueOnGameThread();
	bWant[FogMS_MMinKm] = FMath::IsFinite(MinKm) && MinKm >= 0.0f;
	Want[FogMS_MMinKm] = MinKm;

	FString Changes, Kept;
	for (int32 Index = 0; Index < ManagedCount; ++Index)
	{
		IConsoleVariable* Variable = FogMS_ManagedCVar(Index);
		FManagedSetting& Setting = Managed[Index];
		if (!Variable) continue;
		if (!bWant[Index])
		{
			if (Setting.bApplied) RestoreSetting(Index, Variable, Changes);
			continue;
		}
		if (!Setting.bApplied)
		{
			Setting.bApplied = true;
			Setting.Previous = Variable->GetFloat();
			Setting.Wanted = TNumericLimits<float>::Lowest(); // logs the first write below
		}
		// Every tick: follows the host's Tracing Max Distance, the near/far switch and edits of the r.FogMS.CloudHost.* settings; an
		// explicit value is never overwritten (FogMS_SetGameSetting) and only reported.
		const float Before = Variable->GetFloat();
		const bool bSet = FogMS_SetGameSetting(Variable, Want[Index]);
		if (Setting.Wanted != Want[Index])
		{
			Setting.Wanted = Want[Index];
			Changes += FString::Printf(TEXT(" %s %g -> %g%s;"), FogMS_ManagedCVarNames[Index], Before, Variable->GetFloat(),
				bSet ? TEXT("") : *FString::Printf(TEXT(" (kept: %s)"), *FogMS_SetByName(Variable)));
		}
		if (!bSet) Kept += FString::Printf(TEXT(", %s %g kept (%s)"), FogMS_ManagedCVarNames[Index], Variable->GetFloat(), *FogMS_SetByName(Variable));
	}
	const IConsoleVariable* ModeVariable = FogMS_ManagedCVar(FogMS_MMode);
	const IConsoleVariable* MinVariable = FogMS_ManagedCVar(FogMS_MSampleMin);
	const float NearKm = CVarCloudHostNearDistanceKm.GetValueOnGameThread();
	SettingsNote = FString::Printf(TEXT(", rt mode %d, min samples %d, %s"), ModeVariable ? ModeVariable->GetInt() : -1, MinVariable ? MinVariable->GetInt() : -1,
		!(FMath::IsFinite(NearKm) && NearKm > 0.0f) ? TEXT("near settings always")
		: *FString::Printf(TEXT("%s settings (camera %.2f km, near within %g km)"), bNearSettings ? TEXT("near") : TEXT("far"), NearestViewKm, NearKm)) + Kept;
	if (!Changes.IsEmpty())
		UE_LOG(LogMultiLobeSpec, Display, TEXT("FogMS cloud host settings (SetByGameSetting while a Box renders through a host; %s):%s"),
			bNearSettings ? TEXT("near") : TEXT("far"), *Changes);
}

void UFogMSCloudHostSubsystem::RestoreSetting(int32 Index, IConsoleVariable* Variable, FString& InOutChanges)
{
	FManagedSetting& Setting = Managed[Index];
	Setting.bApplied = false;
	if (!Variable) return;
	// Only a value still at game-setting priority is ours to put back; an explicit value set meanwhile stays.
	if (static_cast<EConsoleVariableFlags>(Variable->GetFlags() & ECVF_SetByMask) == ECVF_SetByGameSetting)
	{
		Variable->Set(*FogMS_CVarText(Variable, Setting.Previous), ECVF_SetByGameSetting);
		InOutChanges += FString::Printf(TEXT(" %s %g;"), FogMS_ManagedCVarNames[Index], Variable->GetFloat());
	}
	else InOutChanges += FString::Printf(TEXT(" %s %g kept (%s);"), FogMS_ManagedCVarNames[Index], Variable->GetFloat(), *FogMS_SetByName(Variable));
}

void UFogMSCloudHostSubsystem::RestoreHostSettings()
{
	FString Changes;
	for (int32 Index = 0; Index < ManagedCount; ++Index)
		if (Managed[Index].bApplied) RestoreSetting(Index, FogMS_ManagedCVar(Index), Changes);
	SettingsNote.Reset();
	if (!Changes.IsEmpty())
		UE_LOG(LogMultiLobeSpec, Display, TEXT("FogMS cloud host settings restored (no Box renders through a host):%s"), *Changes);
}

void UFogMSCloudHostSubsystem::OnMarkRenderStateDirty(UActorComponent& Component)
{
	// Called for every component of every world: cheapest test first. Our own hosts are skipped (their re-creation is the claim).
	if (!Component.IsA<UVolumetricCloudComponent>() || Component.GetWorld() != GetWorld()) return;
	for (const FHost& Host : Hosts)
		if (Host.Component.Get() == &Component) return;
	bOthersDirty = true;
}

#if WITH_EDITOR
void UFogMSCloudHostSubsystem::OnObjectPropertyChanged(UObject* Object, FPropertyChangedEvent& Event)
{
	// Details edits re-register the component (FComponentReregisterContext): no MarkRenderStateDirty, but the edited cloud goes on top.
	UVolumetricCloudComponent* Component = Cast<UVolumetricCloudComponent>(Object);
	if (!Component)
		if (const AVolumetricCloud* Actor = Cast<AVolumetricCloud>(Object)) Component = Actor->FindComponentByClass<UVolumetricCloudComponent>();
	if (!Component || Component->GetWorld() != GetWorld()) return;
	for (const FHost& Host : Hosts)
		if (Host.Component.Get() == Component) return;
	bOthersDirty = true;
}
#endif

AActor* UFogMSCloudHostSubsystem::SpawnHost(UWorld* World, const AFogMSBoxVolume* Box, FString& OutMessage)
{
	if (!World)
	{
		OutMessage = TEXT("no world: nothing spawned");
		return nullptr;
	}
	UMaterial* HostMaterial = GetCloudHostMaterial();
	UMaterialInterface* Instance = LoadObject<UMaterialInterface>(nullptr, FogMS_CloudInstancePath, nullptr, LOAD_NoWarn | LOAD_Quiet);
	if (!HostMaterial || !Instance || Instance->GetMaterial() != HostMaterial)
	{
		OutMessage = TEXT("M_FogMS_Cloud / MI_FogMS_Cloud are missing or MI_FogMS_Cloud is not an instance of M_FogMS_Cloud (run matedit_cloud.py in the editor): nothing spawned");
		return nullptr;
	}
	// One host per level is enough for P2: an existing one (visible or not) is reused.
	AActor* Existing = nullptr;
	FString Others;
	ForEachObjectOfClass(UVolumetricCloudComponent::StaticClass(), [&](UObject* Object)
	{
		const UVolumetricCloudComponent* Component = static_cast<const UVolumetricCloudComponent*>(Object);
		if (Component->GetWorld() != World || !Component->IsRegistered()) return;
		if (FogMS_BaseMaterial(Component) == HostMaterial) { if (!Existing) Existing = Component->GetOwner(); }
		else if (FogMS_CloudRenders(Component)) Others += (Others.IsEmpty() ? TEXT("'") : TEXT(", '")) + FogMS_Label(Component) + TEXT("'");
	}, true, RF_ClassDefaultObject | RF_ArchetypeObject, EInternalObjectFlags::Garbage);
	if (Existing)
	{
		OutMessage = FString::Printf(TEXT("cloud host '%s' already exists: nothing spawned"), *Existing->GetActorNameOrLabel());
		return Existing;
	}
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
#if WITH_EDITOR
	Params.ObjectFlags |= RF_Transactional;
#endif
	AVolumetricCloud* Host = World->SpawnActor<AVolumetricCloud>(AVolumetricCloud::StaticClass(), FTransform::Identity, Params);
	UVolumetricCloudComponent* Component = Host ? Host->FindComponentByClass<UVolumetricCloudComponent>() : nullptr;
	if (!Component)
	{
		OutMessage = TEXT("spawning the Volumetric Cloud actor failed");
		return Host;
	}
#if WITH_EDITOR
	Host->SetActorLabel(FogMS_CloudHostLabel);
#endif
	// Layer = the Box's density band +-10 m (at least 0.1 km), altitude above the SkyAtmosphere ground (FogMS_CloudPlanet); from then
	// on AcquireHost keeps it fitted (r.FogMS.CloudHost.FitLayer).
	double BottomCm = 0.0, TopCm = FogMS_HostLayerMinHeightCm;
	if (Box)
	{
		FFogMSCloudBoxGeometry Geometry;
		Box->GetCloudHostGeometry(Geometry);
		FVector Center;
		double Radius = 0.0;
		FogMS_CloudPlanet(World, Component->PlanetRadius, Center, Radius);
		double AltMin = 0.0, AltMax = 0.0;
		FogMS_BandAltitudes(Geometry, Center, Radius, AltMin, AltMax);
		FogMS_TargetLayer(AltMin, AltMax, BottomCm, TopCm);
	}
	const float WantScale = CVarCloudHostViewSampleScale.GetValueOnGameThread();
	const float ViewScale = FMath::IsFinite(WantScale) && WantScale > 0.0f ? FMath::Clamp(WantScale, 0.05f, 64.0f) : FogMS_HostViewSampleScale;
	Component->SetMobility(EComponentMobility::Movable);
	Component->LayerBottomAltitude = static_cast<float>(BottomCm * 1.0e-5);
	Component->LayerHeight = static_cast<float>((TopCm - BottomCm) * 1.0e-5);
	Component->TracingMaxDistanceMode = EVolumetricCloudTracingMaxDistanceMode::DistanceFromPointOfView;
	Component->TracingMaxDistance = FogMS_HostTraceKm;
	Component->TracingStartDistanceFromCamera = 0.0f;
	Component->ViewSampleCountScale = ViewScale;
	Component->ShadowViewSampleCountScale = FogMS_HostShadowSampleScale;
	Component->ShadowTracingDistance = FogMS_HostShadowTraceKm;
	Component->StopTracingTransmittanceThreshold = FogMS_HostStopTransmittance;
	Component->bUsePerSampleAtmosphericLightTransmittance = false;
	Component->bVisibleInRealTimeSkyCaptures = false;
	Component->SetMaterial(Instance);
	Component->MarkRenderStateDirty();
	// The per-frame scan cache may predate the new host: the next AcquireHost of this frame (the Box button calls UpdateDensity) sees it.
	if (UFogMSCloudHostSubsystem* Self = World->GetSubsystem<UFogMSCloudHostSubsystem>()) Self->ScanFrame = MAX_uint64;
	OutMessage = FString::Printf(TEXT("created cloud host '%s' (MI_FogMS_Cloud; layer %.3f-%.3f km above the ground, kept fitted to the Box; trace %.1f km from the camera, ")
		TEXT("view samples x%g, sun march %.2f km). Only one Volumetric Cloud renders per scene: while this host is visible it displaces %s. ")
		TEXT("Nothing was saved: save the level to keep the host, delete it to get the sky clouds back."),
		*Host->GetActorNameOrLabel(), BottomCm * 1.0e-5, TopCm * 1.0e-5, FogMS_HostTraceKm, ViewScale, FogMS_HostShadowTraceKm,
		Others.IsEmpty() ? TEXT("no other cloud (none is visible now)") : *FString::Printf(TEXT("the sky cloud %s"), *Others));
	return Host;
}
