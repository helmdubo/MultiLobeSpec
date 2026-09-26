"""FogMS P2 (FogMS_PerPixelClouds_Design.md section 4 'P2'): the cloud-host material /MultiLobeSpec/FogMS/M_FogMS_Cloud and its
instance /MultiLobeSpec/FogMS/MI_FogMS_Cloud, created or updated idempotently. Productized from the P1 prototype material
(cloudproto_material.py, M_FogMS_CloudBox_P1): same graph and HLSL, host parameters renamed P1_* -> FogMS_Cloud*, the host phase g
read from the Box's FogMS_ForwardG (Phase G), FogMS_Density default 0 (a host that no Box feeds is empty), field-only debug (mode 3)
without the cloud's own sun. Run in the editor with no other session editing these assets: `py "<this file>"` in the Python
console, or through the UE-MCP bridge with runpy (d45_p2.py matedit). FOGMS_MATEDIT_MODE=report only prints the check.

Who feeds it: the C++ cloud host (Source/MultiLobeSpec/Private/FogMS_CloudHost.cpp, UFogMSCloudHostSubsystem) finds an
AVolumetricCloud whose material's base material is M_FogMS_Cloud, wraps its material in a MID and the Box with Render Path = Cloud
Host writes into that MID every update: the same FogMS_* parameters as its own M_FogMS_Density MID (FogMS_BoxVolume.cpp,
FogMS_WriteDensityParameters) plus FogMS_CloudBoxCenter / FogMS_CloudWorldToLocal0..2 (the density cube's placement).

Data flow per ray-march step of the engine Volumetric Cloud (VolumetricCloud.usf, RenderVolumetricCloudRenderViewCS):
  WorldPosition (default mode = the step's absolute world position; the cloud does not set the *_NoOffsets positions)
    - FogMS_CloudBoxCenter [cm] -> FogMS_CloudBoxLocal (rows FogMS_CloudWorldToLocal0..2 = world axis i of the density cube divided by
      its world scale, offset in w) = LocalPosition in cube space -50..50 (M_FogMS_Density: TransformPosition World -> Local)
    -> three noise UVW nodes (M_FogMS_Density's UVW Custom nodes, verbatim) -> FogMS_Noise at mip 0, three times
    -> FogMS_Extinction_v3 (EXTINCTION_CODE_V3 of matedit_density.py) = sigma_t [1/m] -> SubsurfaceColor (the cloud's extinction)
       Footprint <- FogMS_CloudFootprint (v2, W46 anti-dither, Nubis-style): w = FogMS_CloudPrefilter * max(FogMS_CloudStep, traced
       pixel width at |WorldPosition - CameraPositionWS|); Wavelengths <- FogMS_PrefilterWavelengths (the Box's noise feature sizes).
       FogMS_CloudPrefilter 0 (material default) = w 0: the solver's formula, the v1 material exactly.
  FogMS_CloudAlbedo: FogMS_Albedo, 0 in mode 3 (Field Only debug: the solver's full field alone) -> BaseColor: the cloud scatters
    Albedo * sigma_t of the sun itself, with a march toward the sun on every step (Volumetric Advanced Output, ray-marched volume
    shadow) and the host phase HG(Phase G = FogMS_ForwardG, FogMS_CloudPhaseG2, FogMS_CloudPhaseBlend).
  FogMS_TransportField at LocalPosition * 0.01 + 0.5 (hybrid: RGB = J minus the uncollided sun, A = 0.5 + 0.5 * T_sun * k)
    -> FogMS_EmissiveInjection (valid = Mode > 0.5 and A >= 0.5; valid * Field * Albedo * sigma_t * FogMS_CloudFieldGain)
    -> FogMS_ForwardLobe (FORWARD_LOBE_CODE_V2 of matedit_density.py; FieldA <- the field's ALPHA: S = T_sun * k) -> Emissive
       (the cloud adds Emissive as luminance per metre: the units of sigma_s[1/m] * J, the value the Box material gives the fog).
  FogMS_CloudSkyAO: 0 with a valid field (the field carries the sky with the medium's self-shadowing), 1 without -> AmbientOcclusion.
  FogMS_CloudConservative -> ConservativeDensity.x: 1 inside the density cube (and the height-profile band when detail cannot reach
    the threshold band) with FogMS_Density > 0, else 0: empty steps and an unfed host skip the material.
  Volumetric Advanced Output: phase per pixel, multiple-scattering octaves 0 (Emissive is added once per octave), ground
  contribution off, ray-marched volume shadow on, colour (not grey-scale).
Material: Volume, Additive, DefaultLit (Unlit zeroes albedo and extinction), Used with Volumetric Cloud.

Links are checked definitively (the W40 approach of matedit_density.py): every Custom node pin is read back from the node's T3D
export (stored OutputIndex -> the source's output name) and must be exactly the intended (source, output); no output name is ever
inferred from another pin. Idempotent: an existing material whose FogMS_CloudBoxLocal carries MARKER, whose Custom codes are current,
whose links and settings verify, prints ALREADY_PATCHED and changes nothing (the instance is created if missing). Otherwise the graph
is rebuilt in place and recompiled; the editor log is scanned for 'Failed to compile Material ... M_FogMS_Cloud'. On any error
nothing is saved: an asset with a file on disk is reloaded from it (rollback), a never-saved one is emptied in memory (the next run
rebuilds it). Expressions are deleted one by one (delete_all_material_expressions left some behind in the first round-45 run).
Versions: v1 (round 45) = Footprint and Wavelengths were constants 0; v2 (W46) adds FogMS_CloudFootprint, FogMS_CloudPrefilter,
FogMS_CloudStep and FogMS_PrefilterWavelengths. A v1 asset fails the v2 check (marker, missing node) and is rebuilt in place (upgrade);
a v2 run on a v2 asset prints ALREADY_PATCHED. The Box / subsystem write the new parameters only from round 46 on; a v2 material with
an older plugin keeps FogMS_CloudPrefilter 0 = the v1 image.
v3 (W48, FogMS_Weather_Design.md 3.2, 3.5, 4.1): the FogMS Weather in the cloud SHADOW pass only.
  MP_SUBSURFACE_COLOR <- Shadow Pass Switch(Default: FogMS_Extinction_v3 (the hero Box, as v2),
                                            Shadow:  FogMS_CloudShadowSum = FogMS_Extinction_v3 + FogMS_WeatherExtinction)
  VAO ConservativeDensity <- Shadow Pass Switch(Default: FogMS_CloudConservative v2 (the Box's region grown by FogMS_CloudSkipMargin),
                                                Shadow:  FogMS_CloudShadowConservative = max(that, FogMS_WeatherConservative))
  The engine compiles the switch per pass: GetShadowReplaceState() is 1 only where SHADOW_DEPTH_SHADER is 1, i.e. the cloud shadow map /
  sky AO tracing (FVolumetricCloudShadowPS, VolumetricCloudRendering.cpp:1405); the view tracing and the cloud's own per-step sun march see
  the hero Box only. FogMS_WeatherExtinction evaluates FogMS_WeatherFn (WEATHER_FN_CODE, shared verbatim with M_FogMS_WeatherSun of
  matedit_weather.py) at the sample: WorldPosition - FogMS_WeatherOrigin - FogMS_WeatherWind in the weather domain, altitude = Cloud Sample
  Attributes.Altitude (cm above the ground of the cloud layer). Thin layer (FogMS_WeatherThin 1): the column optical depth of
  RT_FogMS_WeatherSun at the sample's ground point on its sun line, spread over the host layer's path along the sun.
  Parameters (written by UFogMSCloudHostSubsystem, FogMS_CloudHost.cpp W48): FogMS_WeatherOn / _Thin, FogMS_WeatherOrigin / _Domain / _Wind /
  _L0 / _L1 / _SunDir, texture objects FogMS_WeatherMap (RT_FogMS_WeatherMap), _SunMap (RT_FogMS_WeatherSun), _TypeLUT, _Pattern, _Curl;
  FogMS_CloudSkipMargin. Defaults: weather off, margin 0: the v2 image exactly (hero + 0.0 in the shadow pass; the conservative mask
  unchanged). The weather textures (/MultiLobeSpec/FogMS/Weather/T_FogMS_*) come from matedit_weather.py: run it first.
  A v2 asset fails the v3 check (marker, missing nodes) and is rebuilt in place.
Output: MATERIAL_OK saved=<bool> ... | ALREADY_PATCHED ... | TRACE <traceback>."""
import ast
import glob
import os
import re
import shlex
import tempfile
import time
import traceback

import unreal

ASSET_DIR = '/MultiLobeSpec/FogMS'
MATERIAL_NAME = 'M_FogMS_Cloud'
INSTANCE_NAME = 'MI_FogMS_Cloud'
MATERIAL_PATH = ASSET_DIR + '/' + MATERIAL_NAME
INSTANCE_PATH = ASSET_DIR + '/' + INSTANCE_NAME
MARKER = 'FogMS_Cloud v3'
DEFAULT_VOLUME = '/MultiLobeSpec/FogMS/T_FogMS_DefaultVolume'
DENSITY_MATERIAL = '/MultiLobeSpec/FogMS/M_FogMS_Density'
GROUP = 'FogMS Cloud'
WEATHER_GROUP = 'FogMS Weather'
# W48 texture objects of the weather branch: (parameter, default texture). The weather map / sun map defaults are black (branch inert);
# the LUT / pattern / curl are the matedit_weather.py imports.
BLACK_TEXTURE = '/Engine/EngineResources/Black'
WEATHER_TEXTURES = (('FogMS_WeatherMap', BLACK_TEXTURE), ('FogMS_WeatherSunMap', BLACK_TEXTURE),
                    ('FogMS_WeatherTypeLUT', '/MultiLobeSpec/FogMS/Weather/T_FogMS_CloudTypeLUT'),
                    ('FogMS_WeatherPattern', '/MultiLobeSpec/FogMS/Weather/T_FogMS_WeatherPattern'),
                    ('FogMS_WeatherCurl', '/MultiLobeSpec/FogMS/Weather/T_FogMS_Curl2D'))
