#include "FogMS_CloudHost.h"
#include "FogMS_BoxVolume.h"

#include "Components/ActorComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/VolumetricCloudComponent.h"
#include "Engine/Engine.h"
#include "Engine/Texture.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
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
	// W47 cloud shadow map (FogMS_CloudHost.h class comment; FogMS_Weather_Design.md 4.1).
	TAutoConsoleVariable<int32> CVarCloudHostShadowFilter(TEXT("r.FogMS.CloudHost.ShadowSpatialFiltering"), 2,
		TEXT("W47: r.VolumetricCloud.ShadowMap.SpatialFiltering (blur iterations of the engine's cloud shadow map, at most 4; engine default 1) ")
		TEXT("while a Box renders through a cloud host and the atmosphere sun casts cloud shadows: the soft edge of the Box's shadow on the ground ")
		TEXT("and in the fog. Default 2. Negative = leave the engine cvar alone."), ECVF_Default);
	TAutoConsoleVariable<float> CVarCloudHostShadowSnapFraction(TEXT("r.FogMS.CloudHost.ShadowSnapFraction"), 0.25f,
		TEXT("W47: while a Box renders through a cloud host and the atmosphere sun casts cloud shadows, r.VolumetricCloud.ShadowMap.SnapLength = ")
		TEXT("this x the sun's Cloud Shadow Extent (km, at most the engine's 20 km) and r.VolumetricCloud.ShadowMap.SnapToPixelGrid 1 (no shimmer ")
		TEXT("with a small snap). The engine centres the shadow map on the camera in steps of the snap length: with its 20 km and a 5 km extent the ")
		TEXT("camera could be off the map (no shadow near it). Default 0.25 (5 km -> 1.25 km, a whole number of texels at 512 / 1024 / 2048). ")
		TEXT("0 or negative = leave both engine cvars alone."), ECVF_Default);
	// W48 weather (FogMS_CloudHost.h class comment, FogMS_Weather.h).
	TAutoConsoleVariable<int32> CVarWeatherSkipSteps(TEXT("r.FogMS.Weather.SkipSteps"), 8,
		TEXT("W48: r.VolumetricCloud.StepSizeOnZeroConservativeDensity while a FogMS Weather extends the layer of a cloud host that renders a Box ")
		TEXT("(Shadow Layer = Extended): the view ray skips empty space (conservative density 0: everything but the Box) this many steps at a time, ")
		TEXT("so the taller layer (up to the weather top) costs little in the visible pass; the Box's conservative region grows by the same ")
		TEXT("distance (FogMS_CloudSkipMargin = value x the host step), so a skip never jumps over the Box's entry and its samples stay on the ")
		TEXT("same grid. Default 8. 1 or less = leave the engine cvar alone (engine default 1: every empty step is visited)."), ECVF_Default);

	// P1 host settings (round 39, docs/history/FogMS_Prod_Report.md 'Раунд 39'): trace 2 km from the camera, view samples x8 (768), sun march 0.25 km
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
	/** W47: the engine's r.VolumetricCloud.ShadowMap.SnapLength default [km] (VolumetricCloudRendering.cpp), the cap of the plugin's
	 * snap; the base cloud shadow map resolution (512 x the sun's Cloud Shadow Map Resolution Scale) and the default of
	 * r.VolumetricCloud.ShadowMap.MaxResolution (GetVolumetricCloudShadowMapResolution). */
	constexpr float FogMS_EngineShadowSnapKm = 20.0f;
	constexpr float FogMS_ShadowMapBaseResolution = 512.0f;
	constexpr int32 FogMS_EngineShadowMapMaxResolution = 2048;
	/** W47 FogMS.CloudHost.SetupShadows defaults: a 10 km map (radius 5 km) of 1024 texels = 9.8 m per texel, enough for a Box of a
	 * few hundred metres within the host's 2 km trace (FogMS_Weather_Design.md 6, W47). */
	constexpr float FogMS_SetupShadowExtentKm = 5.0f;
	constexpr float FogMS_SetupShadowResolutionScale = 2.0f;

	/** W46: the engine cvars ApplyHostSettings manages, in UFogMSCloudHostSubsystem::Managed order (W47: + the cloud shadow map, W48:
	 * + the empty-step skip of the extended weather layer). */
	enum EFogMSManagedCVar : int32
	{
		FogMS_MDistance, FogMS_MSampleMin, FogMS_MSampleMax, FogMS_MMode, FogMS_MUpsampling, FogMS_MBoxConstraint, FogMS_MMinKm,
		FogMS_MShadowFilter, FogMS_MShadowSnap, FogMS_MShadowSnapToPixel, FogMS_MSkipSteps, FogMS_MCount
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
		TEXT("r.VolumetricCloud.ShadowMap.SpatialFiltering"),
		TEXT("r.VolumetricCloud.ShadowMap.SnapLength"),
		TEXT("r.VolumetricCloud.ShadowMap.SnapToPixelGrid"),
		TEXT("r.VolumetricCloud.StepSizeOnZeroConservativeDensity"),
	};

	/** W48 weather: the band a 'thin layer' host without a Box gets right under the weather base [cm], and the minimum layer height the
	 * engine allows (Layer Height ClampMin 0.1 km). */
	constexpr double FogMS_ThinBandCm = 1.0e4;
	/** W48: the M_FogMS_Cloud v3 scalar that tells a weather-capable host material (matedit_cloud.py). */
	const TCHAR* const FogMS_WeatherOnParameter = TEXT("FogMS_WeatherOn");

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

	/** W47: the engine's cloud shadow map resolution for a sun (GetVolumetricCloudShadowMapResolution, VolumetricCloudRendering.cpp):
	 * 512 x Cloud Shadow Map Resolution Scale, at most r.VolumetricCloud.ShadowMap.MaxResolution. */
	int32 FogMS_ShadowMapResolution(float ResolutionScale)
	{
		static IConsoleVariable* const MaxResolution = IConsoleManager::Get().FindConsoleVariable(TEXT("r.VolumetricCloud.ShadowMap.MaxResolution"), false);
		const int32 Cap = MaxResolution ? MaxResolution->GetInt() : FogMS_EngineShadowMapMaxResolution;
		return FMath::Max(1, FMath::Min(static_cast<int32>(FogMS_ShadowMapBaseResolution * ResolutionScale), Cap));
	}

	/** W47: one cloud shadow map texel [m]. The map is an orthographic projection of 2 x Cloud Shadow Extent (the extent is a radius
	 * in km) around the camera over its resolution (VolumetricCloudRendering.cpp, SphereDiameter). */
	double FogMS_ShadowTexelM(float ExtentKm, int32 Resolution)
	{
		return 2.0 * static_cast<double>(ExtentKm) * 1000.0 / static_cast<double>(FMath::Max(Resolution, 1));
	}

	FString FogMS_LightLabel(const UDirectionalLightComponent* Light)
	{
		if (!Light) return FString(TEXT("?"));
		return Light->GetOwner() ? Light->GetOwner()->GetActorNameOrLabel() : Light->GetName();
	}

	/** W47: the sun builds a cloud shadow map (VolumetricCloudRendering.cpp ShouldRenderCloudShadowmap; r.VolumetricCloud.ShadowMap is
	 * checked by the status only). */
	bool FogMS_SunCastsCloudShadows(const UDirectionalLightComponent* Sun)
	{
		return Sun && Sun->bCastCloudShadows && Sun->CloudShadowStrength > 0.0f;
	}
}

