#include "FogMS_WorldSources.h"

#include "HAL/IConsoleManager.h"
#include "LightSceneInfo.h"
#include "LightSceneProxy.h"
#include "PooledRenderTarget.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "RHIStaticStates.h"
#include "SceneManagement.h"
#include "ScenePrivate.h"
#include "SceneProxies/SkyLightSceneProxy.h"
#include "SceneRendering.h"
#include "SceneView.h"
#include "SystemTextures.h"
#include "TextureResource.h"

namespace
{
	constexpr int32 MaxWorldLights = 256;
	constexpr int32 LightRows = 5;
	constexpr float SourceGridSize = 32.0f;

	TAutoConsoleVariable<float> CVarWorldSunExcludeDegrees(TEXT("r.FogMS.World.SunExcludeDegrees"), 3.0f,
		TEXT("Half-angle in degrees of the cone around the atmosphere sun that is removed from the captured sky radiance used as transport/world boundary. Direct sun is accounted separately with shadows; 0 disables."), ECVF_RenderThreadSafe);
	TAutoConsoleVariable<float> CVarWorldSkyMipBias(TEXT("r.FogMS.World.SkyMipBias"), 0.0f,
		TEXT("Added to the sector-matched sky cubemap mip used as transport/world boundary radiance. Negative sharpens, positive blurs."), ECVF_RenderThreadSafe);

	bool Finite(const FVector3f& Value)
	{
		return FMath::IsFinite(Value.X) && FMath::IsFinite(Value.Y) && FMath::IsFinite(Value.Z);
	}

	bool Nonnegative(const FVector3f& Value)
	{
		return Finite(Value) && Value.X >= 0 && Value.Y >= 0 && Value.Z >= 0;
	}

	bool BindSky(FRDGBuilder& GraphBuilder, const FScene& Scene, const FViewInfo& View,
		FFogMSWorldSourcesParameters& Parameters, FString& Error)
	{
		Parameters.FogMSWorldSkyTexture = GSystemTextures.GetCubeBlackDummy(GraphBuilder);
		Parameters.FogMSWorldSkyBlendTexture = Parameters.FogMSWorldSkyTexture;
		Parameters.FogMSWorldSkySampler = TStaticSamplerState<SF_Trilinear>::GetRHI();
		Parameters.FogMSWorldSkyBlendSampler = Parameters.FogMSWorldSkySampler;
		Parameters.FogMSWorldSkyColor = FVector3f::ZeroVector;
		Parameters.FogMSWorldSkyBlend = 0;
		if (!Scene.SkyLight || !View.Family->EngineShowFlags.Lighting || !View.Family->EngineShowFlags.SkyLighting)
			return true;

		const FSkyLightSceneProxy& Sky = *Scene.SkyLight;
		const float Intensity = Sky.VolumetricScatteringIntensity;
		const FVector3f NativeColor(View.CachedViewUniformShaderParameters->SkyLightColor);
		if (!FMath::IsFinite(Intensity) || Intensity < 0 || !Nonnegative(NativeColor))
		{
			Error = TEXT("B1 sky has invalid color or volumetric intensity");
			return false;
		}
		if (Intensity == 0 || NativeColor.IsZero()) return true;
		Parameters.FogMSWorldSkyColor = NativeColor * Intensity;
		if (!Nonnegative(Parameters.FogMSWorldSkyColor))
		{
			Error = TEXT("B1 sky color overflows after volumetric intensity");
			return false;
		}

		// Narrow equivalent of IndirectLightRendering.cpp:660-731. Calling its
		// private SetupReflectionUniformParameters would require a Renderer export.
		// SceneRendering.cpp:2091-2101 already removes RTC cached pre-exposure from
		// View.SkyLightColor; do not multiply or divide by View.PreExposure here.
		const int32 ReadyIndex = Scene.ConvolvedSkyRenderTargetReadyIndex;
		if (Sky.bRealTimeCaptureEnabled && ReadyIndex >= 0)
		{
			if (ReadyIndex > 1 || !Scene.ConvolvedSkyRenderTarget[ReadyIndex].IsValid())
			{
				Error = TEXT("B1 realtime sky capture has no valid ready cubemap");
				return false;
			}
			Parameters.FogMSWorldSkyTexture = GraphBuilder.RegisterExternalTexture(
				Scene.ConvolvedSkyRenderTarget[ReadyIndex], TEXT("FogMS.WorldSkyRTC"));
			return true;
		}

		if (!Sky.ProcessedTexture || !Sky.ProcessedTexture->TextureRHI.IsValid()
			|| !Sky.ProcessedTexture->SamplerStateRHI.IsValid())
		{
			Error = TEXT("B1 sky capture/cubemap is not ready");
			return false;
		}
		if (!FMath::IsFinite(Sky.BlendFraction) || Sky.BlendFraction < 0 || Sky.BlendFraction > 1)
		{
			Error = TEXT("B1 sky cubemap blend fraction is invalid");
			return false;
		}
		FTexture* Source = Sky.ProcessedTexture;
		if (Sky.BlendFraction > 0)
		{
			FTexture* Destination = Sky.BlendDestinationProcessedTexture;
			if (!Destination || !Destination->TextureRHI.IsValid() || !Destination->SamplerStateRHI.IsValid())
			{
				Error = TEXT("B1 sky cubemap blend destination is not ready");
				return false;
			}
			if (Sky.BlendFraction == 1)
			{
				Source = Destination;
			}
			else
			{
				Parameters.FogMSWorldSkyBlendTexture = RegisterExternalTexture(GraphBuilder,
					Destination->TextureRHI, TEXT("FogMS.WorldSkyBlend"), ERDGTextureFlags::SkipTracking);
				Parameters.FogMSWorldSkyBlendSampler = Destination->SamplerStateRHI;
				Parameters.FogMSWorldSkyBlend = Sky.BlendFraction;
			}
		}
		Parameters.FogMSWorldSkyTexture = RegisterExternalTexture(GraphBuilder,
			Source->TextureRHI, TEXT("FogMS.WorldSkyCapture"), ERDGTextureFlags::SkipTracking);
		Parameters.FogMSWorldSkySampler = Source->SamplerStateRHI;
		return true;
	}
}