mel = unreal.MaterialEditingLibrary

# (name, default). FogMS_* names and meanings are the Box MID's (FogMS_BoxVolume.cpp); FogMS_Density default 0 = empty host.
SCALARS = (('FogMS_WorldAligned', 0.0), ('FogMS_DetailScale', 4.0), ('FogMS_Threshold', 0.5), ('FogMS_Softness', 0.1),
           ('FogMS_Density', 0.0), ('FogMS_DensityFeather', 100.0), ('FogMS_DetailStrength', 0.0), ('FogMS_DetailSecondOctave', 0.5),
           ('FogMS_InjectionMode', 0.0), ('FogMS_ErosionStrength', 0.0), ('FogMS_ErosionDepth', 0.15), ('FogMS_HeightProfile', 0.0),
           ('FogMS_HeightBottom', 0.0), ('FogMS_HeightTop', 1.0), ('FogMS_HeightBottomSoftness', 0.05),
           ('FogMS_HeightTopSoftness', 0.1), ('FogMS_HeightAnvilStrength', 0.0), ('FogMS_ForwardStrength', 0.0),
           ('FogMS_ForwardG', 0.6), ('FogMS_ForwardDepth', 0.5), ('FogMS_ForwardFloor', 0.25), ('FogMS_ForwardEcc', 0.5),
           # host look not driven by the Box (edit them on MI_FogMS_Cloud or a child instance); a debug gain on the field
           ('FogMS_CloudPhaseG2', 0.0), ('FogMS_CloudPhaseBlend', 0.0), ('FogMS_CloudFieldGain', 1.0),
           # v2 (W46 anti-dither): the Box's Host Prefilter (0 = off, the v1 material) and the host's ray-march step [cm] (subsystem)
           ('FogMS_CloudPrefilter', 0.0), ('FogMS_CloudStep', 0.0),
           # v3 (W48): the weather branch (off), thin layer mode, the Box's conservative margin for the empty-step skip [cm]
           ('FogMS_WeatherOn', 0.0), ('FogMS_WeatherThin', 0.0), ('FogMS_CloudSkipMargin', 0.0))
VECTORS = (('FogMS_TileScale', (1.0, 1.0, 1.0, 0.0)), ('FogMS_WorldFrequencies', (0.0005, 0.002, 0.004, 0.0)),
           ('FogMS_WorldPhase0', (0.0, 0.0, 0.0, 0.0)), ('FogMS_WorldPhase1', (0.0, 0.0, 0.0, 0.0)),
           ('FogMS_WorldPhase2', (0.0, 0.0, 0.0, 0.0)), ('FogMS_ChannelMask', (1.0, 0.0, 0.0, 0.0)),
           ('FogMS_WorldExtent', (1000.0, 1000.0, 1000.0, 0.0)), ('FogMS_Albedo', (1.0, 1.0, 1.0, 1.0)),
           ('FogMS_ErosionMask', (0.0, 1.0, 0.0, 0.0)), ('FogMS_CloudBoxCenter', (0.0, 0.0, 0.0, 0.0)),
           # a 2000-cm cube at the origin: local = world * 50 / 1000
           ('FogMS_CloudWorldToLocal0', (0.05, 0.0, 0.0, 0.0)), ('FogMS_CloudWorldToLocal1', (0.0, 0.05, 0.0, 0.0)),
           ('FogMS_CloudWorldToLocal2', (0.0, 0.0, 0.05, 0.0)),
           # v2: world feature size [cm] of the base / detail 0 / detail 1 noise (the Box MID value; 0 = unknown: band unfiltered)
           ('FogMS_PrefilterWavelengths', (0.0, 0.0, 0.0, 0.0)),
           # v3 (W48 weather, FogMS_CloudHost.h FFogMSWeatherFeed): origin [cm], (1/domain, 1/detail tile [1/cm], curl, detail mip), wind
           # displacement [cm], L0 = (base, top [cm], sigma [1/m], detail strength), L1 = (base, top [cm], sigma [1/m], 0), toward the sun
           # + the host layer height [cm]. The defaults are inert (sigma 0).
           ('FogMS_WeatherOrigin', (0.0, 0.0, 0.0, 0.0)), ('FogMS_WeatherDomain', (5.0e-7, 4.0e-6, 0.05, 0.0)),
           ('FogMS_WeatherWind', (0.0, 0.0, 0.0, 0.0)), ('FogMS_WeatherL0', (0.0, 0.0, 0.0, 0.0)),
           ('FogMS_WeatherL1', (0.0, 0.0, 0.0, 0.0)), ('FogMS_WeatherSunDir', (0.0, 0.0, 1.0, 10000.0)))

LOCAL_CODE = """// %s (cloud host, matedit_cloud.py). Box density-cube space of this ray-march sample, -50..50 per axis: the LocalPosition
// M_FogMS_Density gets from TransformPosition(World -> Local) of its cube. Row_i.xyz = world axis i of the cube divided by the cube's
// world scale on that axis (so +-50 at the Box faces), Row_i.w = offset; WorldOffset = sample - FogMS_CloudBoxCenter [cm].
return float3(dot(Row0.xyz, WorldOffset) + Row0.w, dot(Row1.xyz, WorldOffset) + Row1.w, dot(Row2.xyz, WorldOffset) + Row2.w);
""" % MARKER
# The three UVW nodes of M_FogMS_Density (MaterialExpressionCustom_0/1/2), verbatim.
UVW0_CODE = ("if (WorldAligned > 0.5f) return WorldOffset * WorldFrequencies.x + WorldPhase;\n"
             "return (LocalPosition * 0.01f + 0.5f) * TileScale;")
UVW1_CODE = ("if (WorldAligned > 0.5f) return WorldOffset * WorldFrequencies.y + WorldPhase + float3(0.173f, 0.379f, 0.613f);\n"
             "return UVW * DetailScale + float3(0.173f, 0.379f, 0.613f);")
UVW2_CODE = ("if (WorldAligned > 0.5f) return WorldOffset * WorldFrequencies.z + WorldPhase + float3(0.731f, 0.217f, 0.419f);\n"
             "return UVW * DetailScale * 2.0f + float3(0.731f, 0.217f, 0.419f);")
FIELD_UVW_CODE = ("// Box-local cube space is -50..50; transport field texel (x,y,z) = Box-local cell (x,y,z), so uvw = local01.\n"
                  "return LocalPosition * 0.01f + 0.5f;")
EMISSIVE_CODE = """// Cloud-host copy of FogMS_EmissiveInjection (field contract v4) with a debug gain. valid = Mode > 0.5 and FieldA >= 0.5 (hybrid:
// A = 0.5 + 0.5 * T_sun * k; mode 3 full field: A = 1); Field = J minus the uncollided sun (the cloud marches the sun itself). The
// cloud adds Emissive as luminance per metre (VolumetricCloud.usf), the units of sigma_s[1/m] * J.
float V = (Mode > 0.5f && FieldA >= 0.5f) ? 1.0f : 0.0f;
return V * Field * Albedo * max(Extinction, 0.0f) * Gain;"""
ALBEDO_CODE = """// Cloud host BaseColor: the albedo of the cloud's own sun single scattering (per-step march toward the sun, host phase). Mode 3
// (Box 'Field Only (Debug)'): 0, the solver's full field alone (Emissive), extinction unchanged, as M_FogMS_Density mode 3.
return (Mode > 2.5f) ? float3(0.0f, 0.0f, 0.0f) : Albedo;"""
SKY_AO_CODE = """// Cloud host: the cloud's own sky light is replaced by the field (which carries the sky with the medium's self-shadowing): AO 0 with
// a valid field, 1 without (then the cloud lights the Box with the unshadowed distant sky, VolumetricCloud.usf DistantLightLuminance).
return (Mode > 0.5f && FieldA >= 0.5f) ? 0.0f : 1.0f;"""
CONSERVATIVE_CODE = """// FogMS_CloudConservative v2 (W48). Cloud host conservative density (Volumetric Advanced Output, x): 1 where the Box can hold density,
// 0 where it provably cannot, so empty ray-march steps skip the material (VolumetricCloud.usf: conservative density <= 0 -> next step,
// r.VolumetricCloud.StepSizeOnZeroConservativeDensity steps at once in the view pass). An unfed host (FogMS_Density 0, the material
// default) is empty everywhere. Outside the cube the edge fade of FogMS_Extinction is 0. Height profile on: P = 0 below HeightBottom and
// above HeightTop, so there n = detail only with |detail| <= DetailStrength (erosion only subtracts); below the threshold band's lower
// edge Lo = Threshold - Softness / 2 the mask is 0 as well, so the band limits the conservative region only when DetailStrength < Lo.
// v2: the region grows by Margin [cm] on every side (Margin <- FogMS_CloudSkipMargin = the skip x the host step, FogMS_CloudHost.cpp;
// |Row_i| = 1 / the cube's world scale on axis i, so the local margin is Margin * |Row_i|): a skip of that length from outside the grown
// region cannot land inside the Box, and the skipped samples stay on the same grid (t += skip * StepT). Margin 0: v1 exactly.
float3 M = Margin * float3(length(Row0.xyz), length(Row1.xyz), length(Row2.xyz));
bool Inside = all(abs(LocalPosition) <= 50.0f + M) && Density > 0.0f;
float h = LocalPosition.z * 0.01f + 0.5f;
float mh = M.z * 0.01f;
bool Band = HeightProfile < 0.5f || DetailStrength >= Threshold - 0.5f * Softness || (h >= HeightBottom - mh && h <= HeightTop + mh);
return float3((Inside && Band) ? 1.0f : 0.0f, 0.0f, 0.0f);"""

