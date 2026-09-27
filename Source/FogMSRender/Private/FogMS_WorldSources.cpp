#include "FogMS_WorldSources.h"
#include "FogMS_WorldLighting.h" // FFogMSWorldSky (game-thread sky snapshot)

#include "HAL/IConsoleManager.h"
#include "LightSceneInfo.h"
#include "LightSceneProxy.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "PooledRenderTarget.h"
#include "ReadOnlyCVARCache.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "RHIStaticStates.h"
#include "SceneInterface.h"
#include "SceneManagement.h"
// Renderer/Private, P11 light list only (the sky sources are public): ScenePrivate.h for FScene (the static_cast of
// View.Family->Scene, FScene::Lights, FScene::AtmosphereLights, FScene::VolumetricCloud); LightSceneInfo.h (above) for
// FLightSceneInfoCompact::LightSceneInfo and FLightSceneInfo::bVisible / ::Proxy. W47: VolumetricCloudRendering.h for
// FVolumetricCloudRenderSceneInfo::GetVolumetricCloudSceneProxy() (inline; the proxy class itself is public Engine,
// VolumetricCloudProxy.h), only to tell a FogMS cloud host from a foreign cloud for the status note below.
#include "ScenePrivate.h"
#include "SceneView.h"
#include "SystemTextures.h"
#include "TextureResource.h"
#include "VolumetricCloudProxy.h"
#include "VolumetricCloudRendering.h"

DEFINE_LOG_CATEGORY_STATIC(LogFogMSWorldSources, Log, All);

namespace
{
	constexpr int32 MaxWorldLights = 256;
	constexpr int32 LightRows = 5;
	constexpr float SourceGridSize = 32.0f;

	TAutoConsoleVariable<float> CVarWorldSunExcludeDegrees(TEXT("r.FogMS.World.SunExcludeDegrees"), 3.0f,
		TEXT("Half-angle in degrees of the cone around the atmosphere sun that is removed from the captured sky radiance used as transport/world boundary. Direct sun is accounted separately with shadows; 0 disables."), ECVF_RenderThreadSafe);
	TAutoConsoleVariable<float> CVarWorldSkyMipBias(TEXT("r.FogMS.World.SkyMipBias"), 0.0f,
		TEXT("Added to the sector-matched sky cubemap mip used as transport/world boundary radiance. Negative sharpens, positive blurs."), ECVF_RenderThreadSafe);
	TAutoConsoleVariable<int32> CVarWorldSkySource(TEXT("r.FogMS.World.SkySource"), 0,
		TEXT("Sky boundary radiance of the transport/world solve. 0 auto: Real Time Capture + rendered SkyAtmosphere -> Sky View LUT; ")
		TEXT("Real Time Capture + a FogMS Weather sky dome (W49: the capture holds the weather clouds, the LUT does not) -> the capture's SH; ")
		TEXT("static capture -> processed cubemap (public USkyLightComponent API); otherwise the sky SH (View UB). ")
		TEXT("2 force Sky View LUT, 3 force processed capture, 4 force SH. An unavailable forced source falls back to SH; the Box ")
		TEXT("status names the source in use. 1 (the removed private renderer path) acts as 0 with a one-time log warning; other values act as 0."),
		ECVF_RenderThreadSafe);
	TAutoConsoleVariable<int32> CVarWorldSkyLutSamples(TEXT("r.FogMS.World.SkyLutSamples"), 5,
		TEXT("Sky View LUT sky source only: LUT taps averaged (equal weights) over each quadrature sector cone, clamped to [1,13]: ")
		TEXT("centre, up to 8 on the cone rim, the rest (up to 4) at half the cone angle. Deterministic per ordinate. 1 = point sample."),
		ECVF_RenderThreadSafe);

	// FogMSWorldSkySource values (FogMS_WorldSources.ush).
	constexpr uint32 SkySourceNone = 0;
	constexpr uint32 SkySourceCubemap = 1;
	constexpr uint32 SkySourceViewLut = 2;
	constexpr uint32 SkySourceSH = 3;

	bool Finite(const FVector3f& Value)
	{
		return FMath::IsFinite(Value.X) && FMath::IsFinite(Value.Y) && FMath::IsFinite(Value.Z);
	}

	bool Nonnegative(const FVector3f& Value)
	{
		return Finite(Value) && Value.X >= 0 && Value.Y >= 0 && Value.Z >= 0;
	}