bool FogMS_GetWorldSources(FRDGBuilder& GraphBuilder, const FViewInfo& View,
	FVector BoxCenterWS, FVector3f BoxExtent,
	FFogMSWorldSourcesParameters& OutParameters, FString& Error)
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
	if (!View.Family || !View.Family->Scene || !View.CachedViewUniformShaderParameters
		|| BoxCenterWS.ContainsNaN() || !Finite(BoxExtent) || BoxExtent.GetMin() <= 0)
	{
		Error = TEXT("B1 world sources require a valid scene, View UB and Box bounds");
		return false;
	}
	const FScene& Scene = *static_cast<const FScene*>(View.Family->Scene);
	if (!BindSky(GraphBuilder, Scene, View, OutParameters, Error)) return false;
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
	// The FViewInfo overload is not exported. Its implementation and the exported
	// FSceneView accessor read the same EyeAdaptationViewState and return 0 when
	// absent (SceneRendering.cpp:2609; SceneView.cpp:2967). Preserve that exact
	// exposure for inverse-exposure-blended lights, rather than using PreExposure.
	const float Exposure = View.FSceneView::GetLastEyeAdaptationExposure();
	// Sun for the sky exclusion cone. Must be a light that actually enters the list
	// (pass 2 / FogMS_WorldLight shadows it), otherwise the captured disc is the only
	// sun term and must stay. Prefer Scene.AtmosphereLights[0]: the render-thread
	// mirror of bAtmosphereSunLight/AtmosphereSunLightIndex 0, which BoxRuntime uses
	// for DirectionToSun (the aligned transport ordinate) and whose disc the sky
	// capture renders. Otherwise the first accepted directional light.
	const FLightSceneInfo* const AtmosphereSun = Scene.AtmosphereLights[0];
	FVector3f SunDirection = FVector3f::ZeroVector;
	bool bSunIsAtmosphereLight = false;
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
		if (bDirectional && Scene.VolumetricCloud && Show.Atmosphere && Show.Cloud && Proxy.GetCastCloudShadows())
			return Reject(TEXT("native volumetric-cloud shadow visibility is not bound"));
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

	OutParameters.FogMSWorldSunDirection = SunDirection;
	OutParameters.FogMSWorldSunExcludeCos = 2.0f;
	const float ExcludeDegrees = CVarWorldSunExcludeDegrees.GetValueOnRenderThread();
	if (!SunDirection.IsZero() && FMath::IsFinite(ExcludeDegrees) && ExcludeDegrees > 0.0f)
	{
		OutParameters.FogMSWorldSunExcludeCos = FMath::Cos(FMath::DegreesToRadians(FMath::Min(ExcludeDegrees, 30.0f)));
	}
	return true;
}