# W48 weather (module docstring v3). One density function, verbatim in M_FogMS_Cloud (FogMS_WeatherExtinction) and M_FogMS_WeatherSun
# (matedit_weather.py reads this constant with ast): FogMS_Weather_Design.md 3.5.
WEATHER_FN_CODE = r"""// FogMS_WeatherFn v1 (W48, matedit_cloud.py WEATHER_FN_CODE; the same text in M_FogMS_Cloud and M_FogMS_WeatherSun).
// sigma_t [1/m] of the FogMS Weather layers at a point (FogMS_Weather_Design.md 3.5, Nubis 2022):
//   m = WeatherMap(XY)   RT_FogMS_WeatherMap: R coverage L0, G type L0, B storm (W52), A deck L1; one tile = the domain (wrap)
//   L0: hf = (Alt - Base0) / (Top0 - Base0); lut = TypeLUT(u = m.g, v = 1 - hf): R profile, G softness, B billowy, A anvil
//       profile = lut.r * (1 + lut.a) * m.r
//       n = lerp(1, lerp(pattern.g, pattern.r, lut.b), DetailStrength): the pattern (rank-equalised: uniform 0..1) at the detail
//           tiling, curl-warped by hf (height-dependent: a 3D-like noise from 2D textures, HZD's 2D curl), at mip Domain.w
//       sigma0 = Sigma0 * saturate((n - (1 - profile)) / max(lut.g, 0.02))
//   L1: hf1 = (Alt - Base1) / (Top1 - Base1); sigma1 = Sigma1 * smoothstep(0, 0.15, hf1) * (1 - smoothstep(0.75, 1, hf1)) * m.a
//       * (0.7 + 0.3 * the pattern's deck channel at a quarter of the detail frequency)
// XY = sample - weather origin - wind displacement [cm] (the wind moves the whole field; the maps are drawn wind-free);
// Alt = altitude above the ground of the cloud layer [cm]. Domain = (1 / domain [1/cm], 1 / detail tile [1/cm], curl strength [detail
// tiles], detail mip); L0 = (base, top [cm], sigma [1/m], detail strength); L1 = (base, top [cm], sigma [1/m], 0). Sigma 0 = layer off.
// Outside [base, top] of every layer, or with the layer's map channel 0, the result is exactly 0 (FogMS_WeatherConservative relies on it).
struct FogMSWeatherFn
{
    float Sigma(float2 XY, float Alt, float4 Domain, float4 L0, float4 L1, Texture2D Map, SamplerState MapSampler,
                Texture2D TypeLUT, SamplerState TypeLUTSampler, Texture2D Pattern, SamplerState PatternSampler,
                Texture2D Curl, SamplerState CurlSampler)
    {
        bool In0 = L0.z > 0.0f && Alt > L0.x && Alt < L0.y;
        bool In1 = L1.z > 0.0f && Alt > L1.x && Alt < L1.y;
        if (!(In0 || In1)) return 0.0f;
        float4 m = Map.SampleLevel(MapSampler, XY * Domain.x, 0.0f);
        float s = 0.0f;
        if (In0 && m.r > 0.0f)
        {
            float hf = (Alt - L0.x) / max(L0.y - L0.x, 1.0f);
            float4 lut = TypeLUT.SampleLevel(TypeLUTSampler, float2(saturate(m.g), 1.0f - hf), 0.0f);
            float profile = lut.r * (1.0f + lut.a) * m.r;
            float2 duv = XY * Domain.y;
            float2 c = Curl.SampleLevel(CurlSampler, duv * 0.5f, Domain.w).rg * 2.0f - 1.0f;
            float4 p = Pattern.SampleLevel(PatternSampler, duv + c * (Domain.z * hf), Domain.w);
            float n = lerp(1.0f, lerp(p.g, p.r, lut.b), saturate(L0.w));
            s += L0.z * saturate((n - (1.0f - profile)) / max(lut.g, 0.02f));
        }
        if (In1 && m.a > 0.0f)
        {
            float hf1 = (Alt - L1.x) / max(L1.y - L1.x, 1.0f);
            float deck = smoothstep(0.0f, 0.15f, hf1) * (1.0f - smoothstep(0.75f, 1.0f, hf1));
            float low = Pattern.SampleLevel(PatternSampler, XY * (Domain.y * 0.25f), Domain.w).a;
            s += L1.z * deck * m.a * (0.7f + 0.3f * low);
        }
        return s;
    }
};
"""

WEATHER_EXTINCTION_CODE = WEATHER_FN_CODE + r"""// FogMS_WeatherExtinction v1 (W48): the weather's sigma_t [1/m] at this sample of the CLOUD SHADOW PASS (it feeds the Shadow input
// of the extinction's Shadow Pass Switch only; the view pass never evaluates it). Off (On 0: no FogMS Weather, Clear): exactly 0.
// Extended layer (Thin 0): FogMSWeatherFn.Sigma at the sample. Thin layer (Thin 1): every sample of a shadow texel lies on the same sun
// line inside the host layer; G = that line's ground point, OD(G) = RT_FogMS_WeatherSun (the whole weather column along the sun), and
// sigma = OD(G) / (the layer's path along the sun [m]) makes the samples of the texel sum to OD(G): the ground gets exp(-OD).
// Offset = sample - FogMS_WeatherOrigin [cm]; Wind.xy [cm]; Altitude = Cloud Sample Attributes.Altitude [cm]; SunDir.xyz toward the
// atmosphere sun, SunDir.w = the host layer height [cm].
if (On < 0.5f) return 0.0f;
FogMSWeatherFn W;
float2 XY = Offset.xy - Wind.xy;
if (Thin > 0.5f)
{
    float Up = max(SunDir.z, 0.05f);
    float2 G = XY - SunDir.xy * (Altitude / Up);
    float OD = SunMap.SampleLevel(SunMapSampler, G * Domain.x, 0.0f).r;
    return max(OD, 0.0f) / max(SunDir.w * 0.01f / Up, 1.0f);
}
return W.Sigma(XY, Altitude, Domain, L0, L1, Map, MapSampler, TypeLUT, TypeLUTSampler, Pattern, PatternSampler, Curl, CurlSampler);
"""

WEATHER_CONSERVATIVE_CODE = r"""// FogMS_WeatherConservative v1 (W48): conservative density of the weather in the cloud shadow pass: 1 where FogMS_WeatherExtinction
// can be > 0. Extended: inside a layer's altitudes with that layer's map channel > 0 at this point (FogMSWeatherFn is exactly 0
// elsewhere: the channel multiplies the density); thin: 1 everywhere in the host layer while on (the column is known only after the lookup).
if (On < 0.5f) return 0.0f;
if (Thin > 0.5f) return 1.0f;
bool In0 = L0.z > 0.0f && Altitude > L0.x && Altitude < L0.y;
bool In1 = L1.z > 0.0f && Altitude > L1.x && Altitude < L1.y;
if (!(In0 || In1)) return 0.0f;
float4 m = Map.SampleLevel(MapSampler, (Offset.xy - Wind.xy) * Domain.x, 0.0f);
return ((In0 && m.r > 0.0f) || (In1 && m.a > 0.0f)) ? 1.0f : 0.0f;
"""

SHADOW_SUM_CODE = """// FogMS_CloudShadowSum v1 (W48): extinction of the cloud shadow pass = the hero Box (FogMS_Extinction_v3) + the weather.
return Hero + Weather;"""

SHADOW_CONSERVATIVE_CODE = """// FogMS_CloudShadowConservative v1 (W48): conservative density of the cloud shadow pass = the hero Box's region or the weather's.
return float3(max(Hero.x, Weather), 0.0f, 0.0f);"""

FOOTPRINT_CODE = r"""// FogMS_CloudFootprint v1 (W46 cloud-host anti-dither, matedit_cloud.py). Nubis-style prefilter width w [cm] of FogMS_Extinction v3 at
// this ray-march sample: the density is band-limited to the sampling rate, so detail finer than a ray-march step or a traced pixel does
// not turn into per-pixel grain as the ray start jitters from pixel to pixel and frame to frame.
//   w = Prefilter * max(StepCm, Pixel),  Pixel = 2 * D * View.ViewSizeAndInvSize.z / View.ViewToClip[0][0]
// Prefilter <- FogMS_CloudPrefilter <- the Box's Host Prefilter. StepCm <- FogMS_CloudStep <- UFogMSCloudHostSubsystem: the host's nominal
// step = max(r.VolumetricCloud.DistanceToSampleMaxCount, Tracing Max Distance) / min(96 * View Sample Count Scale,
// r.VolumetricCloud.ViewRaySampleMaxCount) (short rays below r.VolumetricCloud.SampleMinCount steps march finer). D = |sample - camera|
// [cm] (CameraOffset = WorldPosition - CameraPositionWS). Pixel = the width of one TRACED pixel at D: the cloud's tracing pass binds the
// volumetric render target's view uniform buffer, whose ViewSize is the tracing resolution (VolumetricRenderTarget.cpp: full in mode 3,
// half in mode 1, quarter in mode 0); an orthographic view (ViewToClip[3][3] = 1) uses the step only.
// Extinction v3 then fades the detail octaves and the erosion pattern by 1 - w / lambda_i (lambda = FogMS_PrefilterWavelengths) and
// widens the threshold band by the removed variance (expected mask); the base noise keeps mip 0 (only its share widens the band).
// Prefilter 0 (material default): float2(0, 0), the v1 material exactly (w = 0 makes every v3 factor an exact identity). The .y
// component (the froxel material's base-noise mip bias) is unused by the cloud host: 0.
float W = 0.0f;
if (Prefilter > 0.0f)
{
    float D = length(CameraOffset);
    float Pixel = View.ViewToClip[3][3] < 1.0f ? 2.0f * D * View.ViewSizeAndInvSize.z / max(View.ViewToClip[0][0], 1e-6f) : 0.0f;
    W = Prefilter * max(max(StepCm, 0.0f), Pixel);
}
return float2(W, 0.0f);
"""