	// W47: the scene's rendering Volumetric Cloud (FScene renders only FScene::VolumetricCloud, the last one added) is a FogMS
	// cloud host when its material's base material is /MultiLobeSpec/FogMS/M_FogMS_Cloud: the rule of the game-thread host scan
	// (UFogMSCloudHostSubsystem::Scan, FogMS_CloudHost.cpp). Render thread: the proxy keeps the component's material pointer
	// (the component keeps the object alive); GetMaterial_Concurrent is the engine's any-thread parent walk. OutMaterial = the
	// cloud material's name for the status note.
	bool CloudIsFogMSHost(const FScene& Scene, FString& OutMaterial)
	{
		const FVolumetricCloudRenderSceneInfo* const Cloud = Scene.VolumetricCloud;
		const UMaterialInterface* const Material = Cloud ? Cloud->GetVolumetricCloudSceneProxy().GetCloudVolumeMaterial() : nullptr;
		const UMaterial* const Base = Material ? Material->GetMaterial_Concurrent() : nullptr;
		OutMaterial = Material ? Material->GetName() : FString(TEXT("none"));
		static const FName HostMaterialName(TEXT("M_FogMS_Cloud"));
		return Base && Base->GetFName() == HostMaterialName;
	}

	// Sky parameters of "no sky": black cube dummy, zero intensity, source none; cone/LUT/SH state inert.
	void ResetSky(FRDGBuilder& GraphBuilder, FFogMSWorldSourcesParameters& Parameters)
	{
		Parameters.FogMSWorldSkyTexture = GSystemTextures.GetCubeBlackDummy(GraphBuilder);
		Parameters.FogMSWorldSkyBlendTexture = Parameters.FogMSWorldSkyTexture;
		Parameters.FogMSWorldSkySampler = TStaticSamplerState<SF_Trilinear>::GetRHI();
		Parameters.FogMSWorldSkyBlendSampler = Parameters.FogMSWorldSkySampler;
		Parameters.FogMSWorldSkyIntensity = 0;
		Parameters.FogMSWorldSkyBlend = 0;
		Parameters.FogMSWorldSkySource = SkySourceNone;
		Parameters.FogMSWorldSkyUndoRTCExposure = 0;
		Parameters.FogMSWorldSkyLowerHemisphere = FVector4f(0, 0, 0, 0);
		Parameters.FogMSWorldSkyLutSamples = 1;
	}

	// Public mirror of ShouldRenderSkyAtmosphere (SkyAtmosphereRendering.cpp:488-499), which gates both the Sky View LUT
	// render (DeferredShadingRenderer.cpp:2430-2438 / 2845-2850) and View.SkyAtmospherePresentInScene (SceneRendering.cpp:1564-1566):
	// FSceneInterface::GetSkyAtmosphereSceneInfo() (SceneInterface.h:436; compared with null only, the type stays opaque),
	// the Atmosphere show flag, r.SupportSkyAtmosphere (FReadOnlyCVARCache) and r.SkyAtmosphere > 0.
	// The shader re-checks View.SkyAtmospherePresentInScene and falls back to SH if the two ever disagree.
	bool SkyAtmosphereRendered(const FSceneView& View)
	{
		static IConsoleVariable* const SkyAtmosphereCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.SkyAtmosphere"));
		const FSceneInterface* const Scene = View.Family->Scene;
		return Scene && Scene->GetSkyAtmosphereSceneInfo() != nullptr && View.Family->EngineShowFlags.Atmosphere
			&& FReadOnlyCVARCache::SupportSkyAtmosphere() && (!SkyAtmosphereCVar || SkyAtmosphereCVar->GetInt() > 0);
	}