// W47: FogMS.CloudHost.SetupShadows (FogMS_CloudHost.h class comment).
static FAutoConsoleCommandWithWorldAndArgs GFogMSCloudHostSetupShadowsCommand(TEXT("FogMS.CloudHost.SetupShadows"),
	TEXT("FogMS.CloudHost.SetupShadows [ExtentKm, default 5] [ResolutionScale, default 2]: sets the atmosphere sun of this world up for the soft ")
	TEXT("ground shadow of the Boxes a cloud host renders (W47): Cast Cloud Shadows on, Cloud Shadow Extent = ExtentKm (the radius of the cloud ")
	TEXT("shadow map around the camera), Cloud Shadow Map Resolution Scale = ResolutionScale (512 x scale texels, at most ")
	TEXT("r.VolumetricCloud.ShadowMap.MaxResolution); texel = 2 x extent / resolution (5 km, x2: 9.8 m). One log line with the previous ")
	TEXT("values; one undo step in the editor. Nothing is saved: save the level to keep it."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
	{
		const float ExtentKm = Args.Num() > 0 ? FCString::Atof(*Args[0]) : FogMS_SetupShadowExtentKm;
		const float Scale = Args.Num() > 1 ? FCString::Atof(*Args[1]) : FogMS_SetupShadowResolutionScale;
		// The Details ranges of the two sun properties (ClampMin 1 km / 0.25), with generous upper bounds.
		if (!(FMath::IsFinite(ExtentKm) && ExtentKm >= 1.0f && ExtentKm <= 10000.0f) || !(FMath::IsFinite(Scale) && Scale >= 0.25f && Scale <= 16.0f))
		{
			UE_LOG(LogMultiLobeSpec, Warning, TEXT("FogMS.CloudHost.SetupShadows: extent %g km / resolution scale %g out of range (extent 1..10000 km, scale 0.25..16): nothing changed."),
				ExtentKm, Scale);
			return;
		}
		FString Message;
		if (UFogMSCloudHostSubsystem::SetupSunShadows(World, ExtentKm, Scale, Message))
		{
			UE_LOG(LogMultiLobeSpec, Display, TEXT("FogMS.CloudHost.SetupShadows: %s"), *Message);
		}
		else
		{
			UE_LOG(LogMultiLobeSpec, Warning, TEXT("FogMS.CloudHost.SetupShadows: %s"), *Message);
		}
	}));

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
	ScanSun = FindAtmosphereSun(World);
}

void UFogMSCloudHostSubsystem::GetWeatherPlanet(const UWorld* World, FVector& OutCenter, double& OutRadius)
{
	FogMS_CloudPlanet(World, 6360.0, OutCenter, OutRadius);
}

UDirectionalLightComponent* UFogMSCloudHostSubsystem::FindAtmosphereSun(const UWorld* World, int32* OutCount)
{
	// The renderer's rule (FScene::AddLightSceneInfo_RenderThread): among the lights used as atmosphere sun 0, the brightest.
	UDirectionalLightComponent* Best = nullptr;
	float BestLuminance = 0.0f;
	int32 Count = 0;
	if (World)
	{
		ForEachObjectOfClass(UDirectionalLightComponent::StaticClass(), [&Best, &BestLuminance, &Count, World](UObject* Object)
		{
			UDirectionalLightComponent* Light = static_cast<UDirectionalLightComponent*>(Object);
			if (Light->GetWorld() != World || !Light->IsRegistered() || !Light->IsVisible() || !Light->bAffectsWorld
				|| !Light->IsUsedAsAtmosphereSunLight() || Light->GetAtmosphereSunLightIndex() != 0) return;
			++Count;
			const float Luminance = Light->GetColoredLightBrightness().GetLuminance();
			if (!Best || Luminance > BestLuminance)
			{
				Best = Light;
				BestLuminance = Luminance;
			}
		}, true, RF_ClassDefaultObject | RF_ArchetypeObject, EInternalObjectFlags::Garbage);
	}
	if (OutCount) *OutCount = Count;
	return Best;
}

FString UFogMSCloudHostSubsystem::CloudShadowNote(const FFogMSCloudBoxGeometry& Geometry) const
{
	const UDirectionalLightComponent* Sun = ScanSun.Get();
	if (!Sun)
		return TEXT(" [cloud shadow: none: no atmosphere sun (a visible Directional Light with Atmosphere Sun Light, index 0)]");
	const FString SunLabel = FogMS_LightLabel(Sun);
	if (!Sun->bCastCloudShadows)
		return FString::Printf(TEXT(" [cloud shadow: off: the sun '%s' has Cast Cloud Shadows off; for this Box's soft shadow on the ground and in the fog run FogMS.CloudHost.SetupShadows (ticks it, extent 5 km, resolution x2) or tick it on the sun]"),
			*SunLabel);
	static IConsoleVariable* const ShadowMap = IConsoleManager::Get().FindConsoleVariable(TEXT("r.VolumetricCloud.ShadowMap"), false);
	if (ShadowMap && ShadowMap->GetInt() <= 0)
		return TEXT(" [cloud shadow: off: r.VolumetricCloud.ShadowMap is 0]");
	if (!(Sun->CloudShadowStrength > 0.0f))
		return FString::Printf(TEXT(" [cloud shadow: off: Cloud Shadow Strength of the sun '%s' is 0]"), *SunLabel);
	const int32 Resolution = FogMS_ShadowMapResolution(Sun->CloudShadowMapResolutionScale);
	const double TexelM = FogMS_ShadowTexelM(Sun->CloudShadowExtent, Resolution);
	const IConsoleVariable* Filter = FogMS_ManagedCVar(FogMS_MShadowFilter);
	FString Note = FString::Printf(TEXT(" [cloud shadow: extent %g km, res %d, texel %.1f m, filter %d"), Sun->CloudShadowExtent, Resolution, TexelM,
		Filter ? Filter->GetInt() : -1);
	if (!(Sun->CloudShadowOnSurfaceStrength > 0.0f)) Note += TEXT("; Cloud Shadow On Surface Strength 0: no shadow on the ground");
	// The density cube is -50..50 local units scaled to the Box: a side is 100 x scale cm = scale m. Horizontal = the cube's X/Y.
	const FVector Scale = Geometry.CubeToWorld.GetScale3D().GetAbs();
	const double BoxM = FMath::Min(Scale.X, Scale.Y);
	if (TexelM > BoxM)
		Note += FString::Printf(TEXT("; WARNING texel %.0f m > Box %.0f m: its shadow is one blurred blot, run FogMS.CloudHost.SetupShadows (extent 5 km, resolution x2: texel 9.8 m)"),
			TexelM, BoxM);
	return Note + TEXT("]");
}