EXTINCTION_PINS = ('Noise', 'Detail0', 'Detail1', 'ChannelMask', 'DetailStrength', 'SecondOctave', 'LocalPosition', 'WorldExtent',
                   'Threshold', 'Softness', 'Density', 'Feather', 'ErosionStrength', 'ErosionDepth', 'HeightProfile', 'HeightBottom',
                   'HeightTop', 'BottomSoftness', 'TopSoftness', 'AnvilStrength', 'ErosionMask', 'Footprint', 'Wavelengths')
FOOTPRINT_PINS = ('CameraOffset', 'Prefilter', 'StepCm')
FORWARD_PINS = ('Emissive', 'FieldA', 'Mode', 'Strength', 'G', 'Depth', 'BackFloor', 'CameraVector', 'Ecc')
CONSERVATIVE_PINS = ('LocalPosition', 'HeightProfile', 'HeightBottom', 'HeightTop', 'Threshold', 'Softness', 'DetailStrength', 'Density',
                     'Row0', 'Row1', 'Row2', 'Margin')
# v3 (W48) weather nodes
WEATHER_EXT_PINS = ('Offset', 'Altitude', 'On', 'Thin', 'Domain', 'Wind', 'L0', 'L1', 'SunDir', 'Map', 'SunMap', 'TypeLUT', 'Pattern', 'Curl')
WEATHER_CONS_PINS = ('Offset', 'Altitude', 'On', 'Thin', 'Domain', 'Wind', 'L0', 'L1', 'Map')
SHADOW_PINS = ('Hero', 'Weather')
# Custom pin <- vector / scalar parameter of the weather nodes (the texture pins <- the texture object of the same stem, 'Map' <-
# FogMS_WeatherMap, 'SunMap' <- FogMS_WeatherSunMap, ...).
WEATHER_PARAM_PINS = {'On': ('FogMS_WeatherOn', ''), 'Thin': ('FogMS_WeatherThin', ''), 'Domain': ('FogMS_WeatherDomain', 'RGBA'),
                      'Wind': ('FogMS_WeatherWind', 'RGBA'), 'L0': ('FogMS_WeatherL0', 'RGBA'), 'L1': ('FogMS_WeatherL1', 'RGBA'),
                      'SunDir': ('FogMS_WeatherSunDir', 'RGBA')}
WEATHER_TEXTURE_PINS = {'Map': 'FogMS_WeatherMap', 'SunMap': 'FogMS_WeatherSunMap', 'TypeLUT': 'FogMS_WeatherTypeLUT',
                        'Pattern': 'FogMS_WeatherPattern', 'Curl': 'FogMS_WeatherCurl'}


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def probe_dir():
    here = globals().get('__file__')
    if here:
        return os.path.dirname(os.path.abspath(here))
    path = globals().get('P2_PROBE_DIR')
    require(path, 'P2_PROBE_DIR (folder of matedit_density.py) is not set')
    return path


def matedit_code(name):
    """A string constant of matedit_density.py (read as text with ast; that script patches on import)."""
    path = os.path.join(probe_dir(), 'matedit_density.py')
    tree = ast.parse(open(path, encoding='utf-8').read())
    for node in tree.body:
        if isinstance(node, ast.Assign) and any(getattr(t, 'id', '') == name for t in node.targets):
            return node.value.value
    raise RuntimeError('%s not found in %s' % (name, path))


def norm(code):
    return str(code).replace('\r\n', '\n').strip()


# ------------------------------------------------------------------ definitive link readback (matedit_density.py W40)
_T3D_PIN = re.compile(r'^\s*Inputs\((\d+)\)=\(InputName="([^"]*)"(.*)$')
_T3D_SOURCE = re.compile(r'Expression="[^"\']*\'([^\']+)\'"')
_T3D_INDEX = re.compile(r'\bOutputIndex=(\d+)')


def export_t3d(obj):
    """The object's T3D text (read-only): each FExpressionInput's Expression and OutputIndex (omitted when 0)."""
    path = os.path.join(tempfile.gettempdir(), 'fogms_matedit_cloud_%s_%d.t3d' % (obj.get_name(), int(time.time() * 1000)))
    task = unreal.AssetExportTask()
    task.set_editor_property('object', obj)
    task.set_editor_property('filename', path)
    task.set_editor_property('automated', True)
    task.set_editor_property('prompt', False)
    task.set_editor_property('replace_identical', True)
    task.set_editor_property('exporter', unreal.ObjectExporterT3D())
    require(unreal.Exporter.run_asset_export_task(task) and os.path.isfile(path), 'T3D export failed for ' + obj.get_name())
    try:
        with open(path, 'rb') as f:
            data = f.read()
    finally:
        try:
            os.remove(path)
        except OSError:
            pass
    return data.decode('utf-16') if data[:2] in (b'\xff\xfe', b'\xfe\xff') else data.decode('utf-8', errors='replace')


def input_sources(material, node):
    names = list(mel.get_material_expression_input_names(node))
    sources = list(mel.get_inputs_for_material_expression(material, node))
    require(len(names) == len(sources), 'Input name/source count mismatch on ' + node.get_name())
    return list(zip(names, sources))


def custom_links(material, node):
    """{pin: (source expression or None, source output name or None)} of a Custom node, from its T3D (stored OutputIndex)."""
    require(isinstance(node, unreal.MaterialExpressionCustom), node.get_name() + ' is not a Custom node')
    pairs = input_sources(material, node)
    parsed = {}
    for line in export_t3d(node).splitlines():
        m = _T3D_PIN.match(line)
        if not m:
            continue
        src_m, idx_m = _T3D_SOURCE.search(m.group(3)), _T3D_INDEX.search(m.group(3))
        parsed[int(m.group(1))] = (m.group(2), re.split(r'[:.]', src_m.group(1))[-1] if src_m else None,
                                   int(idx_m.group(1)) if idx_m else 0)
    require(sorted(parsed) == list(range(len(pairs))),
            'T3D of %s lists pins %s, the node has %d inputs' % (node.get_name(), sorted(parsed), len(pairs)))
    links = {}
    for i, (pin, src) in enumerate(pairs):
        t_pin, t_src, t_index = parsed[i]
        require(t_pin == pin, 'T3D pin %d of %s is %s, not %s' % (i, node.get_name(), t_pin, pin))
        require((src is None) == (t_src is None) and (src is None or src.get_name() == t_src),
                'T3D source of %s.%s is %s, the library reports %s' % (node.get_name(), pin, t_src, src.get_name() if src else None))
        if src is None:
            links[pin] = (None, None)
            continue
        outputs = [str(o) for o in mel.get_material_expression_output_names(src)]
        require(0 <= t_index < len(outputs), '%s.%s reads output %d of %s (%d outputs)' % (node.get_name(), pin, t_index, src.get_name(), len(outputs)))
        links[pin] = (src, outputs[t_index])
    return links


def verify_custom(material, node, wanted):
    """wanted = {pin: (source, output name)}; every pin of the node must be listed and stored exactly so."""
    stored = custom_links(material, node)
    require(sorted(stored) == sorted(wanted), '%s pins %s, wanted %s' % (node.get_name(), sorted(stored), sorted(wanted)))
    for pin, (src, out) in wanted.items():
        got = stored[pin]
        require(got[0] == src and got[1] == out, 'Link %s.%s is %s.%s, wanted %s.%s' % (
            node.get_name(), pin, got[0].get_name() if got[0] else None, got[1], src.get_name(), out))


def verify_single_output_sources(material, node, wanted):
    """Non-Custom node (texture sample, subtract, Volumetric Advanced Output): wanted = {pin: source}; the sources must be
    single-output expressions (then the output index is 0 by construction) and stored exactly."""
    stored = dict(input_sources(material, node))
    for pin, src in wanted.items():
        got = stored.get(pin)
        require(got == src, 'Link %s.%s is %s, wanted %s' % (node.get_name(), pin, got.get_name() if got else None, src.get_name()))
        outs = [str(o) for o in mel.get_material_expression_output_names(src)]
        require(len(outs) <= 1, '%s feeds %s.%s but has outputs %s (use a Custom node check)' % (src.get_name(), node.get_name(), pin, outs))


# ------------------------------------------------------------------ graph
def custom_nodes(material):
    return [e for e in mel.get_material_expressions(material) if isinstance(e, unreal.MaterialExpressionCustom)]


def by_description(material, description):
    found = [e for e in custom_nodes(material) if str(e.get_editor_property('description')) == description]
    return found[0] if len(found) == 1 else None