	// Public sky sources (r.FogMS.World.SkySource 0 / 2 / 3 / 4): FFogMSWorldRequest::Sky (game thread) and the View UB only.
	// Output contract (unchanged from the removed private renderer path): FogMS_WorldSky returns SourceRadiance * View.SkyLightColor *
	// FogMSWorldSkyIntensity with SourceRadiance in "processed sky texture" units, i.e. what the engine itself multiplies
	// by View.SkyLightColor (ReflectionEnvironmentShared.ush:49 cubemap, VolumetricFog.usf:1035 SH).
	bool BindSkyPublic(FRDGBuilder& GraphBuilder, const FSceneView& View, const FFogMSWorldSky& Sky, int32 Mode,
		FFogMSWorldSourcesParameters& Parameters, FString& OutSkySource, FString& Error)
	{
		const FEngineShowFlags& Show = View.Family->EngineShowFlags;
		if (!Sky.bValid || !Show.Lighting || !Show.SkyLighting)
		{
			OutSkySource = Sky.bValid ? TEXT("none (sky lighting show flag off)") : TEXT("none (no visible sky light)");
			return true;
		}
		// Same gates as the removed private path (render-thread sky proxy), from the game-thread values the proxy is built from: proxy LightColor =
		// FLinearColor(LightColor) * Intensity (SkyLightComponent.cpp:274) = ULightComponentBase::GetLightColor(), and
		// VolumetricScatteringIntensity (SkyLightComponent.cpp:251, 493-500). GSkylightIntensityMultiplier and the protected
		// SpecifiedCubemapColorScale are not visible here; they only scale View.SkyLightColor, which stays the multiplier.
		const float Intensity = Sky.VolumetricScatteringIntensity;
		const FVector3f GateColor(Sky.LightColor * View.SkylightScale);
		if (!FMath::IsFinite(Intensity) || Intensity < 0 || !Nonnegative(GateColor))
		{
			Error = TEXT("B1 sky has invalid color or volumetric intensity");
			return false;
		}
		if (Intensity == 0 || GateColor.IsZero())
		{
			OutSkySource = TEXT("none (zero sky light intensity)");
			return true;
		}
		if (!Nonnegative(GateColor * Intensity))
		{
			Error = TEXT("B1 sky color overflows after volumetric intensity");
			return false;
		}

		const bool bAtmosphere = SkyAtmosphereRendered(View);
		const bool bCapture = Sky.ProcessedTexture.IsValid() && Sky.ProcessedSampler.IsValid()
			&& Sky.ProcessedTexture->GetDesc().Dimension == ETextureDimension::TextureCube;
		uint32 Source = SkySourceSH;
		const TCHAR* Fallback = nullptr;
		// W49 (FogMS_Weather_Design.md 4.2): with a FogMS Weather sky dome the Real Time Capture renders the dome (Is Sky mesh: the atmosphere
		// AND the weather clouds, ReflectionEnvironmentRealTimeCapture.cpp 'If there are any mesh tagged as IsSky then we render them only');
		// the Sky View LUT still holds the cloudless atmosphere. The capture's SH (View.SkyIrradianceEnvironmentMap, the same SH that lights
		// the world and the fog) is then the sky: overcast darkens and greys the light inside the Boxes as it does outside.
		const bool bWeatherCapture = Sky.bRealTimeCapture && Sky.bWeatherSky;
		switch (Mode)
		{
		case 2:
			if (bAtmosphere) Source = SkySourceViewLut;
			else Fallback = TEXT("Sky View LUT requested, no rendered SkyAtmosphere");
			break;
		case 3:
			if (bCapture) Source = SkySourceCubemap;
			else Fallback = TEXT("processed capture requested, none ready");
			break;
		case 4:
			break;
		default: // 0 = auto
			if (bWeatherCapture) Source = SkySourceSH;
			else if (Sky.bRealTimeCapture && bAtmosphere) Source = SkySourceViewLut;
			else if (!Sky.bRealTimeCapture && bCapture) Source = SkySourceCubemap;
			else Fallback = Sky.bRealTimeCapture ? TEXT("auto: Real Time Capture without a rendered SkyAtmosphere")
				: TEXT("auto: processed capture not ready");
			break;
		}

		Parameters.FogMSWorldSkyIntensity = Intensity;
		Parameters.FogMSWorldSkySource = Source;
		// View.SkyLightColor = GetEffectiveLightColor() * SkyPreExposureInv * SkylightScale, SkyPreExposureInv = 1 / cached
		// lighting pre-exposure when the proxy has RTC on (SceneRendering.cpp:2091-2101). The RTC cubemap and the RTC sky SH
		// are stored with exactly that exposure (OutputPreExposure = RealTimeReflectionCapturePreExposure in the capture,
		// SkyAtmosphere.usf:264), so it cancels for them. The Sky View LUT (divided by View.PreExposure in the shader) and a
		// static processed capture carry no such factor: multiply back by View.RealTimeReflectionCapturePreExposure, which
		// is the same cached value (SceneRendering.cpp:2076 vs 2094). ASSUMED: the game-thread RTC flag equals the proxy's
		// bRealTimeCaptureEnabled (both IsRealTimeCaptureEnabled(), SkyLightComponent.cpp:263; one frame of lag after a toggle).
		Parameters.FogMSWorldSkyUndoRTCExposure = Sky.bRealTimeCapture && Source != SkySourceSH ? 1.0f : 0.0f;
		if (Source == SkySourceViewLut)
		{
			// The RTC composites LowerHemisphereColor over world z < 0 (ReflectionEnvironmentRealTimeCapture.cpp:977-990 with
			// blend One/SourceAlpha, ReflectionEnvironmentShaders.usf:446-453); the LUT has no such term. Same coverage
			// saturate(a); same units (the capture scales it by the capture exposure that View.SkyLightColor removes).
			const FLinearColor& Lower = Sky.LowerHemisphereColor;
			const FVector4f LowerParam(Lower.R, Lower.G, Lower.B,
				Sky.bLowerHemisphereIsSolidColor && FMath::IsFinite(Lower.A) ? FMath::Clamp(Lower.A, 0.0f, 1.0f) : 0.0f);
			if (LowerParam.W > 0 && !Nonnegative(FVector3f(LowerParam.X, LowerParam.Y, LowerParam.Z)))
			{
				Error = TEXT("B1 sky lower hemisphere color is invalid");
				return false;
			}
			Parameters.FogMSWorldSkyLowerHemisphere = LowerParam;
			const uint32 LutSamples = uint32(FMath::Clamp(CVarWorldSkyLutSamples.GetValueOnRenderThread(), 1, 13));
			Parameters.FogMSWorldSkyLutSamples = LutSamples;
			OutSkySource = LutSamples > 1 ? FString::Printf(TEXT("Sky View LUT, %u-tap sector average"), LutSamples)
				: FString(TEXT("Sky View LUT, point sample"));
		}
		else if (Source == SkySourceCubemap)
		{
			// Public counterpart of the removed private processed-capture branch: RHI refs of the component's ProcessedSkyTexture
			// (USkyLightComponent::GetProcessedSkyTexture, SkyLightComponent.h:308), the resource the proxy's ProcessedTexture
			// points at. No blend: BlendFraction / BlendDestinationProcessedSkyTexture are protected (SkyLightComponent.h:333-338).
			Parameters.FogMSWorldSkyTexture = RegisterExternalTexture(GraphBuilder,
				Sky.ProcessedTexture, TEXT("FogMS.WorldSkyProcessed"), ERDGTextureFlags::SkipTracking);
			Parameters.FogMSWorldSkySampler = Sky.ProcessedSampler;
			Parameters.FogMSWorldSkyBlendSampler = Sky.ProcessedSampler;
			OutSkySource = Sky.bRealTimeCapture ? TEXT("processed capture (public; not the Real Time Capture)")
				: TEXT("processed capture (public)");
		}
		else
		{
			OutSkySource = bWeatherCapture ? TEXT("SH (Real Time Capture, weather clouds)") : TEXT("SH");
		}
		// W49: sources that miss (or freeze) the weather clouds of the sky dome.
		if (Sky.bWeatherSky && Source == SkySourceViewLut)
			OutSkySource += TEXT(" (the FogMS Weather clouds are not in the Sky View LUT: r.FogMS.World.SkySource 0 or 4 takes the capture's SH)");
		else if (Sky.bWeatherSky && !Sky.bRealTimeCapture)
			OutSkySource += TEXT(" (a static sky capture holds the FogMS Weather clouds of its last recapture only: tick Real Time Capture on the SkyLight)");
		if (Fallback) OutSkySource += FString::Printf(TEXT(" (fallback: %s)"), Fallback);
		if (Sky.Count > 1) OutSkySource += FString::Printf(TEXT(" [%d sky lights; first ASkyLight used]"), Sky.Count);
		return true;
	}
}