bool UFogMSCloudHostSubsystem::SetupSunShadows(UWorld* World, float ExtentKm, float ResolutionScale, FString& OutMessage, float RaySampleScale)
{
	int32 Count = 0;
	UDirectionalLightComponent* Sun = FindAtmosphereSun(World, &Count);
	if (!Sun)
	{
		OutMessage = TEXT("no atmosphere sun in this world (a visible Directional Light with Atmosphere Sun Light, index 0): nothing changed.");
		return false;
	}
	const bool bWasCasting = Sun->bCastCloudShadows != 0;
	const float OldExtent = Sun->CloudShadowExtent;
	const float OldScale = Sun->CloudShadowMapResolutionScale;
	const int32 OldResolution = FogMS_ShadowMapResolution(OldScale);
	const int32 NewResolution = FogMS_ShadowMapResolution(ResolutionScale);
	// W48: the weather's tall layer needs more samples along each shadow ray (the engine traces 16 x Cloud Shadow Ray Sample Count Scale
	// samples through the whole layer, at most r.VolumetricCloud.ShadowMap.RaySampleMaxCount); <= 0 = leave it (the W47 command).
	const bool bRaySamples = FMath::IsFinite(RaySampleScale) && RaySampleScale > 0.0f;
	const float OldRayScale = Sun->CloudShadowRaySampleCountScale;
	const FString Values = FString::Printf(TEXT("Cast Cloud Shadows %s -> on, Cloud Shadow Extent %g -> %g km, Cloud Shadow Map Resolution Scale %g -> %g (map %d -> %d texels, texel %.1f -> %.1f m)%s"),
		bWasCasting ? TEXT("on") : TEXT("off"), OldExtent, ExtentKm, OldScale, ResolutionScale, OldResolution, NewResolution,
		FogMS_ShadowTexelM(OldExtent, OldResolution), FogMS_ShadowTexelM(ExtentKm, NewResolution),
		bRaySamples ? *FString::Printf(TEXT(", Cloud Shadow Ray Sample Count Scale %g -> %g (%g -> %g samples per shadow ray before the horizon boost)"),
			OldRayScale, RaySampleScale, FMath::Max(4.0f, 16.0f * OldRayScale), FMath::Max(4.0f, 16.0f * RaySampleScale)) : TEXT(""));
	const FString Several = Count > 1
		? FString::Printf(TEXT(" (%d atmosphere suns with index 0: the brightest one, which the renderer uses)"), Count) : FString();
	if (bWasCasting && OldExtent == ExtentKm && OldScale == ResolutionScale && (!bRaySamples || OldRayScale == RaySampleScale))
	{
		OutMessage = FString::Printf(TEXT("sun '%s'%s is already set (%s): nothing changed."), *FogMS_LightLabel(Sun), *Several, *Values);
		return true;
	}
	// The owner's actor changes only here, on request: one undo step in the editor (UEngine::BeginTransaction exists only WITH_EDITOR;
	// the editor engine implements it), a new light proxy with the new values at the end of the frame.
	int32 Transaction = INDEX_NONE;
#if WITH_EDITOR
	if (GEngine) Transaction = GEngine->BeginTransaction(TEXT("FogMS"), NSLOCTEXT("FogMS", "SetupCloudShadows", "FogMS: Setup Cloud Shadows"), Sun);
#endif
	Sun->Modify();
	Sun->bCastCloudShadows = true;
	Sun->CloudShadowExtent = ExtentKm;
	Sun->CloudShadowMapResolutionScale = ResolutionScale;
	if (bRaySamples) Sun->CloudShadowRaySampleCountScale = RaySampleScale;
	Sun->MarkRenderStateDirty();
#if WITH_EDITOR
	if (Transaction != INDEX_NONE && GEngine) GEngine->EndTransaction();
#endif
	OutMessage = FString::Printf(TEXT("sun '%s'%s: %s. Nothing saved: save the level to keep it%s."), *FogMS_LightLabel(Sun), *Several, *Values,
		Transaction != INDEX_NONE ? TEXT(" (Ctrl+Z undoes it)") : TEXT(""));
	return true;
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
		// W48 extended weather layer: the layer also covers the weather envelope (shadow pass only; the view pass skips its empty steps
		// StepSizeOnZeroConservativeDensity at a time, ApplyHostSettings). The envelope is rounded outward by the actor, so a transition
		// refits the layer at its start and end only.
		const FFogMSWeatherFeed* Weather = ExtendingWeather();
		if (Weather)
		{
			WantBottom = FMath::Min(WantBottom, FMath::Max(0.0, Weather->BottomCm));
			WantTop = FMath::Max(WantTop, Weather->TopCm);
		}
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
				UE_LOG(LogMultiLobeSpec, Display, TEXT("FogMS cloud host '%s': layer %.3f-%.3f -> %.3f-%.3f km above the ground, fitted to Box '%s' (density band %.3f-%.3f km +-%.0f m%s; %d refit(s) since the previous line)."),
					*FogMS_Label(&Component), OldBottom * 1.0e-5, OldTop * 1.0e-5, Bottom * 1.0e-5, Top * 1.0e-5, *Box.GetActorNameOrLabel(),
					AltMin * 1.0e-5, AltMax * 1.0e-5, FogMS_HostLayerMarginCm * 0.01,
					Weather ? *FString::Printf(TEXT(" + FogMS Weather %.3f-%.3f km, shadow pass only"), Weather->BottomCm * 1.0e-5, Weather->TopCm * 1.0e-5) : TEXT(""),
					Host.RefitsSinceLog);
				Host.LastRefitLogTime = Now;
				Host.RefitsSinceLog = 0;
			}
		}
	}
	const FFogMSWeatherFeed* Weather = bFit ? ExtendingWeather() : nullptr;
	OutNote = FString::Printf(TEXT("layer %.3f-%.3f km%s"), Bottom * 1.0e-5, Top * 1.0e-5,
		!bFit ? TEXT(" (r.FogMS.CloudHost.FitLayer 0)")
		: Weather ? TEXT(" (fitted to the Box + the weather layers, shadow pass only)") : TEXT(" (fitted to the Box)"));
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
	{
		// W47: the busy host's cloud shadow holds the other Box, which this Box's solver does not see (its froxel single scattering does).
		const FString ShadowNote = Busy && FogMS_SunCastsCloudShadows(ScanSun.Get())
			? FString::Printf(TEXT("; its cloud shadow (Box '%s') is not in this Box's solver field"), *Busy->GetActorNameOrLabel()) : FString();
		return Fallback(Busy ? FString::Printf(TEXT("busy with Box '%s' (one Box per host in this version)%s"), *Busy->GetActorNameOrLabel(), *ShadowNote)
			: FString(TEXT("none")));
	}
	const FString MaterialProblem = ValidateHostMaterial(*Chosen);
	if (!MaterialProblem.IsEmpty()) return Fallback(FString::Printf(TEXT("'%s': %s"), *FogMS_Label(Chosen), *MaterialProblem));

	// The host's MID: its material when that is already a MID (ours, also one saved with the level, or the weather's), else a new MID of
	// it with the cloud component as outer. SetMaterial re-creates the component's render state, which puts the host on top of the
	// scene's cloud stack.
	FString MIDProblem;
	UMaterialInstanceDynamic* MID = EnsureHostMID(*Chosen, FString::Printf(TEXT("Box '%s'"), *Box.GetActorNameOrLabel()), MIDProblem);
	if (!MID) return Fallback(FString::Printf(TEXT("'%s': %s"), *FogMS_Label(Chosen), *MIDProblem));
	FHost* Host = FindBinding(Chosen);
	if (!Host)
	{
		Host = &Hosts.AddDefaulted_GetRef();
		Host->Component = Chosen;
	}

	// W46 host settings the plugin owns (CPU only; one render-state update when something changed): View Sample Count Scale and the
	// layer fitted to this Box's density band. The component is movable here (it took a MID above), as a spawned host always is.
	bool bDirty = false;
	// W48: a host the weather ran 'shadows only' (Tracing Start Distance >= Tracing Max Distance: an empty view trace, also as saved with a
	// level) draws this Box again from the camera on: start distance 0. A smaller authored start distance is the author's and stays.
	if (Host->bShadowsOnly || (Chosen->TracingStartDistanceFromCamera > 0.0f && Chosen->TracingStartDistanceFromCamera >= Chosen->TracingMaxDistance))
	{
		UE_LOG(LogMultiLobeSpec, Display, TEXT("FogMS cloud host '%s': Tracing Start Distance %g -> 0 km (it renders Box '%s' again; weather shadows-only mode ends)."),
			*FogMS_Label(Chosen), Chosen->TracingStartDistanceFromCamera, *Box.GetActorNameOrLabel());
		Chosen->TracingStartDistanceFromCamera = 0.0f;
		Host->bShadowsOnly = false;
		bDirty = true;
	}
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
	// A Box has adopted this material and layer; weather must never later restore its pre-Box snapshot over the Box's state.
	Host->SavedWeatherMaterial.Reset();
	Host->bWeatherSaved = false;
	Host->bWeatherMaterialReplaced = false;
	Host->Geometry = Geometry;
	// W46 prefilter (M_FogMS_Cloud v2 node FogMS_CloudFootprint): the host's nominal ray-march step; the Box writes its own strength.
	const float StepCm = FogMS_HostStepCm(*Chosen);
	MID->SetScalarParameterValue(TEXT("FogMS_CloudStep"), StepCm);
	Result.MID = MID;
	FString Displaced;
	for (const TWeakObjectPtr<UVolumetricCloudComponent>& Other : ScanOthers)
		if (Other.IsValid()) Displaced += (Displaced.IsEmpty() ? TEXT("'") : TEXT(", '")) + FogMS_Label(Other.Get()) + TEXT("'");
	// W47: the Box's shadow through the engine's cloud shadow map (FogMS_CloudHost.h): settings, hint or warning; one log line whenever
	// that part changes (the sun's flag, extent or resolution, the filter the subsystem sets on its tick).
	const FString ShadowNote = CloudShadowNote(Geometry);
	if (ShadowNote != Host->LastShadowNote)
	{
		Host->LastShadowNote = ShadowNote;
		UE_LOG(LogMultiLobeSpec, Display, TEXT("FogMS cloud host '%s' (Box '%s'):%s"), *FogMS_Label(Chosen), *Box.GetActorNameOrLabel(), *ShadowNote);
	}
	Result.Status = FString::Printf(TEXT(" [render: cloud host] [cloud host '%s': %s, step ~%.1f m, trace %.1f km, view samples x%g%s%s]%s"),
		*FogMS_Label(Chosen), *LayerNote, StepCm * 0.01f, Chosen->TracingMaxDistance, Chosen->ViewSampleCountScale, *SettingsNote,
		Displaced.IsEmpty() ? TEXT("") : *FString::Printf(TEXT("; displaces Volumetric Cloud %s (one cloud renders per scene)"), *Displaced),
		*ShadowNote);
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
	Host.LastShadowNote.Reset();
}