class Graph:
    def __init__(self, material):
        self.m = material
        self.params = {}
        self.links = {}     # Custom node -> {pin: (source, output)}
        self.plain = {}     # other node -> {pin: source}

    def node(self, cls, x, y, **props):
        e = mel.create_material_expression(self.m, cls, x, y)
        require(e is not None, 'Cannot create ' + cls.__name__)
        for k, v in props.items():
            e.set_editor_property(k, v)
        return e

    def link(self, src, out, dst, pin):
        """out = the source's output NAME; '' = its first output (index 0, e.g. RGBA of a Constant4Vector), named explicitly here so
        the T3D readback checks index 0."""
        outs = [str(o) for o in mel.get_material_expression_output_names(src)]
        if out == '' and len(outs) > 1:
            out = outs[0]
        require(out in outs or (out == '' and len(outs) <= 1), '%s has no output %r (outputs %s)' % (src.get_name(), out, outs))
        require(mel.connect_material_expressions(src, out, dst, pin), 'Cannot wire %s.%s -> %s.%s' % (src.get_name(), out, dst.get_name(), pin))
        if isinstance(dst, unreal.MaterialExpressionCustom):
            self.links.setdefault(dst, {})[pin] = (src, out if out else (outs[0] if outs else ''))
        else:
            require(len(outs) <= 1, '%s -> %s.%s: multi-output source into a non-Custom node is not verified by T3D here' % (
                src.get_name(), dst.get_name(), pin))
            self.plain.setdefault(dst, {})[pin] = src

    def custom(self, description, code, out_type, pins, x, y):
        e = self.node(unreal.MaterialExpressionCustom, x, y)
        e.set_editor_property('code', code)
        e.set_editor_property('output_type', out_type)
        e.set_editor_property('description', description)
        items = []
        for pin in pins:
            item = unreal.CustomInput()
            item.set_editor_property('input_name', pin)
            items.append(item)
        e.set_editor_property('inputs', items)
        return e

    def scalar(self, name, default, x, y):
        e = self.node(unreal.MaterialExpressionScalarParameter, x, y)
        e.set_editor_property('parameter_name', name)
        e.set_editor_property('default_value', default)
        e.set_editor_property('group', GROUP)
        self.params[name] = e
        return e

    def vector(self, name, default, x, y):
        e = self.node(unreal.MaterialExpressionVectorParameter, x, y)
        e.set_editor_property('parameter_name', name)
        e.set_editor_property('default_value', unreal.LinearColor(*default))
        e.set_editor_property('group', GROUP)
        self.params[name] = e
        return e


def set_node_bool(node, names, value):
    for name in names:
        try:
            node.set_editor_property(name, value)
            return name
        except Exception:
            pass
    raise RuntimeError('None of %s exists on %s' % (names, node.get_class().get_name()))


def field_default_texture():
    """The FogMS_TransportField placeholder of M_FogMS_Density (the Box binds its real field); else the default volume."""
    dm = unreal.load_asset(DENSITY_MATERIAL)
    if dm is not None:
        for e in mel.get_material_expressions(dm):
            if isinstance(e, unreal.MaterialExpressionTextureSampleParameterVolume) and str(e.get_editor_property('parameter_name')) == 'FogMS_TransportField':
                tex = e.get_editor_property('texture')
                if tex is not None:
                    return tex
    return unreal.load_asset(DEFAULT_VOLUME)