bool FogMS_GetWorldSources(FRDGBuilder& GraphBuilder, const FSceneView& View,
	FVector BoxCenterWS, FVector3f BoxExtent, const FFogMSWorldSky& Sky, const FFogMSWeatherLighting& Weather,
	FFogMSWorldSourcesParameters& OutParameters, FString& OutSkySource, FString& Error)
{
	check(IsInRenderingThread());
	Error.Empty();
	// Shader parameter structs have an empty generated constructor, not value
	// initialization of scalar fields (in particular the light count).
	FMemory::Memzero(&OutParameters, sizeof(OutParameters));
	// 2 = "never inside the cone" (dot of unit vectors <= 1); zero would exclude a hemisphere.
	OutParameters.FogMSWorldSunExcludeCos = 2.0f;
	// Memzero would make light 0 the sun: -1 = no atmosphere sun in the list (T_sun = 1 in transport pass 2).
	OutParameters.FogMSWorldSunLightIndex = -1;
	if (!View.Family || !View.Family->Scene || !View.ViewUniformBuffer.IsValid()
		|| BoxCenterWS.ContainsNaN() || !Finite(BoxExtent) || BoxExtent.GetMin() <= 0)
	{
		Error = TEXT("B1 world sources require a valid scene, View UB and Box bounds");
		return false;
	}
	const FScene& Scene = *static_cast<const FScene*>(View.Family->Scene);
	// Always bind valid fallback SRVs/samplers, including inactive weather and shader permutations
	// where the uniform branch survives compilation. Snapshots retain RHI resources through execution.
	OutParameters.FogMSWeatherMap = GSystemTextures.GetBlackDummy(GraphBuilder);
	OutParameters.FogMSWeatherSunMap = OutParameters.FogMSWeatherMap;
	OutParameters.FogMSWeatherTypeLUT = OutParameters.FogMSWeatherMap;
	OutParameters.FogMSWeatherPattern = OutParameters.FogMSWeatherMap;
	OutParameters.FogMSWeatherCurl = OutParameters.FogMSWeatherMap;
	OutParameters.FogMSWeatherWrapSampler = TStaticSamplerState<SF_Trilinear, AM_Wrap, AM_Wrap, AM_Wrap>::GetRHI();
	OutParameters.FogMSWeatherClampSampler = TStaticSamplerState<SF_Trilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	if (Weather.bActive)
	{
		const auto ValidTexture = [](const FTextureRHIRef& Texture)
		{
			return Texture.IsValid() && Texture->GetDesc().Dimension == ETextureDimension::Texture2D
				&& EnumHasAnyFlags(Texture->GetDesc().Flags, TexCreate_ShaderResource);
		};
		if (!ValidTexture(Weather.Map) || !ValidTexture(Weather.SunMap) || !ValidTexture(Weather.TypeLUT)
			|| !ValidTexture(Weather.Pattern) || !ValidTexture(Weather.Curl) || Weather.Origin.ContainsNaN()
			|| Weather.PlanetCenter.ContainsNaN() || !FMath::IsFinite(Weather.PlanetRadius) || Weather.PlanetRadius <= 0.0
			|| Weather.Domain.ContainsNaN() || Weather.Wind.ContainsNaN() || Weather.L0.ContainsNaN() || Weather.L1.ContainsNaN()
			|| Weather.Domain.X <= 0.f || Weather.Domain.Y <= 0.f || !Finite(Weather.DirectionToSun)
			|| !FMath::IsNearlyEqual(Weather.DirectionToSun.SizeSquared(), 1.f, 0.01f))
		{
			Error = TEXT("Active weather requires finite parameters, a unit sun direction and five resident 2D shader textures.");
			return false;
		}
		OutParameters.FogMSWeatherActive = 1u;
		OutParameters.FogMSWeatherOriginTranslated = FVector3f(Weather.Origin + View.ViewMatrices.GetPreViewTranslation());
		OutParameters.FogMSWeatherPlanetTopTranslated = FVector3f(Weather.PlanetCenter + FVector3d(0, 0, Weather.PlanetRadius)
			+ View.ViewMatrices.GetPreViewTranslation());
		OutParameters.FogMSWeatherPlanetRadius = float(Weather.PlanetRadius);
		OutParameters.FogMSWeatherDomain = Weather.Domain;
		OutParameters.FogMSWeatherWind = Weather.Wind;
		OutParameters.FogMSWeatherL0 = Weather.L0;
		OutParameters.FogMSWeatherL1 = Weather.L1;
		OutParameters.FogMSWeatherSunDirection = Weather.DirectionToSun;
		OutParameters.FogMSWeatherMap = RegisterExternalTexture(GraphBuilder, Weather.Map, TEXT("FogMS.WeatherMap"));
		OutParameters.FogMSWeatherSunMap = RegisterExternalTexture(GraphBuilder, Weather.SunMap, TEXT("FogMS.WeatherSunMap"));
		OutParameters.FogMSWeatherTypeLUT = RegisterExternalTexture(GraphBuilder, Weather.TypeLUT, TEXT("FogMS.WeatherTypeLUT"));
		OutParameters.FogMSWeatherPattern = RegisterExternalTexture(GraphBuilder, Weather.Pattern, TEXT("FogMS.WeatherPattern"));
		OutParameters.FogMSWeatherCurl = RegisterExternalTexture(GraphBuilder, Weather.Curl, TEXT("FogMS.WeatherCurl"));
	}
	ResetSky(GraphBuilder, OutParameters);
	OutSkySource.Empty();
	const int32 SkySource = CVarWorldSkySource.GetValueOnRenderThread();
	if (SkySource == 1)
	{
		// Value 1 was the private renderer sky path (P9), removed after the round 25 A/B; it now runs as 0 (auto).
		static bool bLoggedRetiredSkySource = false; // render thread only
		if (!bLoggedRetiredSkySource)
		{
			bLoggedRetiredSkySource = true;
			UE_LOG(LogFogMSWorldSources, Warning, TEXT("r.FogMS.World.SkySource 1 (private renderer sky path) was removed; using 0 (auto)."));
		}
	}
	if (!BindSkyPublic(GraphBuilder, View, Sky, SkySource >= 2 && SkySource <= 4 ? SkySource : 0, OutParameters, OutSkySource, Error))
		return false;
	const float SkyMipBias = CVarWorldSkyMipBias.GetValueOnRenderThread();
	OutParameters.FogMSWorldSkyMipBias = FMath::IsFinite(SkyMipBias) ? SkyMipBias : 0.0f;

	static const IConsoleVariable* const BiasCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.VolumetricFog.InverseSquaredLightDistanceBiasScale"));
	const float BiasScale = BiasCVar ? BiasCVar->GetFloat() : 1.0f;
	if (!FMath::IsFinite(BiasScale) || BiasScale < 0)
	{
		Error = TEXT("B1 inverse-square distance bias scale is invalid");
		return false;
	}
	// Native fog uses the distance to the next diagonal cell, not its half extent.
	// Replace camera froxel size by the fixed world cell size to keep this stable.
	const float BoxRadius = BoxExtent.Size();
	const float BiasDistance = FMath::Max(2.0f * BoxRadius / SourceGridSize * BiasScale, 1.0f);
	OutParameters.FogMSWorldDistanceBiasSqr = BiasDistance * BiasDistance;
	if (!FMath::IsFinite(OutParameters.FogMSWorldDistanceBiasSqr))
	{
		Error = TEXT("B1 world grid distance bias overflows");
		return false;
	}

	TArray<FVector4f> Rows;
	Rows.Reserve(MaxWorldLights * LightRows);
	const FEngineShowFlags& Show = View.Family->EngineShowFlags;
	// The exported FSceneView accessor (not the unexported FViewInfo overload; both read the same
	// EyeAdaptationViewState and return 0 when absent, SceneRendering.cpp:2609; SceneView.cpp:2967).
	// Preserve that exact exposure for inverse-exposure-blended lights, rather than using PreExposure.
	const float Exposure = View.GetLastEyeAdaptationExposure();
	// Sun for the sky exclusion cone. Must be a light that actually enters the list
	// (pass 2 / FogMS_WorldLight shadows it), otherwise the captured disc is the only
	// sun term and must stay. Prefer Scene.AtmosphereLights[0]: the render-thread
	// mirror of bAtmosphereSunLight/AtmosphereSunLightIndex 0, which BoxRuntime uses
	// for DirectionToSun (the aligned transport ordinate) and whose disc the sky
	// capture renders. Otherwise the first accepted directional light.
	const FLightSceneInfo* const AtmosphereSun = Scene.AtmosphereLights[0];
	FVector3f SunDirection = FVector3f::ZeroVector;
	bool bSunIsAtmosphereLight = false;
	FString CloudShadowNote; // W47: status note for a foreign cloud's shadows (appended to OutSkySource below)
	for (const FLightSceneInfoCompact& Compact : Scene.Lights)
	{
		const FLightSceneInfo* Info = Compact.LightSceneInfo;
		if (!Show.Lighting || !Info || !Info->bVisible || !Info->Proxy) continue;
		const FLightSceneProxy& Proxy = *Info->Proxy;
		const uint8 Type = Proxy.GetLightType();
		if ((Type == LightType_Directional && !Show.DirectionalLights)
			|| (Type == LightType_Point && !Show.PointLights)
			|| (Type == LightType_Spot && !Show.SpotLights)
			|| (Type == LightType_Rect && !Show.RectLights)) continue;

		const float VolumeIntensity = Proxy.GetVolumetricScatteringIntensity();
		if (VolumeIntensity == 0 || FVector3f(Proxy.GetColor()).IsZero()) continue;
		FLightRenderParameters Light{};
		Proxy.GetLightShaderParameters(Light);
		const bool bDirectional = Type == LightType_Directional;
		if (!bDirectional && FMath::IsFinite(Light.InvRadius) && Light.InvRadius > 0
			&& FVector::DistSquared(BoxCenterWS, Light.WorldPosition) > FMath::Square(double(BoxRadius) + 1.0 / Light.InvRadius)) continue;

		auto Reject = [&Error, &Proxy](const TCHAR* Reason)
		{
			Error = FString::Printf(TEXT("B1 world source '%s': %s"), *Proxy.GetOwnerNameOrLabel(), Reason);
			return false;
		};
		if (!FMath::IsFinite(VolumeIntensity) || VolumeIntensity < 0
			|| Light.WorldPosition.ContainsNaN() || !Nonnegative(FVector3f(Light.Color))
			|| !Finite(Light.Direction) || Light.Direction.IsNearlyZero()
			|| !Finite(Light.Tangent) || !FMath::IsFinite(Light.SourceLength) || Light.SourceLength < 0
			|| !FMath::IsFinite(Light.FalloffExponent) || Light.FalloffExponent < 0
			|| (!bDirectional && (!FMath::IsFinite(Light.InvRadius) || Light.InvRadius <= 0))
			|| !FMath::IsFinite(Light.SpotAngles.X) || !FMath::IsFinite(Light.SpotAngles.Y))
			return Reject(TEXT("non-finite or invalid light parameters"));
		if (bDirectional)
		{
			// Atmosphere sun light: GetColor() is the outer-space illuminance. The engine applies the atmosphere
			// transmittance either per pixel in its shaders or by scaling the light colour (DirectionalLightComponent.cpp,
			// GetSunIlluminanceAccountingForSkyAtmospherePerPixelTransmittance). The transport shaders evaluate no
			// per-pixel transmittance, so always take the on-ground illuminance: below the horizon the sun fades with the
			// native sky and clouds instead of lighting the Box from below at night. Non-atmosphere lights: White transmittance.
			Light.Color = Proxy.GetSunIlluminanceOnGroundPostTransmittance();
		}
		const float ExposureScale = Light.GetLightExposureScale(Exposure);
		const FVector3f Color = FVector3f(Light.Color) * (ExposureScale * VolumeIntensity);
		if (!Nonnegative(Color)) return Reject(TEXT("invalid exposure-scaled color"));
		if (Color.IsZero()) continue;
		if (Type != LightType_Directional && Type != LightType_Point && Type != LightType_Spot)
			return Reject(TEXT("rect/unknown light type is not supported"));
		if (Proxy.HasStaticLighting()) return Reject(TEXT("baked-static source requires a separate lightmap provider"));
		if (Proxy.GetIESTexture()) return Reject(TEXT("IES profile is not supported"));
		if (Show.LightFunctions && Proxy.GetLightFunctionMaterial()) return Reject(TEXT("light function is not supported"));
		// W47 (P4 part 1; FogMS_Weather_Design.md 3.10, 4.1): a sun with Cast Cloud Shadows is accepted; it used to fail the whole
		// Box. Each transmittance is applied once per light path, so nothing is counted twice:
		//  - this light's colour (above) is GetSunIlluminanceOnGroundPostTransmittance() = outer-space illuminance x the atmosphere's
		//    transmittance at ground level, a CPU value per light (PrepareSunLightProxy, SkyAtmosphereRendering.cpp:584-594); the
		//    engine's cloud shadow map (Beer shadow map, BSM) is never part of it. The BSM is applied per pixel only by its consumers
		//    (deferred lights, DeferredLightPixelShaders.usf:187-204; Volumetric Fog, VolumetricFog.usf:969-974; Lumen scene lighting,
		//    translucency, SkyAtmosphere). The solver binds no BSM;
		//  - transport pass 2 attenuates this sun per cell by T_sun = RT visibility (the TLAS: opaque geometry only, a Volumetric
		//    Cloud is not in it) x the transmittance of the Box's OWN medium. For a Box the FogMS cloud host renders, the host's BSM
		//    holds exactly that medium: the solver applies it once (T_sun) to the Box's cells, the engine applies the BSM to the other
		//    receivers (ground, height fog and froxel media, Lumen scene, atmosphere) and not to the host itself (M_FogMS_Cloud
		//    ray-marches its own sun shadow per step, bRayMarchVolumeShadow: VolumetricCloud.usf:1069 reads the BSM only without it);
		//  - light paths that now see the Box's shadow are separate paths and correct: with Lumen Bounce Auto the ground under the
		//    Box is darker in the Lumen surface cache (less bounce into the field); with Cloud Shadow On Atmosphere > 0 the Sky View
		//    LUT (the sky source with Real Time Capture) holds the shadowed air (SkyAtmosphere.usf:739-742; tiny for a Box). Lumen
		//    Bounce Off shades the ground with the Box medium itself (r.FogMS.World.FallbackMedium), no BSM: the field is the same
		//    with and without the flag (round 47 check, frozen sky).
		// A cloud that is not a FogMS host (the engine's sky clouds) casts shadows this solver does not see: the Box keeps working and
		// the status says so. A host that renders another Box (one Box per host) is reported by that host status ('busy').
		if (bDirectional && Proxy.IsUsedAsAtmosphereSunLight() && Proxy.GetCastCloudShadows() && Proxy.GetCloudShadowStrength() > 0
			&& Scene.VolumetricCloud && Show.Atmosphere && Show.Cloud && CloudShadowNote.IsEmpty())
		{
			FString CloudMaterial;
			if (!CloudIsFogMSHost(Scene, CloudMaterial))
			{
				CloudShadowNote = FString::Printf(TEXT(" [sun cloud shadows: Volumetric Cloud '%s' is not a FogMS cloud host: its shadow darkens the ground and the fog, not this Box's solver field]"),
					*CloudMaterial);
				static FString LoggedMaterial; // render thread only: one line per foreign cloud material
				if (LoggedMaterial != CloudMaterial)
				{
					LoggedMaterial = CloudMaterial;
					UE_LOG(LogFogMSWorldSources, Warning, TEXT("FogMS: the sun '%s' casts cloud shadows of Volumetric Cloud material '%s', which is not a FogMS cloud host: the FogMS solver does not see those shadows (its sun is shadowed by the Box's own medium only). The Box keeps working."),
						*Proxy.GetOwnerNameOrLabel(), *CloudMaterial);
				}
			}
		}
		if (OutParameters.FogMSWorldNumLights >= MaxWorldLights)
			return Reject(TEXT("more than 256 relevant lights; source is not truncated"));

		const FVector3f Position(Light.WorldPosition + View.ViewMatrices.GetPreViewTranslation());
		if (!Finite(Position)) return Reject(TEXT("translated position overflows"));
		const bool bCastShadow = Show.DynamicShadows && Proxy.CastsVolumetricShadow()
			&& (Proxy.CastsDynamicShadow() || Proxy.CastsStaticShadow());
		Rows.Add(FVector4f(Position, bDirectional ? 0.0f : Light.InvRadius));
		Rows.Add(FVector4f(Light.Direction.GetSafeNormal(), bDirectional ? 0.0f : (Type == LightType_Spot ? 2.0f : 1.0f)));
		Rows.Add(FVector4f(Color, Proxy.IsInverseSquared() ? 1.0f : 0.0f));
		Rows.Add(FVector4f(Light.SpotAngles.X, Light.SpotAngles.Y, Light.FalloffExponent, bCastShadow ? 1.0f : 0.0f));
		Rows.Add(FVector4f(Light.Tangent.GetSafeNormal(), Light.SourceLength));
		// Explicit index of the atmosphere sun's row block (no direction matching in the shader): transport pass 2
		// evaluates exactly this light's shadow ray and medium transmittance into T_sun for hybrid injection.
		if (bDirectional && Info == AtmosphereSun) OutParameters.FogMSWorldSunLightIndex = int32(OutParameters.FogMSWorldNumLights);
		++OutParameters.FogMSWorldNumLights;
		if (bDirectional && !bSunIsAtmosphereLight && (SunDirection.IsZero() || Info == AtmosphereSun))
		{
			// Identical value to the Direction row above: toward the sun (engine
			// DirectionalLightComponent sets LightParameters.Direction = -GetDirection()).
			SunDirection = Light.Direction.GetSafeNormal();
			bSunIsAtmosphereLight = Info == AtmosphereSun;
		}
	}
	if (Rows.IsEmpty()) Rows.Add(FVector4f(0, 0, 0, 0));
	OutParameters.FogMSWorldLights = GraphBuilder.CreateSRV(CreateStructuredBuffer(GraphBuilder, TEXT("FogMS.WorldLights"), Rows));
	// The Box status shows OutSkySource as ' [sky: ...]'; the foreign-cloud note rides along (like the sky-light count note).
	OutSkySource += CloudShadowNote;

	OutParameters.FogMSWorldSunDirection = SunDirection;
	OutParameters.FogMSWorldSunExcludeCos = 2.0f;
	const float ExcludeDegrees = CVarWorldSunExcludeDegrees.GetValueOnRenderThread();
	if (!SunDirection.IsZero() && FMath::IsFinite(ExcludeDegrees) && ExcludeDegrees > 0.0f)
	{
		OutParameters.FogMSWorldSunExcludeCos = FMath::Cos(FMath::DegreesToRadians(FMath::Min(ExcludeDegrees, 30.0f)));
	}
	return true;
}