void UFogMSCloudHostSubsystem::ReleaseBox(const AFogMSBoxVolume& Box)
{
	for (FHost& Host : Hosts)
		if (Host.bBound && Host.Owner.Get() == &Box) EmptyHost(Host, TEXT("its Box no longer renders through it"));
}

void UFogMSCloudHostSubsystem::Tick(float DeltaTime)
{
	UVolumetricCloudComponent* SettingsHost = nullptr;
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
	// W48: the weather's host (shadow pass), with or without a Box.
	UVolumetricCloudComponent* WeatherHost = TickWeather();
	if (!SettingsHost && !WeatherHost)
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
	// from this frame's end-of-frame update on (W48: also the weather's host: the engine builds the cloud shadow map from the rendering
	// cloud only).
	Scan();
	bool bChanged = ScanOthers.Num() != ClaimedOver.Num();
	for (int32 Index = 0; !bChanged && Index < ScanOthers.Num(); ++Index) bChanged = ScanOthers[Index] != ClaimedOver[Index];
	if (bOthersDirty.exchange(false) || bChanged)
	{
		if (!ScanOthers.IsEmpty())
		{
			for (FHost& Host : Hosts)
			{
				if (!Host.bBound && !Host.bWeather) continue;
				Host.Component->MarkRenderStateDirty();
				UE_LOG(LogMultiLobeSpec, Display, TEXT("FogMS cloud host '%s' takes the render over %d other Volumetric Cloud(s), e.g. '%s' (one cloud renders per scene)."),
					*FogMS_Label(Host.Component.Get()), ScanOthers.Num(), *FogMS_Label(ScanOthers[0].Get()));
			}
		}
		ClaimedOver = ScanOthers;
	}
	if (SettingsHost)
	{
		// Near or far settings (r.FogMS.CloudHost.NearDistanceKm, 10 % hysteresis). No camera rendered last frame (minimized, a
		// non-realtime viewport): keep the state.
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
	}
	const bool bWeatherActive = WeatherHost && WeatherFeed.bActive;
	const bool bWeatherExtended = WeatherHost && WeatherHost == SettingsHost && ExtendingWeather() != nullptr;
	ApplyHostSettings(SettingsHost ? *SettingsHost : *WeatherHost, SettingsHost != nullptr, bWeatherActive, bWeatherExtended);
}