def build(material, extinction_code, lobe_code):
    """Creates the whole graph (module docstring) in an empty material; returns the Graph (links to verify)."""
    g = Graph(material)
    y = 0
    for name, default in SCALARS:
        g.scalar(name, default, -2600, y); y += 90
    for name, default in VECTORS:
        g.vector(name, default, -2300, y); y += 130
    P = g.params
    wp = g.node(unreal.MaterialExpressionWorldPosition, -2000, -400)
    try:  # default mode = absolute world position, the one the cloud sets (module docstring)
        wp.set_editor_property('world_position_shader_offset', unreal.WorldPositionIncludedOffsets.WPT_DEFAULT)
    except Exception as e:
        print('NOTE world_position_shader_offset not set (%s); the node default is WPT_DEFAULT' % e)
    offset = g.node(unreal.MaterialExpressionSubtract, -1800, -400)
    require(mel.connect_material_expressions(wp, 'XYZ', offset, 'A'), 'Cannot wire WorldPosition.XYZ -> Subtract.A')
    require(mel.connect_material_expressions(P['FogMS_CloudBoxCenter'], 'RGB', offset, 'B'), 'Cannot wire FogMS_CloudBoxCenter.RGB -> Subtract.B')
    local = g.custom('FogMS_CloudBoxLocal', LOCAL_CODE, unreal.CustomMaterialOutputType.CMOT_FLOAT3,
                     ('WorldOffset', 'Row0', 'Row1', 'Row2'), -1600, -400)
    g.link(offset, '', local, 'WorldOffset')
    for i in range(3):
        g.link(P['FogMS_CloudWorldToLocal%d' % i], 'RGBA', local, 'Row%d' % i)
    uvw = []
    for i, code in enumerate((UVW0_CODE, UVW1_CODE, UVW2_CODE)):
        pins = ('WorldAligned', 'WorldOffset', 'WorldFrequencies', 'WorldPhase') + (('LocalPosition', 'TileScale') if i == 0 else ('UVW', 'DetailScale'))
        node = g.custom('FogMS_CloudNoiseUVW%d' % i, code, unreal.CustomMaterialOutputType.CMOT_FLOAT3, pins, -1300, -200 + 260 * i)
        g.link(P['FogMS_WorldAligned'], '', node, 'WorldAligned'); g.link(offset, '', node, 'WorldOffset')
        g.link(P['FogMS_WorldFrequencies'], 'RGB', node, 'WorldFrequencies'); g.link(P['FogMS_WorldPhase%d' % i], 'RGB', node, 'WorldPhase')
        if i == 0:
            g.link(local, '', node, 'LocalPosition'); g.link(P['FogMS_TileScale'], 'RGB', node, 'TileScale')
        else:
            g.link(uvw[0], '', node, 'UVW'); g.link(P['FogMS_DetailScale'], '', node, 'DetailScale')
        uvw.append(node)
    volume = unreal.load_asset(DEFAULT_VOLUME)
    require(volume is not None, 'Missing ' + DEFAULT_VOLUME)
    mip0 = g.node(unreal.MaterialExpressionConstant, -1200, 600, r=0.0)
    samples = []
    for i in range(3):
        s = g.node(unreal.MaterialExpressionTextureSampleParameterVolume, -1000, -200 + 260 * i)
        s.set_editor_property('parameter_name', 'FogMS_Noise'); s.set_editor_property('group', GROUP)
        s.set_editor_property('texture', volume)
        s.set_editor_property('sampler_type', unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR)
        s.set_editor_property('mip_value_mode', unreal.TextureMipValueMode.TMVM_MIP_LEVEL)
        g.link(uvw[i], '', s, 'UVs'); g.link(mip0, '', s, 'Level')
        samples.append(s)
    # v2 (W46): the prefilter width from the camera distance, the host step and the Box's strength (v1: constants 0).
    camera = g.node(unreal.MaterialExpressionCameraPositionWS, -1200, 800)
    camoff = g.node(unreal.MaterialExpressionSubtract, -1000, 800)
    require(mel.connect_material_expressions(wp, 'XYZ', camoff, 'A'), 'Cannot wire WorldPosition.XYZ -> camera Subtract.A')
    require(mel.connect_material_expressions(camera, '', camoff, 'B'), 'Cannot wire CameraPositionWS -> camera Subtract.B')
    footprint = g.custom('FogMS_CloudFootprint', FOOTPRINT_CODE, unreal.CustomMaterialOutputType.CMOT_FLOAT2, FOOTPRINT_PINS, -800, 700)
    g.link(camoff, '', footprint, 'CameraOffset'); g.link(P['FogMS_CloudPrefilter'], '', footprint, 'Prefilter')
    g.link(P['FogMS_CloudStep'], '', footprint, 'StepCm')
    wavelengths = P['FogMS_PrefilterWavelengths']
    ext = g.custom('FogMS_Extinction_v3', extinction_code, unreal.CustomMaterialOutputType.CMOT_FLOAT1, EXTINCTION_PINS, -600, 0)
    sources = {'Noise': (samples[0], 'RGBA'), 'Detail0': (samples[1], 'RGBA'), 'Detail1': (samples[2], 'RGBA'),
               'ChannelMask': (P['FogMS_ChannelMask'], 'RGBA'), 'DetailStrength': (P['FogMS_DetailStrength'], ''),
               'SecondOctave': (P['FogMS_DetailSecondOctave'], ''), 'LocalPosition': (local, ''), 'WorldExtent': (P['FogMS_WorldExtent'], 'RGB'),
               'Threshold': (P['FogMS_Threshold'], ''), 'Softness': (P['FogMS_Softness'], ''), 'Density': (P['FogMS_Density'], ''),
               'Feather': (P['FogMS_DensityFeather'], ''), 'ErosionStrength': (P['FogMS_ErosionStrength'], ''),
               'ErosionDepth': (P['FogMS_ErosionDepth'], ''), 'HeightProfile': (P['FogMS_HeightProfile'], ''),
               'HeightBottom': (P['FogMS_HeightBottom'], ''), 'HeightTop': (P['FogMS_HeightTop'], ''),
               'BottomSoftness': (P['FogMS_HeightBottomSoftness'], ''), 'TopSoftness': (P['FogMS_HeightTopSoftness'], ''),
               'AnvilStrength': (P['FogMS_HeightAnvilStrength'], ''), 'ErosionMask': (P['FogMS_ErosionMask'], 'RGBA'),
               'Footprint': (footprint, ''), 'Wavelengths': (wavelengths, 'RGBA')}
    for pin in EXTINCTION_PINS:
        g.link(sources[pin][0], sources[pin][1], ext, pin)
    fuvw = g.custom('FogMS_TransportUVW', FIELD_UVW_CODE, unreal.CustomMaterialOutputType.CMOT_FLOAT3, ('LocalPosition',), -600, 1000)
    g.link(local, '', fuvw, 'LocalPosition')
    field = g.node(unreal.MaterialExpressionTextureSampleParameterVolume, -350, 1000)
    field.set_editor_property('parameter_name', 'FogMS_TransportField'); field.set_editor_property('group', GROUP)
    field.set_editor_property('texture', field_default_texture() or volume)
    field.set_editor_property('sampler_type', unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR)
    g.link(fuvw, '', field, 'UVs')
    emissive = g.custom('FogMS_EmissiveInjection', EMISSIVE_CODE, unreal.CustomMaterialOutputType.CMOT_FLOAT3,
                        ('Field', 'FieldA', 'Albedo', 'Extinction', 'Mode', 'Gain'), -50, 700)
    g.link(field, 'RGB', emissive, 'Field'); g.link(field, 'A', emissive, 'FieldA'); g.link(P['FogMS_Albedo'], 'RGB', emissive, 'Albedo')
    g.link(ext, '', emissive, 'Extinction'); g.link(P['FogMS_InjectionMode'], '', emissive, 'Mode'); g.link(P['FogMS_CloudFieldGain'], '', emissive, 'Gain')
    camera = g.node(unreal.MaterialExpressionCameraVectorWS, -50, 1300)
    lobe = g.custom('FogMS_ForwardLobe', lobe_code, unreal.CustomMaterialOutputType.CMOT_FLOAT3, FORWARD_PINS, 250, 700)
    lobe_sources = {'Emissive': (emissive, ''), 'FieldA': (field, 'A'), 'Mode': (P['FogMS_InjectionMode'], ''),
                    'Strength': (P['FogMS_ForwardStrength'], ''), 'G': (P['FogMS_ForwardG'], ''), 'Depth': (P['FogMS_ForwardDepth'], ''),
                    'BackFloor': (P['FogMS_ForwardFloor'], ''), 'CameraVector': (camera, ''), 'Ecc': (P['FogMS_ForwardEcc'], '')}
    for pin in FORWARD_PINS:
        g.link(lobe_sources[pin][0], lobe_sources[pin][1], lobe, pin)
    albedo = g.custom('FogMS_CloudAlbedo', ALBEDO_CODE, unreal.CustomMaterialOutputType.CMOT_FLOAT3, ('Albedo', 'Mode'), 250, 400)
    g.link(P['FogMS_Albedo'], 'RGB', albedo, 'Albedo'); g.link(P['FogMS_InjectionMode'], '', albedo, 'Mode')
    sky = g.custom('FogMS_CloudSkyAO', SKY_AO_CODE, unreal.CustomMaterialOutputType.CMOT_FLOAT1, ('FieldA', 'Mode'), 250, 1100)
    g.link(field, 'A', sky, 'FieldA'); g.link(P['FogMS_InjectionMode'], '', sky, 'Mode')
    cons = g.custom('FogMS_CloudConservative', CONSERVATIVE_CODE, unreal.CustomMaterialOutputType.CMOT_FLOAT3, CONSERVATIVE_PINS, 250, -500)
    for pin, src in (('LocalPosition', local), ('HeightProfile', P['FogMS_HeightProfile']), ('HeightBottom', P['FogMS_HeightBottom']),
                     ('HeightTop', P['FogMS_HeightTop']), ('Threshold', P['FogMS_Threshold']), ('Softness', P['FogMS_Softness']),
                     ('DetailStrength', P['FogMS_DetailStrength']), ('Density', P['FogMS_Density']), ('Margin', P['FogMS_CloudSkipMargin'])):
        g.link(src, '', cons, pin)
    for i in range(3):
        g.link(P['FogMS_CloudWorldToLocal%d' % i], 'RGBA', cons, 'Row%d' % i)
    # v3 (W48): the weather branch of the cloud shadow pass.
    tex = {}
    y = 1500
    for name, path in WEATHER_TEXTURES:
        t = g.node(unreal.MaterialExpressionTextureObjectParameter, -1300, y)
        t.set_editor_property('parameter_name', name)
        t.set_editor_property('group', WEATHER_GROUP)
        default = unreal.load_asset(path)
        require(default is not None, 'Missing texture %s (run matedit_weather.py first)' % path)
        t.set_editor_property('texture', default)
        g.params[name] = t
        tex[name] = t
        y += 140
    woff = g.node(unreal.MaterialExpressionSubtract, -1000, 1400)
    require(mel.connect_material_expressions(wp, 'XYZ', woff, 'A'), 'Cannot wire WorldPosition.XYZ -> weather Subtract.A')
    require(mel.connect_material_expressions(P['FogMS_WeatherOrigin'], 'RGB', woff, 'B'), 'Cannot wire FogMS_WeatherOrigin.RGB -> weather Subtract.B')
    csa = g.node(unreal.MaterialExpressionCloudSampleAttribute, -1000, 1550)
    wext = g.custom('FogMS_WeatherExtinction', WEATHER_EXTINCTION_CODE, unreal.CustomMaterialOutputType.CMOT_FLOAT1, WEATHER_EXT_PINS, -600, 1500)
    wcons = g.custom('FogMS_WeatherConservative', WEATHER_CONSERVATIVE_CODE, unreal.CustomMaterialOutputType.CMOT_FLOAT1, WEATHER_CONS_PINS, -600, 1900)
    for node, pins in ((wext, WEATHER_EXT_PINS), (wcons, WEATHER_CONS_PINS)):
        for pin in pins:
            if pin == 'Offset':
                g.link(woff, '', node, pin)
            elif pin == 'Altitude':
                g.link(csa, 'Altitude', node, pin)
            elif pin in WEATHER_PARAM_PINS:
                name, out = WEATHER_PARAM_PINS[pin]
                g.link(P[name], out, node, pin)
            else:
                g.link(tex[WEATHER_TEXTURE_PINS[pin]], '', node, pin)
    ssum = g.custom('FogMS_CloudShadowSum', SHADOW_SUM_CODE, unreal.CustomMaterialOutputType.CMOT_FLOAT1, SHADOW_PINS, -250, 1500)
    g.link(ext, '', ssum, 'Hero'); g.link(wext, '', ssum, 'Weather')
    scons = g.custom('FogMS_CloudShadowConservative', SHADOW_CONSERVATIVE_CODE, unreal.CustomMaterialOutputType.CMOT_FLOAT3, SHADOW_PINS, -250, 1900)
    g.link(cons, '', scons, 'Hero'); g.link(wcons, '', scons, 'Weather')
    sw_ext = g.node(unreal.MaterialExpressionShadowReplace, 50, 1500)
    g.link(ext, '', sw_ext, 'Default'); g.link(ssum, '', sw_ext, 'Shadow')
    sw_cons = g.node(unreal.MaterialExpressionShadowReplace, 50, 1900)
    g.link(cons, '', sw_cons, 'Default'); g.link(scons, '', sw_cons, 'Shadow')
    vao = g.node(unreal.MaterialExpressionVolumetricAdvancedMaterialOutput, 600, -300)
    set_node_bool(vao, ('ray_march_volume_shadow', 'bRayMarchVolumeShadow'), True)
    set_node_bool(vao, ('ground_contribution', 'bGroundContribution'), False)
    set_node_bool(vao, ('gray_scale_material', 'bGrayScaleMaterial'), False)
    set_node_bool(vao, ('clamp_multi_scattering_contribution', 'bClampMultiScatteringContribution'), True)
    vao.set_editor_property('multi_scattering_approximation_octave_count', 0)
    vao.set_editor_property('per_sample_phase_evaluation', False)
    for pin, src in vao_wanted(vao, P, sw_cons).items():
        g.link(src, '', vao, pin)
    for prop, src in (('MP_BASE_COLOR', albedo), ('MP_SUBSURFACE_COLOR', sw_ext), ('MP_EMISSIVE_COLOR', lobe), ('MP_AMBIENT_OCCLUSION', sky)):
        require(mel.connect_material_property(src, '', getattr(unreal.MaterialProperty, prop)), 'Cannot route ' + prop)
    return g


def vao_wanted(vao, P, cons):
    """{VAO pin name: source}: Phase G <- FogMS_ForwardG (the Box's Phase G), G2 / Blend <- host scalars, conservative density (v3: the
    conservative Shadow Pass Switch)."""
    names = [str(n) for n in mel.get_material_expression_input_names(vao)]
    key = dict((n.replace(' ', '').lower(), n) for n in names)
    out = {}
    for want, src in (('phaseg', P['FogMS_ForwardG']), ('phaseg2', P['FogMS_CloudPhaseG2']), ('phaseblend', P['FogMS_CloudPhaseBlend']),
                      ('conservativedensity', cons)):
        require(want in key, 'Volumetric Advanced Output has no input like %s: %s' % (want, names))
        out[key[want]] = src
    return out


def verify_prefilter(material, nodes, P):
    """v2 (W46) links, checked on every run (just built or existing asset) from the T3D readback: FogMS_Extinction_v3.Footprint <-
    FogMS_CloudFootprint, .Wavelengths <- FogMS_PrefilterWavelengths.RGBA; FogMS_CloudFootprint.Prefilter / .StepCm <- FogMS_CloudPrefilter /
    FogMS_CloudStep, .CameraOffset <- a Subtract of (WorldPosition, CameraPositionWS) (multi-output WorldPosition: checked by class, as
    the WorldPosition -> FogMS_CloudBoxCenter subtract)."""
    def nm(e):
        return e.get_name() if e else None
    ext, foot = nodes['FogMS_Extinction_v3'], nodes['FogMS_CloudFootprint']
    links = custom_links(material, ext)
    require(links['Footprint'][0] == foot, 'FogMS_Extinction_v3.Footprint is fed by %s, wanted FogMS_CloudFootprint' % nm(links['Footprint'][0]))
    require(links['Wavelengths'][0] == P['FogMS_PrefilterWavelengths'] and links['Wavelengths'][1] == 'RGBA',
            'FogMS_Extinction_v3.Wavelengths is %s.%s, wanted FogMS_PrefilterWavelengths.RGBA' % (nm(links['Wavelengths'][0]), links['Wavelengths'][1]))
    fl = custom_links(material, foot)
    require(fl['Prefilter'][0] == P['FogMS_CloudPrefilter'], 'FogMS_CloudFootprint.Prefilter is fed by %s' % nm(fl['Prefilter'][0]))
    require(fl['StepCm'][0] == P['FogMS_CloudStep'], 'FogMS_CloudFootprint.StepCm is fed by %s' % nm(fl['StepCm'][0]))
    sub = fl['CameraOffset'][0]
    require(isinstance(sub, unreal.MaterialExpressionSubtract), 'FogMS_CloudFootprint.CameraOffset is fed by %s, wanted a Subtract' % nm(sub))
    ins = dict(input_sources(material, sub))
    require(isinstance(ins.get('A'), unreal.MaterialExpressionWorldPosition) and isinstance(ins.get('B'), unreal.MaterialExpressionCameraPositionWS),
            'camera Subtract inputs are A=%s B=%s, wanted WorldPosition - CameraPositionWS' % (nm(ins.get('A')), nm(ins.get('B'))))