void UFogMSCloudHostSubsystem::ApplyHostSettings(UVolumetricCloudComponent& Host, bool bHero, bool bWeatherActive, bool bWeatherExtended)
{
	float Want[ManagedCount] = {};
	bool bWant[ManagedCount] = {};
	// W46 view settings: only while a Box renders through the host (a weather-only host traces nothing in view: 'shadows only').
	const bool bStep = bHero && CVarCloudHostStepSettings.GetValueOnGameThread() != 0;
	bWant[FogMS_MDistance] = bStep;
	Want[FogMS_MDistance] = FMath::Max(Host.TracingMaxDistance, 0.1f);
	// View Sample Count Scale above 8 needs a higher cap than the engine's 768 samples (VolumetricCloudRendering.cpp SampleCountMax).
	const float Samples = FMath::CeilToFloat(UVolumetricCloudComponent::BaseViewRaySampleCount * FMath::Max(Host.ViewSampleCountScale, 0.05f));
	bWant[FogMS_MSampleMax] = bStep && Samples > FogMS_EngineViewRaySampleMaxCount;
	Want[FogMS_MSampleMax] = Samples;
	const int32 Mode = bNearSettings ? CVarCloudHostRTMode.GetValueOnGameThread() : CVarCloudHostFarRTMode.GetValueOnGameThread();
	bWant[FogMS_MMode] = bHero && Mode >= 0;
	Want[FogMS_MMode] = static_cast<float>(FMath::Min(Mode, 3));
	const int32 MinCount = bNearSettings ? CVarCloudHostSampleMinCount.GetValueOnGameThread() : CVarCloudHostFarSampleMinCount.GetValueOnGameThread();
	bWant[FogMS_MSampleMin] = bHero && MinCount >= 0;
	Want[FogMS_MSampleMin] = static_cast<float>(FMath::Min(MinCount, 4096));
	const int32 Upsampling = CVarCloudHostUpsamplingMode.GetValueOnGameThread();
	bWant[FogMS_MUpsampling] = bHero && Upsampling >= 0;
	Want[FogMS_MUpsampling] = static_cast<float>(FMath::Min(Upsampling, 4));
	const int32 Constraint = CVarCloudHostBoxConstraint.GetValueOnGameThread();
	bWant[FogMS_MBoxConstraint] = bHero && Constraint >= 0;
	Want[FogMS_MBoxConstraint] = Constraint > 0 ? 1.0f : 0.0f;
	const float MinKm = CVarCloudHostReprojectionMinKm.GetValueOnGameThread();
	bWant[FogMS_MMinKm] = bHero && FMath::IsFinite(MinKm) && MinKm >= 0.0f;
	Want[FogMS_MMinKm] = MinKm;
	// W48: the extended weather layer makes every upward view ray march up to Tracing Max Distance through empty space: skip it.
	const int32 SkipSteps = CVarWeatherSkipSteps.GetValueOnGameThread();
	bWant[FogMS_MSkipSteps] = bHero && bWeatherExtended && SkipSteps > 1;
	Want[FogMS_MSkipSteps] = static_cast<float>(FMath::Min(SkipSteps, 64));
	// W47 cloud shadow map: only while the atmosphere sun casts cloud shadows (the engine builds the map only then); otherwise, and when
	// the sun stops casting them, the previous values come back. W48: also for the weather without a Box.
	const UDirectionalLightComponent* Sun = ScanSun.Get();
	const bool bShadowMap = (bHero || bWeatherActive) && FogMS_SunCastsCloudShadows(Sun);
	const int32 ShadowFilter = CVarCloudHostShadowFilter.GetValueOnGameThread();
	bWant[FogMS_MShadowFilter] = bShadowMap && ShadowFilter >= 0;
	Want[FogMS_MShadowFilter] = static_cast<float>(FMath::Min(ShadowFilter, 4));
	const float SnapFraction = CVarCloudHostShadowSnapFraction.GetValueOnGameThread();
	const bool bSnap = bShadowMap && FMath::IsFinite(SnapFraction) && SnapFraction > 0.0f && FMath::IsFinite(Sun->CloudShadowExtent);
	bWant[FogMS_MShadowSnap] = bSnap;
	// Whole metres, so the value survives the text round trip of IConsoleVariable::Set exactly.
	Want[FogMS_MShadowSnap] = bSnap
		? FMath::Clamp(FMath::RoundToFloat(SnapFraction * Sun->CloudShadowExtent * 1000.0f) / 1000.0f, 0.01f, FogMS_EngineShadowSnapKm) : 0.0f;
	bWant[FogMS_MShadowSnapToPixel] = bSnap;
	Want[FogMS_MShadowSnapToPixel] = 1.0f;

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
			// W47: a value that already was the wanted one (e.g. SnapToPixelGrid 1, the engine default) is not a change.
			if (!bSet || Before != Variable->GetFloat())
				Changes += FString::Printf(TEXT(" %s %g -> %g%s;"), FogMS_ManagedCVarNames[Index], Before, Variable->GetFloat(),
					bSet ? TEXT("") : *FString::Printf(TEXT(" (kept: %s)"), *FogMS_SetByName(Variable)));
		}
		if (!bSet) Kept += FString::Printf(TEXT(", %s %g kept (%s)"), FogMS_ManagedCVarNames[Index], Variable->GetFloat(), *FogMS_SetByName(Variable));
	}
	// W48: with an empty-step skip in effect (the weather's or any explicit value), the Box's conservative region grows by skip x the host
	// step (M_FogMS_Cloud v3 FogMS_CloudConservative v2), so a skip from outside never lands past the Box's entry; 0 without a skip.
	const IConsoleVariable* SkipVariable = FogMS_ManagedCVar(FogMS_MSkipSteps);
	const int32 SkipNow = SkipVariable ? FMath::Max(SkipVariable->GetInt(), 1) : 1;
	if (bHero)
	{
		if (UMaterialInstanceDynamic* MID = Cast<UMaterialInstanceDynamic>(Host.Material.Get()))
			MID->SetScalarParameterValue(TEXT("FogMS_CloudSkipMargin"), SkipNow > 1 ? static_cast<float>(SkipNow) * FogMS_HostStepCm(Host) : 0.0f);
	}
	const IConsoleVariable* ModeVariable = FogMS_ManagedCVar(FogMS_MMode);
	const IConsoleVariable* MinVariable = FogMS_ManagedCVar(FogMS_MSampleMin);
	const float NearKm = CVarCloudHostNearDistanceKm.GetValueOnGameThread();
	SettingsNote = FString::Printf(TEXT(", rt mode %d, min samples %d, %s%s"), ModeVariable ? ModeVariable->GetInt() : -1, MinVariable ? MinVariable->GetInt() : -1,
		!(FMath::IsFinite(NearKm) && NearKm > 0.0f) ? TEXT("near settings always")
		: *FString::Printf(TEXT("%s settings (camera %.2f km, near within %g km)"), bNearSettings ? TEXT("near") : TEXT("far"), NearestViewKm, NearKm),
		SkipNow > 1 ? *FString::Printf(TEXT(", empty-step skip x%d%s"), SkipNow, bWeatherExtended ? TEXT(" (weather layer)") : TEXT("")) : TEXT("")) + Kept;
	if (!Changes.IsEmpty())
		UE_LOG(LogMultiLobeSpec, Display, TEXT("FogMS cloud host settings (SetByGameSetting while a Box renders through a host%s; %s):%s"),
			bHero ? TEXT("") : TEXT(" or the weather uses one without a Box: cloud shadow map only"), bNearSettings ? TEXT("near") : TEXT("far"), *Changes);
}