def verify_weather(material, nodes, P):
    """v3 (W48) links, checked on every run from the T3D readback: the weather Custom nodes' pins (parameters by output, texture objects,
    the weather-offset Subtract, Cloud Sample Attributes.Altitude), the shadow sum / conservative nodes, the hero conservative v2 pins, and
    the two Shadow Pass Switches (SubsurfaceColor and the VAO conservative density)."""
    def nm(e):
        return e.get_name() if e else None
    def want(node, pin, src, out):
        got = custom_links(material, node)[pin]
        require(got[0] == src and got[1] == out, '%s.%s is %s.%s, wanted %s.%s' % (
            node.get_name(), pin, nm(got[0]), got[1], nm(src), out))
    for desc, pins in (('FogMS_WeatherExtinction', WEATHER_EXT_PINS), ('FogMS_WeatherConservative', WEATHER_CONS_PINS)):
        node = nodes[desc]
        links = custom_links(material, node)
        require(sorted(links) == sorted(pins), '%s pins %s, wanted %s' % (desc, sorted(links), sorted(pins)))
        for pin in pins:
            src, out = links[pin]
            if pin == 'Offset':
                require(isinstance(src, unreal.MaterialExpressionSubtract), '%s.Offset is fed by %s, wanted a Subtract' % (desc, nm(src)))
                ins = dict(input_sources(material, src))
                require(isinstance(ins.get('A'), unreal.MaterialExpressionWorldPosition) and ins.get('B') == P['FogMS_WeatherOrigin'],
                        '%s.Offset Subtract is A=%s B=%s, wanted WorldPosition - FogMS_WeatherOrigin' % (desc, nm(ins.get('A')), nm(ins.get('B'))))
            elif pin == 'Altitude':
                require(isinstance(src, unreal.MaterialExpressionCloudSampleAttribute) and out == 'Altitude',
                        '%s.Altitude is %s.%s, wanted Cloud Sample Attributes.Altitude' % (desc, nm(src), out))
            elif pin in WEATHER_PARAM_PINS:
                name, wanted_out = WEATHER_PARAM_PINS[pin]
                want(node, pin, P[name], wanted_out)
            else:
                want(node, pin, P[WEATHER_TEXTURE_PINS[pin]], '')
    want(nodes['FogMS_CloudShadowSum'], 'Hero', nodes['FogMS_Extinction_v3'], '')
    want(nodes['FogMS_CloudShadowSum'], 'Weather', nodes['FogMS_WeatherExtinction'], '')
    want(nodes['FogMS_CloudShadowConservative'], 'Hero', nodes['FogMS_CloudConservative'], '')
    want(nodes['FogMS_CloudShadowConservative'], 'Weather', nodes['FogMS_WeatherConservative'], '')
    cl = custom_links(material, nodes['FogMS_CloudConservative'])
    require(cl['Margin'][0] == P['FogMS_CloudSkipMargin'], 'FogMS_CloudConservative.Margin is fed by %s' % nm(cl['Margin'][0]))
    for i in range(3):
        require(cl['Row%d' % i] == (P['FogMS_CloudWorldToLocal%d' % i], 'RGBA'),
                'FogMS_CloudConservative.Row%d is %s.%s' % (i, nm(cl['Row%d' % i][0]), cl['Row%d' % i][1]))
    switches = [e for e in mel.get_material_expressions(material) if isinstance(e, unreal.MaterialExpressionShadowReplace)]
    require(len(switches) == 2, 'expected two Shadow Pass Switches, found %d' % len(switches))
    sub = mel.get_material_property_input_node(material, unreal.MaterialProperty.MP_SUBSURFACE_COLOR)
    require(sub in switches, 'MP_SUBSURFACE_COLOR is fed by %s, wanted a Shadow Pass Switch' % nm(sub))
    verify_single_output_sources(material, sub, {'Default': nodes['FogMS_Extinction_v3'], 'Shadow': nodes['FogMS_CloudShadowSum']})
    other = [s for s in switches if s != sub][0]
    verify_single_output_sources(material, other, {'Default': nodes['FogMS_CloudConservative'], 'Shadow': nodes['FogMS_CloudShadowConservative']})
    return other


def find_param(material, name):
    for e in mel.get_material_expressions(material):
        try:
            if str(e.get_editor_property('parameter_name')) == name:
                return e
        except Exception:
            pass
    return None


def verify(material, extinction_code, lobe_code, g=None):
    """Definitive check of the whole graph; returns a list of problems (empty = current). With g (just built) the recorded links
    are the wanted ones; without it (an existing asset) the wanted links are rebuilt from the parameter / description names."""
    problems = []
    try:
        require(material.get_editor_property('material_domain') == unreal.MaterialDomain.MD_VOLUME, 'domain is not Volume')
        require(material.get_editor_property('blend_mode') == unreal.BlendMode.BLEND_ADDITIVE, 'blend mode is not Additive')
        require(material.get_editor_property('shading_model') == unreal.MaterialShadingModel.MSM_DEFAULT_LIT, 'shading model is not DefaultLit')
        require(mel.has_material_usage(material, unreal.MaterialUsage.MATUSAGE_VOLUMETRIC_CLOUD), 'Used with Volumetric Cloud not set')
        codes = {'FogMS_CloudBoxLocal': LOCAL_CODE, 'FogMS_CloudNoiseUVW0': UVW0_CODE, 'FogMS_CloudNoiseUVW1': UVW1_CODE,
                 'FogMS_CloudNoiseUVW2': UVW2_CODE, 'FogMS_Extinction_v3': extinction_code, 'FogMS_TransportUVW': FIELD_UVW_CODE,
                 'FogMS_EmissiveInjection': EMISSIVE_CODE, 'FogMS_ForwardLobe': lobe_code, 'FogMS_CloudAlbedo': ALBEDO_CODE,
                 'FogMS_CloudSkyAO': SKY_AO_CODE, 'FogMS_CloudConservative': CONSERVATIVE_CODE, 'FogMS_CloudFootprint': FOOTPRINT_CODE,
                 'FogMS_WeatherExtinction': WEATHER_EXTINCTION_CODE, 'FogMS_WeatherConservative': WEATHER_CONSERVATIVE_CODE,
                 'FogMS_CloudShadowSum': SHADOW_SUM_CODE, 'FogMS_CloudShadowConservative': SHADOW_CONSERVATIVE_CODE}
        nodes = {}
        for desc, code in codes.items():
            node = by_description(material, desc)
            require(node is not None, 'Custom node %s missing (or not unique)' % desc)
            require(norm(node.get_editor_property('code')) == norm(code), 'Custom node %s has other code' % desc)
            nodes[desc] = node
        for name, default in SCALARS:
            p = find_param(material, name)
            require(p is not None, 'scalar %s missing' % name)
            require(abs(mel.get_material_default_scalar_parameter_value(material, name) - default) < 1e-6, 'scalar %s default is not %s' % (name, default))
        for name, default in VECTORS:
            require(find_param(material, name) is not None, 'vector %s missing' % name)
        for name, path in WEATHER_TEXTURES:
            t = find_param(material, name)
            require(isinstance(t, unreal.MaterialExpressionTextureObjectParameter), 'texture object %s missing' % name)
            tex = t.get_editor_property('texture')
            require(tex is not None and tex.get_path_name().split('.')[0] == path, 'texture object %s default is %s, wanted %s' % (
                name, tex.get_path_name() if tex else None, path))
        vaos = [e for e in mel.get_material_expressions(material) if isinstance(e, unreal.MaterialExpressionVolumetricAdvancedMaterialOutput)]
        require(len(vaos) == 1, 'expected one Volumetric Advanced Output, found %d' % len(vaos))
        vao = vaos[0]
        require(int(vao.get_editor_property('multi_scattering_approximation_octave_count')) == 0, 'VAO octaves are not 0')
        for names, want in ((('ray_march_volume_shadow', 'bRayMarchVolumeShadow'), True), (('ground_contribution', 'bGroundContribution'), False),
                            (('gray_scale_material', 'bGrayScaleMaterial'), False)):
            got = None
            for n in names:
                try:
                    got = bool(vao.get_editor_property(n)); break
                except Exception:
                    pass
            require(got == want, 'VAO %s is %s, wanted %s' % (names[0], got, want))
        P = dict((name, find_param(material, name)) for name, _ in SCALARS + VECTORS + WEATHER_TEXTURES)
        # v3: the conservative density reaches the VAO through its Shadow Pass Switch, the extinction SubsurfaceColor through the other.
        cons_switch = verify_weather(material, nodes, P)
        verify_single_output_sources(material, vao, vao_wanted(vao, P, cons_switch))
        for prop, desc in (('MP_BASE_COLOR', 'FogMS_CloudAlbedo'), ('MP_EMISSIVE_COLOR', 'FogMS_ForwardLobe'), ('MP_AMBIENT_OCCLUSION', 'FogMS_CloudSkyAO')):
            src = mel.get_material_property_input_node(material, getattr(unreal.MaterialProperty, prop))
            require(src == nodes[desc], '%s is fed by %s, wanted %s' % (prop, src.get_name() if src else None, desc))
        verify_prefilter(material, nodes, P)
        if g is not None:
            for node, wanted in g.links.items():
                verify_custom(material, node, wanted)
            for node, wanted in g.plain.items():
                verify_single_output_sources(material, node, wanted)
        else:
            # Existing asset: the field pins are the ones that went wrong before (W40); check every Custom node's links against the
            # names: field pins by output, everything else must be wired (no dangling pin).
            field = [e for e in mel.get_material_expressions(material) if isinstance(e, unreal.MaterialExpressionTextureSampleParameterVolume)
                     and str(e.get_editor_property('parameter_name')) == 'FogMS_TransportField']
            require(len(field) == 1, 'expected one FogMS_TransportField sample, found %d' % len(field))
            for desc, node in nodes.items():
                links = custom_links(material, node)
                for pin, (src, out) in links.items():
                    require(src is not None, '%s.%s is not connected' % (desc, pin))
                    if src == field[0]:
                        want = 'RGB' if (desc, pin) == ('FogMS_EmissiveInjection', 'Field') else 'A'
                        require(out == want, '%s.%s reads field output %s, wanted %s' % (desc, pin, out, want))
    except Exception as e:
        problems.append(str(e))
    return problems


def editor_log_path():
    log_dir = unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_log_dir())
    for token in shlex.split(unreal.SystemLibrary.get_command_line(), posix=False):
        token = token.strip('"')
        low = token.lower()
        if low.startswith('-abslog='):
            return token.split('=', 1)[1].strip('"')
        if low.startswith('-log='):
            name = token.split('=', 1)[1].strip('"')
            return name if os.path.isabs(name) else os.path.join(log_dir, name)
    logs = sorted(glob.glob(os.path.join(log_dir, '*.log')), key=os.path.getmtime, reverse=True)
    return logs[0] if logs else None


def compile_and_check(material, marker):
    """Recompile, wait for this material's shaders (get_statistics runs FinishCompilation), scan the log after `marker`."""
    unreal.log(marker)
    errors = [str(err) for err in (mel.recompile_material(material) or [])]
    if errors:
        return errors, 'translation'
    mel.get_statistics(material)
    try:
        unreal.SystemLibrary.execute_console_command(None, 'FLUSHLOG')
    except Exception:
        pass
    path = editor_log_path()
    if not path or not os.path.isfile(path):
        return [], 'shader result NOT verified (editor log not found)'
    with open(path, 'rb') as f:
        f.seek(max(0, os.path.getsize(path) - (16 << 20)))
        text = f.read().decode('utf-8', errors='replace')
    if marker not in text:
        return [], 'shader result NOT verified (marker not in %s)' % path
    lines = text.rsplit(marker, 1)[1].splitlines()
    failed = [i for i, line in enumerate(lines) if 'Failed to compile Material' in line and MATERIAL_NAME in line]
    if failed:
        return ['\n'.join(lines[i:i + 12]) for i in failed], 'shader (log %s)' % path
    return [], 'shader compiled (log %s)' % path


def ensure_instance(material):
    """MI_FogMS_Cloud: a MaterialInstanceConstant of M_FogMS_Cloud without overrides. Returns (instance, created)."""
    mi = unreal.load_asset(INSTANCE_PATH) if unreal.EditorAssetLibrary.does_asset_exist(INSTANCE_PATH) else None
    if mi is not None:
        require(isinstance(mi, unreal.MaterialInstanceConstant), INSTANCE_PATH + ' is not a Material Instance Constant')
        parent = mi.get_editor_property('parent')
        require(parent == material, '%s parent is %s, not %s (fix it by hand)' % (INSTANCE_PATH, parent.get_path_name() if parent else None, MATERIAL_PATH))
        return mi, False
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    mi = tools.create_asset(INSTANCE_NAME, ASSET_DIR, unreal.MaterialInstanceConstant, unreal.MaterialInstanceConstantFactoryNew())
    require(mi is not None, 'Cannot create ' + INSTANCE_PATH)
    mel.set_material_instance_parent(mi, material)
    require(mi.get_editor_property('parent') == material, 'MI parent readback failed')
    return mi, True


def package_file(asset_path):
    """The .uasset of a /MultiLobeSpec/... asset (plugin Content folder); None when it cannot be located."""
    for plugin_dir in (unreal.Paths.project_plugins_dir(),):
        path = os.path.join(unreal.SystemLibrary.convert_to_absolute_path(plugin_dir), 'MultiLobeSpec', 'Content',
                            asset_path[len('/MultiLobeSpec/'):] + '.uasset')
        return os.path.normpath(path)
    return None


def clear_expressions(material):
    """Deletes every expression one by one (delete_all_material_expressions left some behind in the first round-45 run)."""
    for _ in range(3):
        exprs = list(mel.get_material_expressions(material))
        if not exprs:
            return
        for e in exprs:
            mel.delete_material_expression(material, e)
    require(not list(mel.get_material_expressions(material)), 'Cannot clear the expressions of ' + material.get_name())


def rollback(material):
    """Nothing saved. An asset with a file on disk is reloaded from it; one that was never saved is emptied in memory (unsaved;
    the next run rebuilds it, restarting the editor discards it)."""
    try:
        path = package_file(MATERIAL_PATH)
        if path and os.path.isfile(path):
            pkg = material.get_outermost()
            unreal.EditorLoadingAndSavingUtils.reload_packages([pkg], unreal.ReloadPackagesInteractionMode.ASSUME_POSITIVE)
            print('ROLLBACK %s reloaded from disk (%s)' % (MATERIAL_PATH, path))
        else:
            clear_expressions(material)
            mel.recompile_material(material)
            print('ROLLBACK %s was never saved: emptied in memory (do not save it; the next run rebuilds it)' % MATERIAL_PATH)
    except Exception as e:
        print('ROLLBACK incomplete (%s): reload %s by hand, do not save it' % (e, MATERIAL_PATH))


def main():
    extinction_code = matedit_code('EXTINCTION_CODE_V3')
    lobe_code = matedit_code('FORWARD_LOBE_CODE_V2')
    report_only = os.environ.get('FOGMS_MATEDIT_MODE', '') == 'report'
    material = unreal.load_asset(MATERIAL_PATH) if unreal.EditorAssetLibrary.does_asset_exist(MATERIAL_PATH) else None
    problems = verify(material, extinction_code, lobe_code) if material is not None else ['missing']
    if report_only:
        print('REPORT %s: %s' % (MATERIAL_PATH, 'current (%s)' % MARKER if not problems else problems))
        return
    if not problems:
        mi, created = ensure_instance(material)
        saved = unreal.EditorAssetLibrary.save_asset(INSTANCE_PATH, only_if_is_dirty=False) if created else False
        print('ALREADY_PATCHED %s (%s); %s %s' % (MATERIAL_PATH, MARKER, INSTANCE_PATH, 'created saved=%s' % saved if created else 'present'))
        return
    created = material is None
    if created:
        tools = unreal.AssetToolsHelpers.get_asset_tools()
        material = tools.create_asset(MATERIAL_NAME, ASSET_DIR, unreal.Material, unreal.MaterialFactoryNew())
        require(material is not None, 'Cannot create ' + MATERIAL_PATH)
        print('CREATED', MATERIAL_PATH)
    else:
        print('REBUILD %s: %s' % (MATERIAL_PATH, problems))
        clear_expressions(material)
    try:
        # Blend mode before the domain: a new material is Opaque, and Volume + Opaque logs a transient 'Failed to compile'.
        material.set_editor_property('blend_mode', unreal.BlendMode.BLEND_ADDITIVE)
        material.set_editor_property('shading_model', unreal.MaterialShadingModel.MSM_DEFAULT_LIT)
        material.set_editor_property('material_domain', unreal.MaterialDomain.MD_VOLUME)
        usage = getattr(mel, 'set_base_material_usage', None) or mel.set_material_usage
        usage(material, unreal.MaterialUsage.MATUSAGE_VOLUMETRIC_CLOUD)
        g = build(material, extinction_code, lobe_code)
        errors, note = compile_and_check(material, 'FOGMS_MATEDIT_CLOUD_%d' % int(time.time() * 1000))
        print('COMPILE %s: %s' % (MATERIAL_NAME, note))
        require(not errors, 'Compile errors: %s' % errors)
        after = verify(material, extinction_code, lobe_code, g)
        require(not after, 'Readback after build: %s' % after)
        after = verify(material, extinction_code, lobe_code)   # the check an ALREADY_PATCHED run does must pass too
        require(not after, 'Name-based readback after build: %s' % after)
        print('LINKS verified by T3D: %d Custom nodes, %d pins; %d other nodes' % (
            len(g.links), sum(len(v) for v in g.links.values()), len(g.plain)))
    except Exception:
        print('TRACE', traceback.format_exc())
        rollback(material)
        print('NOT SAVED')
        return
    saved = unreal.EditorAssetLibrary.save_asset(MATERIAL_PATH, only_if_is_dirty=False)
    mi, mi_created = ensure_instance(material)
    mi_saved = unreal.EditorAssetLibrary.save_asset(INSTANCE_PATH, only_if_is_dirty=False)
    print('MATERIAL_OK saved=%s %s domain=%s blend=%s cloud_usage=%s; %s %s saved=%s' % (
        saved, MATERIAL_PATH, material.get_editor_property('material_domain'), material.get_editor_property('blend_mode'),
        mel.has_material_usage(material, unreal.MaterialUsage.MATUSAGE_VOLUMETRIC_CLOUD), INSTANCE_PATH,
        'created' if mi_created else 'present', mi_saved))


try:
    main()
except Exception:
    print('TRACE', traceback.format_exc())