void UFogMSCloudHostSubsystem::RestoreSetting(int32 Index, IConsoleVariable* Variable, FString& InOutChanges)
{
	FManagedSetting& Setting = Managed[Index];
	Setting.bApplied = false;
	if (!Variable) return;
	// W47: nothing to put back when the value is the previous one (it was already the wanted value, e.g. SnapToPixelGrid 1).
	if (Variable->GetFloat() == Setting.Previous)
	{
		InOutChanges += FString::Printf(TEXT(" %s %g;"), FogMS_ManagedCVarNames[Index], Variable->GetFloat());
		return;
	}
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

UMaterialInstanceDynamic* UFogMSCloudHostSubsystem::EnsureHostMID(UVolumetricCloudComponent& Component, const FString& Context, FString& OutProblem)
{
	UMaterialInterface* Current = Component.Material.Get();
	if (UMaterialInstanceDynamic* Existing = Cast<UMaterialInstanceDynamic>(Current)) return Existing;
	UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(Current, &Component,
		MakeUniqueObjectName(&Component, UMaterialInstanceDynamic::StaticClass(), TEXT("MID_FogMS_CloudHost")));
	if (MID) Component.SetMaterial(MID);
	if (!MID || Component.Material.Get() != MID)
	{
		OutProblem = TEXT("cannot assign a dynamic material (a Static-mobility host in a game world?)");
		return nullptr;
	}
	UE_LOG(LogMultiLobeSpec, Display, TEXT("FogMS cloud host '%s': material %s wrapped in %s for %s."), *FogMS_Label(&Component),
		*GetNameSafe(Current), *MID->GetName(), *Context);
	return MID;
}

bool UFogMSCloudHostSubsystem::IsHostBoundToBox(const AActor* HostActor) const
{
	for (const FHost& Host : Hosts)
		if (Host.bBound && Host.Component.IsValid() && Host.Component->GetOwner() == HostActor) return true;
	return false;
}

bool UFogMSCloudHostSubsystem::ClaimWeather(const AActor& Owner, FString& OutOther)
{
	const AActor* Current = WeatherFeed.Owner.Get();
	if (!Current || Current == &Owner || WeatherFeed.FedFrame + 1 < GFrameCounter) return true;
	OutOther = Current->GetActorNameOrLabel();
	return false;
}

void UFogMSCloudHostSubsystem::FeedWeather(const FFogMSWeatherFeed& Feed)
{
	const AActor* Previous = WeatherFeed.Owner.Get();
	if (Previous && Previous != Feed.Owner.Get())
		for (FHost& Host : Hosts) if (Host.bWeather) ReleaseWeatherHost(Host, TEXT("another FogMS Weather actor took over"));
	WeatherFeed = Feed;
	WeatherFeed.FedFrame = GFrameCounter;
}

void UFogMSCloudHostSubsystem::StopWeather(const AActor& Owner, const TCHAR* Reason)
{
	if (WeatherFeed.Owner.Get() != &Owner) return;
	for (FHost& Host : Hosts)
	{
		if (Host.bWeather) ReleaseWeatherHost(Host, Reason);
		Host.bWeatherMaterialReplaced = false;
	}
	UE_LOG(LogMultiLobeSpec, Display, TEXT("FogMS Weather '%s' stopped (%s): its shadow-pass branch is off; the managed cloud cvars follow on the next tick."),
		*Owner.GetActorNameOrLabel(), Reason);
	WeatherFeed = FFogMSWeatherFeed();
	bWeatherNeedsHost = false;
	bWeatherHostUsable = false;
	WeatherNote.Reset();
	LastWeatherLog.Reset();
}

const FFogMSWeatherFeed* UFogMSCloudHostSubsystem::ExtendingWeather() const
{
	if (!WeatherFeed.Owner.IsValid() || WeatherFeed.FedFrame + 1 < GFrameCounter) return nullptr;
	if (!WeatherFeed.bActive || WeatherFeed.bThinLayer || !bWeatherHostUsable) return nullptr;
	return &WeatherFeed;
}

void UFogMSCloudHostSubsystem::WriteWeatherParameters(UMaterialInstanceDynamic& MID, const UVolumetricCloudComponent& Component, bool bOn) const
{
	MID.SetScalarParameterValue(FogMS_WeatherOnParameter, bOn ? 1.0f : 0.0f);
	MID.SetScalarParameterValue(TEXT("FogMS_WeatherLightingOn"), bOn && WeatherFeed.bLightLocalClouds ? 1.0f : 0.0f);
	if (!bOn) return;
	MID.SetScalarParameterValue(TEXT("FogMS_WeatherThin"), WeatherFeed.bThinLayer ? 1.0f : 0.0f);
	if (UTexture* Texture = WeatherFeed.Map.Get()) MID.SetTextureParameterValue(TEXT("FogMS_WeatherMap"), Texture);
	if (UTexture* Texture = WeatherFeed.SunMap.Get()) MID.SetTextureParameterValue(TEXT("FogMS_WeatherSunMap"), Texture);
	if (UTexture* Texture = WeatherFeed.TypeLUT.Get()) MID.SetTextureParameterValue(TEXT("FogMS_WeatherTypeLUT"), Texture);
	if (UTexture* Texture = WeatherFeed.Pattern.Get()) MID.SetTextureParameterValue(TEXT("FogMS_WeatherPattern"), Texture);
	if (UTexture* Texture = WeatherFeed.Curl.Get()) MID.SetTextureParameterValue(TEXT("FogMS_WeatherCurl"), Texture);
	MID.SetVectorParameterValue(TEXT("FogMS_WeatherOrigin"), WeatherFeed.Origin);
	MID.SetVectorParameterValue(TEXT("FogMS_WeatherDomain"), WeatherFeed.Domain);
	MID.SetVectorParameterValue(TEXT("FogMS_WeatherWind"), WeatherFeed.Wind);
	MID.SetVectorParameterValue(TEXT("FogMS_WeatherL0"), WeatherFeed.L0);
	MID.SetVectorParameterValue(TEXT("FogMS_WeatherL1"), WeatherFeed.L1);
	// Toward the atmosphere sun (the one the engine's cloud shadow map uses) and the host layer height [cm] (thin layer: the weather column
	// is spread over the layer's path along the sun).
	const UDirectionalLightComponent* Sun = ScanSun.Get();
	const FVector ToSun = Sun ? FVector(-Sun->GetForwardVector()).GetSafeNormal() : FVector::UpVector;
	MID.SetVectorParameterValue(TEXT("FogMS_WeatherSunDir"), FLinearColor(static_cast<float>(ToSun.X), static_cast<float>(ToSun.Y), static_cast<float>(ToSun.Z),
		FMath::Max(Component.LayerHeight, 0.1f) * 1.0e5f));
}

void UFogMSCloudHostSubsystem::ReleaseWeatherHost(FHost& Host, const TCHAR* Reason)
{
	if (!Host.bWeather) return;
	UVolumetricCloudComponent* Component = Host.Component.Get();
	FString Restored;
	if (Component)
	{
		const bool bOwnsMaterial = Host.WeatherMID.IsValid() && Component->Material.Get() == Host.WeatherMID.Get();
		if (UMaterialInstanceDynamic* MID = bOwnsMaterial ? Host.WeatherMID.Get() : nullptr)
		{
			MID->SetScalarParameterValue(FogMS_WeatherOnParameter, 0.0f);
			MID->SetScalarParameterValue(TEXT("FogMS_WeatherLightingOn"), 0.0f);
		}
		bool bDirty = false;
		if (!Host.bBound && Host.bWeatherSaved && bOwnsMaterial)
		{
			// Restore the untouched original (including any original MID overrides), only while our private MID still owns this host.
			if (UMaterialInterface* Original = Host.SavedWeatherMaterial.Get())
			{
				Component->SetMaterial(Original);
				Restored += FString::Printf(TEXT("; original material '%s' restored"), *Original->GetName());
			}
			Restored += FString::Printf(TEXT("; layer %.3f-%.3f -> %.3f-%.3f km, Tracing Start Distance %g -> %g km"), Component->LayerBottomAltitude,
				Component->LayerBottomAltitude + Component->LayerHeight, Host.SavedLayerBottomKm, Host.SavedLayerBottomKm + Host.SavedLayerHeightKm,
				Component->TracingStartDistanceFromCamera, Host.SavedStartDistanceKm);
			Component->LayerBottomAltitude = Host.SavedLayerBottomKm;
			Component->LayerHeight = Host.SavedLayerHeightKm;
			Component->TracingStartDistanceFromCamera = Host.SavedStartDistanceKm;
			bDirty = true;
		}
		else if (!Host.bBound && Host.bShadowsOnly && bOwnsMaterial)
		{
			Restored += FString::Printf(TEXT("; Tracing Start Distance %g -> 0 km"), Component->TracingStartDistanceFromCamera);
			Component->TracingStartDistanceFromCamera = 0.0f;
			bDirty = true;
		}
		else if (Host.bBound)
			Restored += TEXT("; the layer refits to its Box on the Box's next update");
		else if (!bOwnsMaterial)
		{
			Host.bWeatherMaterialReplaced = true;
			Restored += TEXT("; material replaced externally: material and layer left unchanged");
		}
		if (bDirty) Component->MarkRenderStateDirty();
	}
	UE_LOG(LogMultiLobeSpec, Display, TEXT("FogMS Weather leaves cloud host '%s' (%s): weather shadows off%s."), *FogMS_Label(Component), Reason, *Restored);
	Host.bWeather = false;
	Host.bShadowsOnly = false;
	Host.bWeatherSaved = false;
	Host.SavedWeatherMaterial.Reset();
	Host.WeatherMID.Reset();
}

UVolumetricCloudComponent* UFogMSCloudHostSubsystem::TickWeather()
{
	bWeatherNeedsHost = false;
	const AActor* Owner = WeatherFeed.Owner.Get();
	if (!Owner || WeatherFeed.FedFrame + 1 < GFrameCounter)
	{
		// The actor stopped feeding without StopWeather (removed with its level, ticking off): release what it used.
		for (FHost& Host : Hosts)
		{
			if (Host.bWeather) ReleaseWeatherHost(Host, TEXT("the FogMS Weather actor stopped feeding"));
			Host.bWeatherMaterialReplaced = false;
		}
		if (WeatherFeed.FedFrame != 0) WeatherFeed = FFogMSWeatherFeed();
		bWeatherHostUsable = false;
		WeatherNote.Reset();
		LastWeatherLog.Reset();
		return nullptr;
	}
	if (!WeatherFeed.bActive)
	{
		// Clear and cirrus-only weather do not need a shadow host. Release before selecting a host or wrapping its material.
		for (FHost& Host : Hosts)
		{
			if (Host.bWeather) ReleaseWeatherHost(Host, TEXT("the weather has no active low layer or deck"));
			Host.bWeatherMaterialReplaced = false;
		}
		bWeatherHostUsable = false;
		WeatherNote = TEXT("no active low layer or deck: no weather shadow host claimed");
		LastWeatherLog.Reset();
		return nullptr;
	}
	const FString Label = Owner->GetActorNameOrLabel();
	const auto Problem = [this, &Label](const FString& Note)
	{
		for (FHost& Host : Hosts)
			if (Host.bWeather) ReleaseWeatherHost(Host, TEXT("the weather cannot use it now"));
		bWeatherHostUsable = false;
		WeatherNote = Note;
		if (Note != LastWeatherLog)
		{
			LastWeatherLog = Note;
			UE_LOG(LogMultiLobeSpec, Warning, TEXT("FogMS Weather '%s': %s"), *Label, *Note);
		}
		return static_cast<UVolumetricCloudComponent*>(nullptr);
	};
	Scan();
	// The host a Box renders through (both share it: one cloud renders per scene), else the first rendering host.
	UVolumetricCloudComponent* Chosen = nullptr;
	for (const TWeakObjectPtr<UVolumetricCloudComponent>& Weak : ScanHosts)
	{
		UVolumetricCloudComponent* Component = Weak.Get();
		if (!Component) continue;
		const FHost* Binding = FindBinding(Component);
		if (Binding && Binding->bBound) { Chosen = Component; break; }
		if (!Chosen) Chosen = Component;
	}
	for (FHost& Host : Hosts)
		if (Host.bWeather && Host.Component.Get() != Chosen) ReleaseWeatherHost(Host, TEXT("the weather uses another cloud host now"));
	if (!Chosen)
	{
		FString Hidden;
		for (const TWeakObjectPtr<UVolumetricCloudComponent>& Idle : ScanIdle)
			if (Idle.IsValid()) { Hidden = FogMS_Label(Idle.Get()); break; }
		bWeatherNeedsHost = Hidden.IsEmpty();
		return Problem(Hidden.IsEmpty()
			? FString(TEXT("no cloud host in this level: no weather shadows (tick Create Cloud Host on the weather actor, or run FogMS.CloudHost.Create)"))
			: FString::Printf(TEXT("cloud host '%s' exists but does not render (hidden or not visible): show it for the weather shadows"), *Hidden));
	}
	const FString MaterialProblem = ValidateHostMaterial(*Chosen);
	if (!MaterialProblem.IsEmpty()) return Problem(FString::Printf(TEXT("cloud host '%s': %s: no weather shadows"), *FogMS_Label(Chosen), *MaterialProblem));
	float WeatherDefault = 0.0f;
	const UMaterial* Base = FogMS_BaseMaterial(Chosen);
	if (!Base || !Base->GetScalarParameterDefaultValue(FHashedMaterialParameterInfo(FogMS_WeatherOnParameter), WeatherDefault))
		return Problem(FString::Printf(TEXT("cloud host '%s': M_FogMS_Cloud has no weather shadow branch (older than W48): run matedit_cloud.py; no weather shadows"),
			*FogMS_Label(Chosen)));
	FHost* Host = FindBinding(Chosen);
	if (!Host)
	{
		Host = &Hosts.AddDefaulted_GetRef();
		Host->Component = Chosen;
	}
	FString MIDProblem;
	if (Host->bWeatherMaterialReplaced && !Host->bBound)
		return Problem(FString::Printf(TEXT("cloud host '%s': material replaced externally; toggle weather off/on to resume weather shadows"), *FogMS_Label(Chosen)));
	UMaterialInstanceDynamic* MID = nullptr;
	if (Host->bBound)
	{
		MID = EnsureHostMID(*Chosen, FString::Printf(TEXT("FogMS Weather '%s'"), *Label), MIDProblem);
	}
	else if (Host->bWeatherSaved)
	{
		// A user replacement ends this claim; never restore the old snapshot over the replacement.
		if (Chosen->Material.Get() != Host->WeatherMID.Get())
			return Problem(FString::Printf(TEXT("cloud host '%s': its weather material was replaced externally"), *FogMS_Label(Chosen)));
		MID = Host->WeatherMID.Get();
	}
	else
	{
		TStrongObjectPtr<UMaterialInterface> Original(Chosen->Material.Get());
		UMaterialInstanceDynamic* OriginalMID = Cast<UMaterialInstanceDynamic>(Original.Get());
		UMaterialInterface* Parent = OriginalMID ? OriginalMID->Parent.Get() : Original.Get();
		MID = UMaterialInstanceDynamic::Create(Parent, Chosen,
			MakeUniqueObjectName(Chosen, UMaterialInstanceDynamic::StaticClass(), TEXT("MID_FogMS_WeatherHost")));
		if (MID)
		{
			// A sibling, not a dynamic-instance parent chain: keep the same parent/static permutation and all authored overrides.
			if (OriginalMID) MID->CopyParameterOverrides(OriginalMID);
			Chosen->SetMaterial(MID);
			if (Chosen->Material.Get() == MID)
			{
				Host->SavedWeatherMaterial = MoveTemp(Original);
				Host->bWeatherSaved = true;
				Host->SavedLayerBottomKm = Chosen->LayerBottomAltitude;
				Host->SavedLayerHeightKm = Chosen->LayerHeight;
				Host->SavedStartDistanceKm = Chosen->TracingStartDistanceFromCamera;
			}
			else MID = nullptr;
		}
		if (!MID) MIDProblem = TEXT("cannot assign a private weather material");
	}
	if (!MID) return Problem(FString::Printf(TEXT("cloud host '%s': %s"), *FogMS_Label(Chosen), *MIDProblem));
	Host->WeatherMID = MID;
	bWeatherHostUsable = true;
	if (!Host->bWeather)
	{
		Host->bWeather = true;
		UE_LOG(LogMultiLobeSpec, Display, TEXT("FogMS Weather '%s' casts its shadows through cloud host '%s' (shadow pass only, %s layer)."), *Label, *FogMS_Label(Chosen),
			WeatherFeed.bThinLayer ? TEXT("thin") : TEXT("extended"));
	}
	LastWeatherLog.Reset();
	FString Mode;
	if (!Host->bBound)
	{
		// 'Shadows only': no Box renders through this host. Its previous layer and start distance are kept once for the restore.
		bool bDirty = false;
		if (!Host->bWeatherSaved)
		{
			Host->bWeatherSaved = true;
			Host->SavedLayerBottomKm = Chosen->LayerBottomAltitude;
			Host->SavedLayerHeightKm = Chosen->LayerHeight;
			Host->SavedStartDistanceKm = Chosen->TracingStartDistanceFromCamera;
		}
		// The view pass traces nothing (TMin = TMax = Tracing Max Distance: SampleMinCount steps at one point, all skipped by conservative
		// density 0); the cloud shadow map is traced from the layer, independent of it.
		const float EmptyTrace = FMath::Max(Chosen->TracingMaxDistance, 0.1f);
		if (Chosen->TracingStartDistanceFromCamera < EmptyTrace)
		{
			UE_LOG(LogMultiLobeSpec, Display, TEXT("FogMS cloud host '%s': weather shadows only (no Box renders through it): Tracing Start Distance %g -> %g km (empty view trace)."),
				*FogMS_Label(Chosen), Chosen->TracingStartDistanceFromCamera, EmptyTrace);
			Chosen->TracingStartDistanceFromCamera = EmptyTrace;
			bDirty = true;
		}
		Host->bShadowsOnly = true;
		if (WeatherFeed.bActive && CVarCloudHostFitLayer.GetValueOnGameThread() != 0)
		{
			double WantBottom = 0.0, WantTop = 0.0;
			if (WeatherFeed.bThinLayer)
			{
				WantTop = FMath::Max(WeatherFeed.BaseCm, FogMS_ThinBandCm);
				WantBottom = WantTop - FogMS_ThinBandCm;
			}
			else
			{
				WantBottom = FMath::Max(0.0, WeatherFeed.BottomCm);
				WantTop = FMath::Max(WeatherFeed.TopCm, WantBottom + FogMS_HostLayerMinHeightCm);
			}
			const double Bottom = static_cast<double>(Chosen->LayerBottomAltitude) * 1.0e5;
			const double Top = Bottom + static_cast<double>(Chosen->LayerHeight) * 1.0e5;
			if (FMath::Abs(Bottom - WantBottom) > FogMS_LayerRefitCm || FMath::Abs(Top - WantTop) > FogMS_LayerRefitCm)
			{
				UE_LOG(LogMultiLobeSpec, Display, TEXT("FogMS cloud host '%s': layer %.3f-%.3f -> %.3f-%.3f km above the ground, fitted to FogMS Weather '%s' (%s, shadows only)."),
					*FogMS_Label(Chosen), Bottom * 1.0e-5, Top * 1.0e-5, WantBottom * 1.0e-5, WantTop * 1.0e-5, *Label,
					WeatherFeed.bThinLayer ? TEXT("thin layer: a 0.1 km band under the weather base") : TEXT("the weather layers"));
				Chosen->LayerBottomAltitude = static_cast<float>(WantBottom * 1.0e-5);
				Chosen->LayerHeight = static_cast<float>((WantTop - WantBottom) * 1.0e-5);
				bDirty = true;
			}
		}
		// No hero in the view or the shadow pass (EmptyHost wrote these when the Box left; a host the weather wrapped never had one).
		MID->SetScalarParameterValue(TEXT("FogMS_Density"), 0.0f);
		MID->SetScalarParameterValue(TEXT("FogMS_InjectionMode"), 0.0f);
		if (bDirty) Chosen->MarkRenderStateDirty();
		Mode = TEXT("shadows only: no Box renders through it, the view trace is empty");
	}
	else
	{
		const AFogMSBoxVolume* Box = Host->Owner.Get();
		Mode = FString::Printf(TEXT("also renders Box '%s'"), Box ? *Box->GetActorNameOrLabel() : TEXT("?"));
	}
	WriteWeatherParameters(*MID, *Chosen, WeatherFeed.bActive);
	const IConsoleVariable* Skip = FogMS_ManagedCVar(FogMS_MSkipSteps);
	WeatherNote = FString::Printf(TEXT("host '%s' (%s): layer %.3f-%.3f km%s%s"), *FogMS_Label(Chosen), *Mode, Chosen->LayerBottomAltitude,
		Chosen->LayerBottomAltitude + Chosen->LayerHeight,
		!WeatherFeed.bActive ? TEXT(", weather branch off (no active layer)")
		: WeatherFeed.bThinLayer ? TEXT(", thin layer: the weather column is spread over it in the shadow pass")
		: TEXT(", extended to the weather layers (shadow pass only)"),
		Host->bBound && WeatherFeed.bActive && !WeatherFeed.bThinLayer && Skip ? *FString::Printf(TEXT(", view empty-step skip x%d"), FMath::Max(Skip->GetInt(), 1)) : TEXT(""));
	return Chosen;
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

AActor* UFogMSCloudHostSubsystem::SpawnHost(UWorld* World, const AFogMSBoxVolume* Box, FString& OutMessage, bool* bOutSpawned)
{
	if (bOutSpawned) *bOutSpawned = false;
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
	if (bOutSpawned) *bOutSpawned = Host != nullptr;
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
	OutMessage = FString::Printf(TEXT("created cloud host '%s' (MI_FogMS_Cloud; layer %.3f-%.3f km above the ground, %s; trace %.1f km from the camera, ")
		TEXT("view samples x%g, sun march %.2f km). Only one Volumetric Cloud renders per scene: while this host is visible it displaces %s. ")
		TEXT("Nothing was saved: save the level to keep the host, delete it to get the sky clouds back."),
		*Host->GetActorNameOrLabel(), BottomCm * 1.0e-5, TopCm * 1.0e-5,
		Box ? TEXT("kept fitted to the Box") : TEXT("fitted to the FogMS Weather layers / the Boxes that use it"), FogMS_HostTraceKm, ViewScale, FogMS_HostShadowTraceKm,
		Others.IsEmpty() ? TEXT("no other cloud (none is visible now)") : *FString::Printf(TEXT("the sky cloud %s"), *Others));
	return Host;
}
