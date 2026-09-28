"""FogMS W48 + W49 (FogMS_Weather_Design.md 3.3, 3.6, 5.3-5.4, 6 'W48', 'W49'): the plugin's weather assets in /MultiLobeSpec/FogMS/Weather, created or
updated idempotently. Run in the editor with no other session editing these assets: `py "<this file>"` in the Python console, or through
the UE-MCP bridge with runpy (d48_weather.py matedit). Run it BEFORE matedit_cloud.py (the host material v3 takes the LUT / pattern / curl
as its texture defaults). FOGMS_MATEDIT_MODE=report only prints the checks.

1. Textures from the offline generator, reproducible: `python texgen/gen_weather_textures.py --out D:/FogMS_ProbeFrames/texgen` (numpy +
   Pillow, seed 11, periodic lattices, no third-party data; FOGMS_TEXGEN_DIR overrides the folder). TEXGEN_SHA256 = the PNGs of that run
   (checked twice, identical); another hash is imported with a NOTE (you regenerated with other settings). Generated files stay on D:,
   only the .uasset files are committed (they carry the PNG as source, 4-350 KB):
     T_FogMS_WeatherPattern  512^2 RGBA: R cumuliform, G stratiform, B storm seeds, A deck; every channel rank-equalised, so coverage C
                             thresholded as field > 1 - C covers exactly C of the area. Uncompressed BGRA8 (TC_VectorDisplacementmap: the
                             threshold relies on the 8-bit values), sRGB off, WRAP, mips (the shadow pass reads the detail at a coarse
                             mip), bilinear, never streamed.
     T_FogMS_Curl2D          128^2 RG: divergence-free curl of a tileable potential (0.5 = no motion). Same settings.
     T_FogMS_CloudTypeLUT    64 x 128 RGBA: U = cloud type (0 St .. 1 Cb), V = 1 - height in the layer; R profile, G softness, B billowy,
                             A anvil. Uncompressed, sRGB off, CLAMP, no mips, bilinear.
   Each texture keeps its PNG's SHA-256 in the metadata tag FogMS_SourceSHA256: same hash and settings = unchanged.
2. M_FogMS_WeatherCompose (Surface, Unlit, Alpha Composite, Apply Fogging off): one texel of RT_FogMS_WeatherMap per pixel of a
   DrawMaterialToRenderTarget draw (AFogMSWeather, FogMS_Weather.cpp). Custom FogMS_WeatherCompose(UV, Pattern, P0, P1) = float4(R coverage
   L0, G type L0, B storm, A deck L1) -> FogMS_WeatherComposeRGB -> Emissive, FogMS_WeatherComposeOpacity (1 - A) -> Opacity. The actor
   clears the map to (0,0,0,1) first; Alpha Composite blends RGB = src + dst (1 - a) = emissive and A = dst.a (1 - a) = 1 - opacity = deck.
   Parameters: FogMS_WeatherComposeP0 = (coverage, type, edge width, deck coverage), FogMS_WeatherComposeP1 = (storm, calibrate, 0, 0),
   texture object FogMS_WeatherPattern. P1.y = 1 draws the constant (0.25, 0.5, 0.75, 0.125) for the actor's one-time RGBA write check.
3. M_FogMS_WeatherSun (Surface, Unlit, Opaque): one texel of RT_FogMS_WeatherSun (R16F, thin layer only) = the optical depth of every weather
   layer along the sun line through the texel's ground point: Custom FogMS_WeatherSunOD = WEATHER_FN_CODE (read verbatim from
   matedit_cloud.py: the same density function as the host's shadow branch) + 24 midpoint samples -> Emissive. Parameters
   FogMS_WeatherDomain / _L0 / _L1 / _SunDir, texture objects FogMS_WeatherMap / _TypeLUT / _Pattern / _Curl.
4. DA_FogMS_Weather_Clear / _Scattered / _Broken / _Overcast: FogMS Weather State data assets (UFogMSWeatherState.apply_preset; the numbers
   live in C++, FogMS_Weather.cpp GetPresetValues). An existing asset whose Preset differs (edited by hand: Custom) is reported and left.
   W49: the W48 assets are not rewritten; UFogMSWeatherState::PostLoad gives a preset asset the preset's new fields (cirrus), printed here.
5. W49 M_FogMS_WeatherSky (Surface, Unlit, Opaque, Is Sky, Two Sided; FogMS_Weather_Design.md 3.2, 4.2, 6 'W49'): the material of
   AFogMSWeather::SkyDome (see the SKY_* constants for why an Is Sky dome is drawn behind everything and lands in the SkyLight capture).
   Graph: FogMS_SkyPointNear / Far / Cirrus (SKY_GEO_CODE + SKY_POINT_BODY, Which = Constant 0 / 1 / 2) -> SkyAtmosphereAerialPerspective and
   SkyAtmosphereLightIlluminance[0] (their World Position input) at those points; SkyAtmosphereLightDirection[0], LightDiskLuminance[0],
   ViewLuminance, DistantLightScatteredLuminance; Reflection Capture Pass Switch (Default Constant 1, Reflection Constant 0) -> Quality;
   CameraVectorWS, CameraPositionWS; all into FogMS_WeatherSky = WEATHER_FN_CODE (matedit_cloud.py, verbatim: the host shadow pass's
   density) + SKY_GEO_CODE + SKY_BODY -> Emissive. Parameters (group FogMS Weather Sky; the weather ones carry the host's names and
   values): FogMS_WeatherOrigin / _Domain / _Wind / _L0 / _L1, FogMS_SkyL2 / _SkyMarch / _SkyLight, FogMS_SkyOn (default 0: the plain sky),
   texture objects FogMS_WeatherMap / _TypeLUT / _Pattern / _Curl. The HLSL is DXC-checked standalone (scratchpad dxc_w49.py pattern).
Links are checked definitively from the T3D export (stored OutputIndex -> the source's output name), as matedit_cloud.py. Idempotent:
current assets print ALREADY_PATCHED and nothing changes. On any error nothing is saved; a material with a file on disk is reloaded from
it (rollback), a never-saved one is emptied in memory. Output: WEATHER_OK ... | ALREADY_PATCHED ... | TRACE <traceback>."""
import ast
import glob
import hashlib
import os
import re
import shlex
import tempfile
import time
import traceback

import unreal

WEATHER_DIR = '/MultiLobeSpec/FogMS/Weather'
COMPOSE_NAME = 'M_FogMS_WeatherCompose'
SUN_NAME = 'M_FogMS_WeatherSun'
COMPOSE_PATH = WEATHER_DIR + '/' + COMPOSE_NAME
SUN_PATH = WEATHER_DIR + '/' + SUN_NAME
GROUP = 'FogMS Weather'
MARKER = 'FogMS_Weather v1'
BLACK_TEXTURE = '/Engine/EngineResources/Black'
TEXGEN_DIR = os.environ.get('FOGMS_TEXGEN_DIR', 'D:/FogMS_ProbeFrames/texgen')
# (asset, png, address, mips): gen_weather_textures.py defaults (--size 512 --curl 128 --lut 64x128 --seed 11).
TEXTURES = (('T_FogMS_WeatherPattern', 'T_FogMS_WeatherPattern_512.png', 'wrap', True),
            ('T_FogMS_Curl2D', 'T_FogMS_Curl2D_128.png', 'wrap', True),
            ('T_FogMS_CloudTypeLUT', 'T_FogMS_CloudTypeLUT_64x128.png', 'clamp', False))
TEXTURE_SIZES = {'T_FogMS_WeatherPattern': (512, 512), 'T_FogMS_Curl2D': (128, 128), 'T_FogMS_CloudTypeLUT': (64, 128)}
# SHA-256 of the PNGs of `gen_weather_textures.py` with its defaults (2026-09-26, twice, identical).
TEXGEN_SHA256 = {'T_FogMS_WeatherPattern_512.png': '3106dd79f4f20c72cefd1f34f3da8bdc4698503a82f4a5ae7ca9e08515eb8433',
                 'T_FogMS_Curl2D_128.png': '5ea0d87cd583fb5b638a63ec43f2458e9a903c3294fc6d36d9ce65465420e704',
                 'T_FogMS_CloudTypeLUT_64x128.png': '8516a64f56a733f7e26bab7ef5a87ba24e50882b3e55f76a02d693c00c9fe6d5'}
HASH_TAG = 'FogMS_SourceSHA256'
PRESETS = (('DA_FogMS_Weather_Clear', 'CLEAR'), ('DA_FogMS_Weather_Scattered', 'SCATTERED'),
           ('DA_FogMS_Weather_Broken', 'BROKEN'), ('DA_FogMS_Weather_Overcast', 'OVERCAST'))
mel = unreal.MaterialEditingLibrary

COMPOSE_CODE = r"""// %s: FogMS_WeatherCompose (W48, matedit_weather.py): one texel of RT_FogMS_WeatherMap (R coverage L0, G type L0, B storm /
// precipitation, A deck L1) from T_FogMS_WeatherPattern (rank-equalised channels: R cumuliform, G stratiform, B storm seeds, A deck) and
// the blended state: P0 = (coverage L0, type L0, edge width, deck coverage), P1 = (storm, calibrate, 0, 0). The map covers ONE pattern
// tile = the weather domain; the wind moves it at lookup (the host), so it is redrawn only when the state changes.
// Coverage C is an exact area fraction: the type blends the stratiform and cumuliform fields, mixv = smoothstep(0.2, 0.6, type), and the
// threshold z solves P((1 - mixv) X + mixv Y > z) = C for independent uniform X, Y (trapezoid CDF; gen_weather_textures.py
// threshold_for_mix: the covered area stays C during a type transition).
// Calibrate (P1.y 1, the actor's one-time RGBA write check): the constant (0.25, 0.5, 0.75, 0.125).
if (P1.y > 0.5f) return float4(0.25f, 0.5f, 0.75f, 0.125f);
float C = saturate(P0.x);
float T = saturate(P0.y);
float w = max(P0.z, 1e-3f);
float D = saturate(P0.w);
float4 p = Pattern.SampleLevel(PatternSampler, UV, 0.0f);
float mixv = smoothstep(0.2f, 0.6f, T);
float f = lerp(p.g, p.r, mixv);
float a = min(mixv, 1.0f - mixv);
float b = max(mixv, 1.0f - mixv);
float q = 1.0f - C;
float z = q;
if (a >= 1e-6f)
{
    if (q <= a / (2.0f * b)) z = sqrt(2.0f * a * b * q);
    else if (q <= 1.0f - a / (2.0f * b)) z = 0.5f * a + b * q;
    else z = 1.0f - sqrt(2.0f * a * b * (1.0f - q));
}
float cov = C > 0.0f ? smoothstep(z - w, z + w, f) : 0.0f;
float deck = D > 0.0f ? smoothstep((1.0f - D) - w, (1.0f - D) + w, p.a) : 0.0f;
return float4(cov, T, saturate(P1.x), deck);
""" % MARKER
COMPOSE_RGB_CODE = """// FogMS_WeatherComposeRGB (W48): RGB of RT_FogMS_WeatherMap -> Emissive (Alpha Composite: RGB = emissive over the cleared map).
return Value.rgb;"""
COMPOSE_OPACITY_CODE = """// FogMS_WeatherComposeOpacity (W48): Opacity = 1 - deck, so the map's alpha (cleared to 1) becomes 1 - opacity = deck.
return 1.0f - Value.a;"""
SUN_OD_BODY = r"""// W50 shared remaining-column integral, at the ground plane. Wind-free periodic optical depth.
FogMSWeatherFn W;
float OD = W.SunOpticalDepth(UV / Domain.x, 0.0f, SunDir.xyz, Domain, L0, L1,
    Map, MapSampler, TypeLUT, TypeLUTSampler, Pattern, PatternSampler, Curl, CurlSampler);
return float3(OD, 0.0f, 0.0f);
"""

COMPOSE_PINS = ('UV', 'Pattern', 'P0', 'P1')
SUN_PINS = ('UV', 'Domain', 'L0', 'L1', 'SunDir', 'Map', 'TypeLUT', 'Pattern', 'Curl')
SUN_VECTORS = (('FogMS_WeatherDomain', (5.0e-7, 4.0e-6, 0.05, 0.0)), ('FogMS_WeatherL0', (0.0, 0.0, 0.0, 0.0)),
               ('FogMS_WeatherL1', (0.0, 0.0, 0.0, 0.0)), ('FogMS_WeatherSunDir', (0.0, 0.0, 1.0, 0.0)))
COMPOSE_VECTORS = (('FogMS_WeatherComposeP0', (0.0, 0.5, 0.04, 0.0)), ('FogMS_WeatherComposeP1', (0.0, 0.0, 0.0, 0.0)))

# ---------------------------------------------------------------------------------------------------------------- W49 sky dome
# M_FogMS_WeatherSky (Surface, Unlit, Opaque, Is Sky, Two Sided) on AFogMSWeather::SkyDome. The engine draws an Is Sky mesh only in the
# SkyPass (after the base pass, depth test, NO depth write; ShouldIncludeMaterialInDefaultOpaquePass keeps it out of the depth prepass and
# the base pass): sky pixels keep the far depth, so the SkyAtmosphere pass adds no aerial perspective on top of the dome, height / volumetric
# fog treat them as sky, and the cloud host composes over them exactly as over the atmosphere's own sky. With an Is Sky mesh in the scene the
# SkyAtmosphere stops drawing sky pixels (bRenderSkyPixel = !bSceneHasSkyMaterial) and the real-time sky capture renders the sky meshes
# instead of the atmosphere: the dome draws the atmosphere itself (SkyAtmosphereViewLuminance + LightDiskLuminance) and so lands in the
# SkyLight capture with the weather clouds.
SKY_NAME = 'M_FogMS_WeatherSky'
SKY_PATH = WEATHER_DIR + '/' + SKY_NAME
SKY_GROUP = 'FogMS Weather Sky'
SKY_MARKER = 'FogMS_WeatherSky v1'
SKY_GEO_CODE = r"""// FogMSSkyGeo v1 (W49, matedit_weather.py SKY_GEO_CODE; the same text in every M_FogMS_WeatherSky Custom node that needs the view ray).
// The view ray against spherical shells around the SkyAtmosphere planet (the planet the cloud host's layer and Cloud Sample Attributes.
// Altitude use), in the view's own sky frame (the real-time sky capture sets it for its capture position): H = |sky camera - planet centre|
// (View.SkyPlanetTranslatedWorldCenterAndViewHeight.w, computed on the CPU in double), R = the ground radius, h0 = H - R the camera altitude,
// Mu / MuL = cos(zenith) of the view / sun direction at the camera. Lengths in cm.
//   Alt(t)      altitude of the ray point at distance t (|C + tV| - R without cancellation)
//   CosAt(t,..) cos(zenith) of a direction D at the ray point t: (H dot(D, up_cam) + t dot(D, V)) / (R + Alt(t))
//   Sphere(A)   the ray interval inside the sphere of altitude A (stable quadratic roots; (1, 0) = none)
//   Shell(..)   the first ray segment inside the shell [Lo, Hi], ended by the ground and TMax ((1, 0) = none)
//   Thin(A,..)  the distance to the thin shell A (its exit from below, its entry from above), -1 = none / behind the ground / beyond TMax
struct FogMSSkyGeo
{
    float H;
    float R;
    float h0;
    float Mu;
    float MuL;
    void Init(float3 V, float3 L)
    {
        float3 C = ResolvedView.SkyCameraTranslatedWorldOrigin - ResolvedView.SkyPlanetTranslatedWorldCenterAndViewHeight.xyz;
        float3 Up = normalize(C);
        H = max(ResolvedView.SkyPlanetTranslatedWorldCenterAndViewHeight.w, 1.0f);
        R = ResolvedView.SkyAtmosphereBottomRadiusKm * 100000.0f;
        h0 = H - R;
        Mu = dot(V, Up);
        MuL = dot(L, Up);
    }
    float Alt(float t)
    {
        float q = t * (2.0f * H * Mu + t);
        return h0 + q / (sqrt(max(H * H + q, 0.0f)) + H);
    }
    float CosAt(float t, float DUp, float DV)
    {
        return (H * DUp + t * DV) / max(R + Alt(t), 1.0f);
    }
    float2 Sphere(float A)
    {
        float b = H * Mu;
        float k = (h0 - A) * (2.0f * R + h0 + A);
        float disc = b * b - k;
        if (disc < 0.0f) return float2(1.0f, 0.0f);
        float s = sqrt(disc);
        float q = b >= 0.0f ? -(b + s) : (s - b);
        if (abs(q) < 1e-6f) return float2(1.0f, 0.0f);
        float r1 = k / q;
        float t0 = min(q, r1);
        float t1 = max(q, r1);
        if (t1 <= 0.0f) return float2(1.0f, 0.0f);
        return float2(max(t0, 0.0f), t1);
    }
    float2 Shell(float Lo, float Hi, float TMax)
    {
        float2 O = Sphere(Hi);
        if (!(O.x < O.y)) return float2(1.0f, 0.0f);
        float2 I = Sphere(Lo);
        float2 G = Sphere(0.0f);
        float t0 = O.x;
        float t1 = O.y;
        if (I.x < I.y)
        {
            if (I.x <= t0) t0 = max(t0, I.y);
            else t1 = min(t1, I.x);
        }
        if (G.x < G.y) t1 = min(t1, G.x);
        t1 = min(t1, TMax);
        return t0 < t1 ? float2(t0, t1) : float2(1.0f, 0.0f);
    }
    float Thin(float A, float TMax)
    {
        float2 S = Sphere(A);
        if (!(S.x < S.y)) return -1.0f;
        float t = h0 < A ? S.y : S.x;
        float2 G = Sphere(0.0f);
        if (G.x < G.y && G.x < t) return -1.0f;
        return (t > 0.0f && t <= TMax) ? t : -1.0f;
    }
};
"""
SKY_POINT_BODY = r"""// FogMS_SkyPoint v1 (W49, matedit_weather.py): absolute world position [cm] of one point on this pixel's view ray for the SkyAtmosphere
// nodes of M_FogMS_WeatherSky (aerial perspective and sun illuminance at that point): Which 0 = the start of the first cloud segment (L0 or
// the deck L1), 1 = the end of the last one, 2 = the cirrus layer L2. Same geometry as FogMS_WeatherSky (FogMSSkyGeo, TMax = March.x, 2 x
// for the cirrus), so the march interpolates between exactly these points. No segment: a point 10 km along the ray (then unused).
float3 V = -normalize(CamVec);
FogMSSkyGeo Geo;
Geo.Init(V, float3(0.0f, 0.0f, 1.0f));
float TMax = max(March.x, 1000.0f);
float t = 1.0e6f;
if (Which < 1.5f)
{
    float2 S0 = L0.z > 0.0f ? Geo.Shell(L0.x, L0.y, TMax) : float2(1.0f, 0.0f);
    float2 S1 = L1.z > 0.0f ? Geo.Shell(L1.x, L1.y, TMax) : float2(1.0f, 0.0f);
    bool B0 = S0.x < S0.y;
    bool B1 = S1.x < S1.y;
    if (B0 || B1)
        t = Which < 0.5f ? min(B0 ? S0.x : 1.0e30f, B1 ? S1.x : 1.0e30f) : max(B0 ? S0.y : 0.0f, B1 ? S1.y : 0.0f);
}
else if (L2.x > 0.0f && L2.z > 0.0f)
{
    float t2 = Geo.Thin(L2.y, 2.0f * TMax);
    if (t2 > 0.0f) t = t2;
}
return CamWS + V * t;
"""
SKY_BODY = r"""// FogMS_WeatherSky v1 (W49, matedit_weather.py; FogMS_Weather_Design.md 1.4, 3.2, 3.5, 3.10, 4.2): the visible FogMS Weather sky on the Is
// Sky dome. Luminance [cd/m2, not pre-exposed: the base pass pre-exposes, with the capture's exposure in the real-time sky capture]:
//   Bg     = SkyAtmosphereViewLuminance + SkyAtmosphereLightDiskLuminance: the sky the SkyAtmosphere draws itself without an Is Sky mesh (the
//            same Sky View LUT; the disk is 0 in reflection captures), with the cirrus L2 composited: a 2D layer at L2.y, optical depth
//            L2.z x coverage (the weather pattern stretched 4:1 along L2.w, curl-warped), ice phase 0.8 HG(0.75) + 0.2 HG(-0.2), lit by
//            SunCirrus and the sky, its own aerial perspective APCirrus.
//   L0, L1 = a march along the view ray through the shells [base, top] (FogMSSkyGeo) with the SAME density as the cloud host's shadow pass
//            (FogMSWeatherFn: weather map x cloud-type LUT x pattern noise; the same origin / wind / domain / layer parameters), so the
//            clouds sit where their ground shadows are; the detail is read at the mip of the pixel footprint / step.
//            Per sample (sigma_s = albedo 0.98 x sigma_t): sun E(t) (SkyAtmosphereLightIlluminance, SunNear..SunFar) x [dual-lobe HG x
//            exp(-OD to the sun)] (L0: March.w samples through L0 + the deck column at the sun crossing; deck: its column above, analytic)
//            + E mu0 D / (2 pi): the two-stream diffuse sun below the layers above (T = 2 mu0 / (2 mu0 + (1 - g) tau), g 0.85, D = T minus
//            the direct beam; design 1.4) + the sky: SkyAtmosphereDistantLightScatteredLuminance x saturate(Light.w + height in layer) x
//            the deck's two-stream sky transmission. Energy-conserving steps (Hillaire 2016), front to back, stop at transmittance 0.02.
//   AP     = SkyAtmosphereAerialPerspective at the transmittance-weighted cloud depth (APNear..APFar): Clouds x AP.a + (1 - T) x AP.rgb.
//   out    = Clouds + T x Bg.
// Quality (Reflection Capture Pass Switch: 1 in views, 0 in reflection and real-time sky captures): the capture marches 6 / 4 steps with
// one sun sample, no jitter, one detail mip coarser. On 0 (the material default, a dome no weather actor feeds): Bg only = the plain sky.
float3 V = -normalize(CamVec);
float3 Ls = normalize(SunDir);
float3 Bg = SkyBg + SunDisk;
if (On < 0.5f) return Bg;
FogMSSkyGeo Geo;
Geo.Init(V, Ls);
FogMSWeatherFn W;
const float Pi4 = 12.5663706f;
bool bView = Quality > 0.5f;
float TMax = max(March.x, 1000.0f);
float Mu = dot(V, Ls);
float PixAngle = 2.0f * ResolvedView.ViewSizeAndInvSize.w / max(abs(ResolvedView.ViewToClip[1][1]), 1e-3f);
float Texel = 1.0f / max(Domain.y * 512.0f, 1e-9f);
float MipBias = bView ? 0.0f : 1.0f;
float Jitter = 0.5f;
if (bView)
{
    float2 Px = Parameters.SvPosition.xy + 5.588238f * float(View.StateFrameIndexMod8);
    Jitter = frac(52.9829189f * frac(dot(Px, float2(0.06711056f, 0.00583715f))));
}
// ---- L2 cirrus into the background
if (L2.x > 0.0f && L2.z > 0.0f)
{
    float T2 = Geo.Thin(L2.y, 2.0f * TMax);
    if (T2 > 0.0f)
    {
        float2 XY2 = CamWS.xy + V.xy * T2 - Origin.xy - Wind.xy;
        float2 Dir = float2(cos(L2.w), sin(L2.w));
        float2 UV2 = float2(dot(XY2, Dir) * 0.25f, dot(XY2, float2(-Dir.y, Dir.x))) * (Domain.y * 0.5f);
        float Mip2 = clamp(log2(max(T2 * PixAngle, 1.0f) / (2.0f * Texel)), 0.0f, 6.0f) + MipBias;
        float2 Cw = Curl.SampleLevel(CurlSampler, UV2 * 0.5f, Mip2).rg * 2.0f - 1.0f;
        float4 P2 = Pattern.SampleLevel(PatternSampler, UV2 + Cw * 0.15f, Mip2);
        float F2 = lerp(P2.g, P2.r, 0.25f);
        float Cov2 = smoothstep(1.0f - L2.x - 0.08f, 1.0f - L2.x + 0.08f, F2) * (1.0f - smoothstep(1.2f * TMax, 2.0f * TMax, T2));
        float CosV2 = abs(Geo.CosAt(T2, Geo.Mu, 1.0f));
        float A2 = 1.0f - exp(-L2.z * Cov2 / max(CosV2, 0.08f));
        float Hg1 = (1.0f - 0.5625f) / (Pi4 * pow(max(1.5625f - 1.5f * Mu, 1e-4f), 1.5f));
        float Hg2 = (1.0f - 0.04f) / (Pi4 * pow(max(1.04f + 0.4f * Mu, 1e-4f), 1.5f));
        float3 C2 = 0.9f * (SunCirrus * (0.8f * Hg1 + 0.2f * Hg2) + Ambient);
        Bg = Bg * (1.0f - A2) + A2 * (C2 * APCirrus.a + APCirrus.rgb);
    }
}
// ---- L0 / L1 segments, front to back
float2 S0 = L0.z > 0.0f ? Geo.Shell(L0.x, L0.y, TMax) : float2(1.0f, 0.0f);
float2 S1 = L1.z > 0.0f ? Geo.Shell(L1.x, L1.y, TMax) : float2(1.0f, 0.0f);
bool B0 = S0.x < S0.y;
bool B1 = S1.x < S1.y;
if (!(B0 || B1)) return Bg;
float TNear = min(B0 ? S0.x : 1.0e30f, B1 ? S1.x : 1.0e30f);
float TFar = max(B0 ? S0.y : 0.0f, B1 ? S1.y : 0.0f);
float InvRange = 1.0f / max(TFar - TNear, 1.0f);
bool DeckFirst = B1 && (!B0 || S1.x < S0.x);
const float Gts = 0.85f;
const float Albedo = 0.98f;
float4 Zero4 = float4(0.0f, 0.0f, 0.0f, 0.0f);
int N0 = bView ? clamp((int)March.y, 1, 64) : 6;
int N1 = bView ? clamp((int)March.z, 1, 32) : 4;
int K = bView ? clamp((int)March.w, 1, 8) : 1;
float G1 = clamp(Light.x, -0.95f, 0.95f);
float G2 = clamp(Light.y, -0.95f, 0.95f);
float Phase = lerp((1.0f - G1 * G1) / (Pi4 * pow(max(1.0f + G1 * G1 - 2.0f * G1 * Mu, 1e-4f), 1.5f)),
                   (1.0f - G2 * G2) / (Pi4 * pow(max(1.0f + G2 * G2 - 2.0f * G2 * Mu, 1e-4f), 1.5f)), saturate(Light.z));
float3 Lsum = float3(0.0f, 0.0f, 0.0f);
float Tv = 1.0f;
float Wt = 0.0f;
float Ws = 0.0f;
[loop] for (int s = 0; s < 2; ++s)
{
    bool Deck = (s == 0) == DeckFirst;
    float2 S = Deck ? S1 : S0;
    if (!(S.x < S.y) || Tv < 0.02f) continue;
    int N = Deck ? N1 : N0;
    float4 LA = Deck ? Zero4 : L0;
    float4 LB = Deck ? L1 : Zero4;
    float Dt = (S.y - S.x) / N;
    [loop] for (int i = 0; i < N; ++i)
    {
        float T = S.x + (i + Jitter) * Dt;
        float Alt = Geo.Alt(T);
        float2 XY = CamWS.xy + V.xy * T - Origin.xy - Wind.xy;
        float4 Dom = Domain;
        Dom.w = clamp(log2(max(max(T * PixAngle, 0.5f * Dt), 1.0f) / Texel), 0.0f, 6.0f) + MipBias;
        float Fade = 1.0f - smoothstep(0.6f * TMax, TMax, T);
        float Sigma = Fade * W.Sigma(XY, Alt, Dom, LA, LB, Map, MapSampler, TypeLUT, TypeLUTSampler, Pattern, PatternSampler, Curl, CurlSampler);
        if (Sigma <= 1e-7f) continue;
        float3 E = lerp(SunNear, SunFar, saturate((T - TNear) * InvRange));
        float Mu0 = Geo.CosAt(T, Geo.MuL, Mu);
        float M0 = max(Mu0, 0.0f);
        float MuS = max(Mu0, 0.05f);
        float TauDeck = 0.0f;
        float ODs = 0.0f;
        if (Deck)
        {
            float4 Md = Map.SampleLevel(MapSampler, XY * Domain.x, 0.0f);
            TauDeck = L1.z * Md.a * max(L1.y - Alt, 0.0f) * 0.0068f;
        }
        else
        {
            float Dsun = min(max(L0.y - Alt, 0.0f) / MuS, 2.0f * (L0.y - L0.x));
            float Ds = Dsun / K;
            float4 DomS = Dom;
            DomS.w += 1.5f;
            [loop] for (int k = 0; k < K; ++k)
            {
                float D = (k + 0.5f) * Ds;
                ODs += W.Sigma(XY + Ls.xy * D, Alt + D * Mu0, DomS, L0, Zero4, Map, MapSampler, TypeLUT, TypeLUTSampler, Pattern,
                               PatternSampler, Curl, CurlSampler) * Ds * 0.01f;
            }
            if (L1.z > 0.0f && L1.x > Alt && M0 > 0.0f)
            {
                float DDeck = (0.5f * (L1.x + L1.y) - Alt) / MuS;
                float4 Md = Map.SampleLevel(MapSampler, (XY + Ls.xy * DDeck) * Domain.x, 0.0f);
                TauDeck = L1.z * Md.a * (L1.y - L1.x) * 0.0068f;
            }
        }
        float TauV = TauDeck + ODs * M0;
        float Tdir = exp(-ODs - TauDeck / MuS);
        float Ttot = 2.0f * M0 / (2.0f * M0 + (1.0f - Gts) * TauV + 1e-6f);
        float Dif = max(Ttot - exp(-TauV / MuS), 0.0f);
        float Hf = Deck ? saturate((Alt - L1.x) / max(L1.y - L1.x, 1.0f)) : saturate((Alt - L0.x) / max(L0.y - L0.x, 1.0f));
        float Tsky = 2.0f / (2.0f + (1.0f - Gts) * TauDeck);
        float3 Sc = E * (Phase * Tdir + M0 * Dif * 0.15915494f) + Ambient * (saturate(Light.w + Hf) * Tsky);
        float Tstep = exp(-Sigma * Dt * 0.01f);
        float Wgt = Tv * (1.0f - Tstep);
        Lsum += Sc * (Albedo * Wgt);
        Wt += Wgt * T;
        Ws += Wgt;
        Tv *= Tstep;
        if (Tv < 0.02f) break;
    }
}
float Tm = Ws > 0.0f ? Wt / Ws : TNear;
float4 AP = lerp(APNear, APFar, saturate((Tm - TNear) * InvRange));
return Lsum * AP.a + (1.0f - Tv) * AP.rgb + Tv * Bg;
"""
SKY_POINT_PINS = ('CamVec', 'CamWS', 'L0', 'L1', 'L2', 'March', 'Which')
SKY_PINS = ('CamVec', 'CamWS', 'SunDir', 'SunNear', 'SunFar', 'SunCirrus', 'APNear', 'APFar', 'APCirrus', 'Ambient', 'SkyBg', 'SunDisk',
            'Quality', 'On', 'Origin', 'Domain', 'Wind', 'L0', 'L1', 'L2', 'March', 'Light', 'Map', 'TypeLUT', 'Pattern', 'Curl')
# (parameter, default): the weather parameters carry the cloud host's names and values (AFogMSWeather writes both); defaults inert (sigma 0).
SKY_VECTORS = (('FogMS_WeatherOrigin', (0.0, 0.0, 0.0, 0.0)), ('FogMS_WeatherDomain', (5.0e-7, 4.0e-6, 0.05, 0.0)),
               ('FogMS_WeatherWind', (0.0, 0.0, 0.0, 0.0)), ('FogMS_WeatherL0', (0.0, 0.0, 0.0, 0.0)), ('FogMS_WeatherL1', (0.0, 0.0, 0.0, 0.0)),
               # cirrus (coverage, altitude cm, optical depth, streak direction rad); march (max distance cm, L0 steps, deck steps, sun
               # samples); light (phase g forward, phase g back, back-lobe weight, sky light at the cloud base)
               ('FogMS_SkyL2', (0.0, 800000.0, 0.5, 0.5236)), ('FogMS_SkyMarch', (6000000.0, 20.0, 8.0, 4.0)),
               ('FogMS_SkyLight', (0.8, -0.3, 0.25, 0.5)))
SKY_SCALARS = (('FogMS_SkyOn', 0.0),)
SKY_TEXTURES = (('FogMS_WeatherMap', BLACK_TEXTURE), ('FogMS_WeatherTypeLUT', WEATHER_DIR + '/T_FogMS_CloudTypeLUT'),
                ('FogMS_WeatherPattern', WEATHER_DIR + '/T_FogMS_WeatherPattern'), ('FogMS_WeatherCurl', WEATHER_DIR + '/T_FogMS_Curl2D'))
# Custom pin <- parameter (name, output) of FogMS_WeatherSky / FogMS_SkyPoint*
SKY_PARAM_PINS = {'On': ('FogMS_SkyOn', ''), 'Origin': ('FogMS_WeatherOrigin', 'RGBA'), 'Domain': ('FogMS_WeatherDomain', 'RGBA'),
                  'Wind': ('FogMS_WeatherWind', 'RGBA'), 'L0': ('FogMS_WeatherL0', 'RGBA'), 'L1': ('FogMS_WeatherL1', 'RGBA'),
                  'L2': ('FogMS_SkyL2', 'RGBA'), 'March': ('FogMS_SkyMarch', 'RGBA'), 'Light': ('FogMS_SkyLight', 'RGBA'),
                  'Map': ('FogMS_WeatherMap', ''), 'TypeLUT': ('FogMS_WeatherTypeLUT', ''), 'Pattern': ('FogMS_WeatherPattern', ''),
                  'Curl': ('FogMS_WeatherCurl', '')}


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def probe_dir():
    here = globals().get('__file__')
    if here:
        return os.path.dirname(os.path.abspath(here))
    path = globals().get('P2_PROBE_DIR')
    require(path, 'P2_PROBE_DIR (folder of matedit_cloud.py) is not set')
    return path


def cloud_constant(name):
    """A string constant of matedit_cloud.py (read as text with ast; that script patches on import). String concatenations of earlier
    constants are evaluated in order."""
    if name == 'WEATHER_FN_CODE':
        with open(os.path.join(probe_dir(), '../../../Shaders/Private/FogMS_WeatherLighting.ush'), encoding='utf-8') as f:
            return f.read()
    path = os.path.join(probe_dir(), 'matedit_cloud.py')
    tree = ast.parse(open(path, encoding='utf-8').read())
    env = {}
    for node in tree.body:
        if isinstance(node, ast.Assign) and len(node.targets) == 1 and isinstance(node.targets[0], ast.Name):
            try:
                env[node.targets[0].id] = eval(compile(ast.Expression(node.value), path, 'eval'), {'__builtins__': {}}, env)
            except Exception:
                pass
    require(name in env and isinstance(env[name], str), '%s not found in %s' % (name, path))
    return env[name]


def norm(code):
    return str(code).replace('\r\n', '\n').strip()


def sha256(path):
    with open(path, 'rb') as f:
        return hashlib.sha256(f.read()).hexdigest()


# ------------------------------------------------------------------ definitive link readback (matedit_cloud.py / matedit_density.py W40)
_T3D_PIN = re.compile(r'^\s*Inputs\((\d+)\)=\(InputName="([^"]*)"(.*)$')
_T3D_SOURCE = re.compile(r'Expression="[^"\']*\'([^\']+)\'"')
_T3D_INDEX = re.compile(r'\bOutputIndex=(\d+)')


def export_t3d(obj):
    path = os.path.join(tempfile.gettempdir(), 'fogms_matedit_weather_%s_%d.t3d' % (obj.get_name(), int(time.time() * 1000)))
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
    stored = custom_links(material, node)
    require(sorted(stored) == sorted(wanted), '%s pins %s, wanted %s' % (node.get_name(), sorted(stored), sorted(wanted)))
    for pin, (src, out) in wanted.items():
        got = stored[pin]
        require(got[0] == src and got[1] == out, 'Link %s.%s is %s.%s, wanted %s.%s' % (
            node.get_name(), pin, got[0].get_name() if got[0] else None, got[1], src.get_name(), out))


def verify_single_output_sources(material, node, wanted):
    """Non-Custom node (W49: sky atmosphere nodes, Reflection Capture Pass Switch): wanted = {pin: source}; the sources must be
    single-output expressions (then the output index is 0 by construction) and stored exactly (matedit_cloud.py, same check)."""
    stored = dict(input_sources(material, node))
    for pin, src in wanted.items():
        got = stored.get(pin)
        require(got == src, 'Link %s.%s is %s, wanted %s' % (node.get_name(), pin, got.get_name() if got else None, src.get_name()))
        outs = [str(o) for o in mel.get_material_expression_output_names(src)]
        require(len(outs) <= 1, '%s feeds %s.%s but has outputs %s (use a Custom node check)' % (src.get_name(), node.get_name(), pin, outs))


def position_pin(node):
    """The world-position input of a SkyAtmosphereLightIlluminance / SkyAtmosphereAerialPerspective node ('World Position' with the
    default Absolute origin; the name comes from GetInputName, so it is looked up rather than assumed)."""
    names = [str(n) for n in mel.get_material_expression_input_names(node)]
    found = [n for n in names if 'position' in n.lower()]
    require(len(found) == 1, '%s inputs %s: no single position input' % (node.get_name(), names))
    return found[0]


class Graph:
    def __init__(self, material):
        self.m = material
        self.params = {}
        self.links = {}     # Custom node -> {pin: (source, output)}
        self.plain = {}     # other node -> {pin: source} (single-output sources only, W49)

    def node(self, cls, x, y, **props):
        e = mel.create_material_expression(self.m, cls, x, y)
        require(e is not None, 'Cannot create ' + cls.__name__)
        for k, v in props.items():
            e.set_editor_property(k, v)
        return e

    def link(self, src, out, dst, pin):
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

    def vector(self, name, default, x, y, group=GROUP):
        e = self.node(unreal.MaterialExpressionVectorParameter, x, y)
        e.set_editor_property('parameter_name', name)
        e.set_editor_property('default_value', unreal.LinearColor(*default))
        e.set_editor_property('group', group)
        self.params[name] = e
        return e

    def scalar(self, name, default, x, y, group=GROUP):
        e = self.node(unreal.MaterialExpressionScalarParameter, x, y)
        e.set_editor_property('parameter_name', name)
        e.set_editor_property('default_value', default)
        e.set_editor_property('group', group)
        self.params[name] = e
        return e

    def texture(self, name, path, x, y, group=GROUP):
        e = self.node(unreal.MaterialExpressionTextureObjectParameter, x, y)
        e.set_editor_property('parameter_name', name)
        e.set_editor_property('group', group)
        tex = unreal.load_asset(path)
        require(tex is not None, 'Missing texture ' + path)
        e.set_editor_property('texture', tex)
        self.params[name] = e
        return e


def custom_nodes(material):
    return [e for e in mel.get_material_expressions(material) if isinstance(e, unreal.MaterialExpressionCustom)]


def by_description(material, description):
    found = [e for e in custom_nodes(material) if str(e.get_editor_property('description')) == description]
    return found[0] if len(found) == 1 else None


def find_param(material, name):
    for e in mel.get_material_expressions(material):
        try:
            if str(e.get_editor_property('parameter_name')) == name:
                return e
        except Exception:
            pass
    return None


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
    failed = [i for i, line in enumerate(lines) if 'Failed to compile Material' in line and material.get_name() in line]
    if failed:
        return ['\n'.join(lines[i:i + 12]) for i in failed], 'shader (log %s)' % path
    return [], 'shader compiled (log %s)' % path


def package_file(asset_path):
    plugin_dir = unreal.SystemLibrary.convert_to_absolute_path(unreal.Paths.project_plugins_dir())
    return os.path.normpath(os.path.join(plugin_dir, 'MultiLobeSpec', 'Content', asset_path[len('/MultiLobeSpec/'):] + '.uasset'))


def clear_expressions(material):
    for _ in range(3):
        exprs = list(mel.get_material_expressions(material))
        if not exprs:
            return
        for e in exprs:
            mel.delete_material_expression(material, e)
    require(not list(mel.get_material_expressions(material)), 'Cannot clear the expressions of ' + material.get_name())


def rollback(material, path):
    try:
        f = package_file(path)
        if os.path.isfile(f):
            unreal.EditorLoadingAndSavingUtils.reload_packages([material.get_outermost()], unreal.ReloadPackagesInteractionMode.ASSUME_POSITIVE)
            print('ROLLBACK %s reloaded from disk (%s)' % (path, f))
        else:
            clear_expressions(material)
            mel.recompile_material(material)
            print('ROLLBACK %s was never saved: emptied in memory (do not save it; the next run rebuilds it)' % path)
    except Exception as e:
        print('ROLLBACK incomplete (%s): reload %s by hand, do not save it' % (e, path))


# ------------------------------------------------------------------ 1. textures
def texture_problems(name, png, address, mips):
    """[] when the asset is current: exists, same source hash tag, sRGB off, uncompressed, address, mips, size."""
    path = WEATHER_DIR + '/' + name
    if not unreal.EditorAssetLibrary.does_asset_exist(path):
        return ['missing']
    tex = unreal.load_asset(path)
    problems = []
    if not isinstance(tex, unreal.Texture2D):
        return ['%s is not a Texture2D' % path]
    tag = unreal.EditorAssetLibrary.get_metadata_tag(tex, HASH_TAG)
    if tag != sha256(png):
        problems.append('source hash %s != %s' % (tag, sha256(png)[:12]))
    if tex.get_editor_property('srgb'):
        problems.append('sRGB on')
    if tex.get_editor_property('compression_settings') != unreal.TextureCompressionSettings.TC_VECTOR_DISPLACEMENTMAP:
        problems.append('compression %s' % tex.get_editor_property('compression_settings'))
    want_addr = unreal.TextureAddress.TA_WRAP if address == 'wrap' else unreal.TextureAddress.TA_CLAMP
    if tex.get_editor_property('address_x') != want_addr or tex.get_editor_property('address_y') != want_addr:
        problems.append('address %s/%s' % (tex.get_editor_property('address_x'), tex.get_editor_property('address_y')))
    want_mips = unreal.TextureMipGenSettings.TMGS_FROM_TEXTURE_GROUP if mips else unreal.TextureMipGenSettings.TMGS_NO_MIPMAPS
    if tex.get_editor_property('mip_gen_settings') != want_mips:
        problems.append('mips %s' % tex.get_editor_property('mip_gen_settings'))
    if tex.get_editor_property('filter') != unreal.TextureFilter.TF_BILINEAR:
        problems.append('filter %s' % tex.get_editor_property('filter'))
    if not tex.get_editor_property('never_stream'):
        problems.append('streamed')
    size = (tex.blueprint_get_size_x(), tex.blueprint_get_size_y())
    if size != TEXTURE_SIZES[name]:
        problems.append('size %s != %s' % (size, TEXTURE_SIZES[name]))
    return problems


def import_texture(name, png, address, mips):
    task = unreal.AssetImportTask()
    task.set_editor_property('filename', png)
    task.set_editor_property('destination_path', WEATHER_DIR)
    task.set_editor_property('destination_name', name)
    task.set_editor_property('replace_existing', True)
    task.set_editor_property('automated', True)
    task.set_editor_property('save', False)
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
    tex = unreal.load_asset(WEATHER_DIR + '/' + name)
    require(isinstance(tex, unreal.Texture2D), 'import of %s failed' % png)
    tex.set_editor_property('srgb', False)
    tex.set_editor_property('compression_settings', unreal.TextureCompressionSettings.TC_VECTOR_DISPLACEMENTMAP)
    addr = unreal.TextureAddress.TA_WRAP if address == 'wrap' else unreal.TextureAddress.TA_CLAMP
    tex.set_editor_property('address_x', addr)
    tex.set_editor_property('address_y', addr)
    tex.set_editor_property('mip_gen_settings', unreal.TextureMipGenSettings.TMGS_FROM_TEXTURE_GROUP if mips else unreal.TextureMipGenSettings.TMGS_NO_MIPMAPS)
    tex.set_editor_property('filter', unreal.TextureFilter.TF_BILINEAR)
    tex.set_editor_property('never_stream', True)
    unreal.EditorAssetLibrary.set_metadata_tag(tex, HASH_TAG, sha256(png))
    return tex


# ------------------------------------------------------------------ 2./3. materials
def build_compose(material):
    g = Graph(material)
    uv = g.node(unreal.MaterialExpressionTextureCoordinate, -900, 0)
    pattern = g.texture('FogMS_WeatherPattern', WEATHER_DIR + '/T_FogMS_WeatherPattern', -900, 150)
    y = 350
    for name, default in COMPOSE_VECTORS:
        g.vector(name, default, -900, y); y += 150
    compose = g.custom('FogMS_WeatherCompose', COMPOSE_CODE, unreal.CustomMaterialOutputType.CMOT_FLOAT4, COMPOSE_PINS, -500, 100)
    g.link(uv, '', compose, 'UV'); g.link(pattern, '', compose, 'Pattern')
    g.link(g.params['FogMS_WeatherComposeP0'], 'RGBA', compose, 'P0'); g.link(g.params['FogMS_WeatherComposeP1'], 'RGBA', compose, 'P1')
    rgb = g.custom('FogMS_WeatherComposeRGB', COMPOSE_RGB_CODE, unreal.CustomMaterialOutputType.CMOT_FLOAT3, ('Value',), -150, 0)
    g.link(compose, '', rgb, 'Value')
    opacity = g.custom('FogMS_WeatherComposeOpacity', COMPOSE_OPACITY_CODE, unreal.CustomMaterialOutputType.CMOT_FLOAT1, ('Value',), -150, 250)
    g.link(compose, '', opacity, 'Value')
    require(mel.connect_material_property(rgb, '', unreal.MaterialProperty.MP_EMISSIVE_COLOR), 'Cannot route Emissive')
    require(mel.connect_material_property(opacity, '', unreal.MaterialProperty.MP_OPACITY), 'Cannot route Opacity')
    return g


def build_sun(material, sun_code):
    g = Graph(material)
    uv = g.node(unreal.MaterialExpressionTextureCoordinate, -900, 0)
    y = 150
    for name, default in SUN_VECTORS:
        g.vector(name, default, -900, y); y += 150
    tex = {'Map': g.texture('FogMS_WeatherMap', BLACK_TEXTURE, -900, y),
           'TypeLUT': g.texture('FogMS_WeatherTypeLUT', WEATHER_DIR + '/T_FogMS_CloudTypeLUT', -900, y + 150),
           'Pattern': g.texture('FogMS_WeatherPattern', WEATHER_DIR + '/T_FogMS_WeatherPattern', -900, y + 300),
           'Curl': g.texture('FogMS_WeatherCurl', WEATHER_DIR + '/T_FogMS_Curl2D', -900, y + 450)}
    od = g.custom('FogMS_WeatherSunOD', sun_code, unreal.CustomMaterialOutputType.CMOT_FLOAT3, SUN_PINS, -400, 200)
    g.link(uv, '', od, 'UV')
    for pin, name in (('Domain', 'FogMS_WeatherDomain'), ('L0', 'FogMS_WeatherL0'), ('L1', 'FogMS_WeatherL1'), ('SunDir', 'FogMS_WeatherSunDir')):
        g.link(g.params[name], 'RGBA', od, pin)
    for pin, node in tex.items():
        g.link(node, '', od, pin)
    require(mel.connect_material_property(od, '', unreal.MaterialProperty.MP_EMISSIVE_COLOR), 'Cannot route Emissive')
    return g


SKY_POINT_DESCS = ('FogMS_SkyPointNear', 'FogMS_SkyPointFar', 'FogMS_SkyPointCirrus')


def build_sky(material, codes):
    """W49 M_FogMS_WeatherSky (module docstring 5): three FogMS_SkyPoint nodes (Which 0 / 1 / 2) -> SkyAtmosphereAerialPerspective and
    SkyAtmosphereLightIlluminance[0] at the cloud segment start / end and the cirrus point; SkyAtmosphereLightDirection[0],
    LightDiskLuminance[0], ViewLuminance, DistantLightScatteredLuminance; Reflection Capture Pass Switch (Default 1, Reflection 0) -> Quality;
    everything into FogMS_WeatherSky -> Emissive."""
    g = Graph(material)
    y = -400
    for name, default in SKY_VECTORS:
        g.vector(name, default, -1900, y, SKY_GROUP); y += 140
    for name, default in SKY_SCALARS:
        g.scalar(name, default, -1900, y, SKY_GROUP); y += 100
    for name, path in SKY_TEXTURES:
        g.texture(name, path, -1900, y, SKY_GROUP); y += 140
    P = g.params
    camvec = g.node(unreal.MaterialExpressionCameraVectorWS, -1550, -450)
    campos = g.node(unreal.MaterialExpressionCameraPositionWS, -1550, -330)
    consts = [g.node(unreal.MaterialExpressionConstant, -1550, -150 + 100 * i, r=float(i)) for i in range(3)]
    aps, suns = [], []
    for i, desc in enumerate(SKY_POINT_DESCS):
        pt = g.custom(desc, codes['point'], unreal.CustomMaterialOutputType.CMOT_FLOAT3, SKY_POINT_PINS, -1250, -450 + 330 * i)
        g.link(camvec, '', pt, 'CamVec'); g.link(campos, '', pt, 'CamWS')
        for pin in ('L0', 'L1', 'L2', 'March'):
            name, out = SKY_PARAM_PINS[pin]
            g.link(P[name], out, pt, pin)
        g.link(consts[i], '', pt, 'Which')
        ap = g.node(unreal.MaterialExpressionSkyAtmosphereAerialPerspective, -900, -450 + 330 * i)
        g.link(pt, '', ap, position_pin(ap))
        sun = g.node(unreal.MaterialExpressionSkyAtmosphereLightIlluminance, -900, -330 + 330 * i, light_index=0)
        g.link(pt, '', sun, position_pin(sun))
        aps.append(ap); suns.append(sun)
    sundir = g.node(unreal.MaterialExpressionSkyAtmosphereLightDirection, -900, 600, light_index=0)
    disk = g.node(unreal.MaterialExpressionSkyAtmosphereLightDiskLuminance, -900, 700, light_index=0)
    view = g.node(unreal.MaterialExpressionSkyAtmosphereViewLuminance, -900, 800)
    amb = g.node(unreal.MaterialExpressionSkyAtmosphereDistantLightScatteredLuminance, -900, 900)
    sw = g.node(unreal.MaterialExpressionReflectionCapturePassSwitch, -900, 1000)
    g.link(consts[1], '', sw, 'Default'); g.link(consts[0], '', sw, 'Reflection')
    sky = g.custom('FogMS_WeatherSky', codes['sky'], unreal.CustomMaterialOutputType.CMOT_FLOAT3, SKY_PINS, -450, 200)
    sources = {'CamVec': camvec, 'CamWS': campos, 'SunDir': sundir, 'SunNear': suns[0], 'SunFar': suns[1], 'SunCirrus': suns[2],
               'APNear': aps[0], 'APFar': aps[1], 'APCirrus': aps[2], 'Ambient': amb, 'SkyBg': view, 'SunDisk': disk, 'Quality': sw}
    for pin in SKY_PINS:
        if pin in sources:
            g.link(sources[pin], '', sky, pin)
        else:
            name, out = SKY_PARAM_PINS[pin]
            g.link(P[name], out, sky, pin)
    require(mel.connect_material_property(sky, '', unreal.MaterialProperty.MP_EMISSIVE_COLOR), 'Cannot route Emissive')
    return g


def sky_settings(material):
    material.set_editor_property('blend_mode', unreal.BlendMode.BLEND_OPAQUE)
    material.set_editor_property('shading_model', unreal.MaterialShadingModel.MSM_UNLIT)
    material.set_editor_property('material_domain', unreal.MaterialDomain.MD_SURFACE)
    material.set_editor_property('is_sky', True)
    material.set_editor_property('two_sided', True)


def first_output(expr):
    outs = [str(o) for o in mel.get_material_expression_output_names(expr)]
    return outs[0] if outs else ''


def verify_sky(material, codes, g=None):
    """[] when M_FogMS_WeatherSky is current: settings, Custom codes, parameters and texture defaults, and every link (Custom pins from the
    T3D readback, the sky atmosphere nodes and the switch by their single-output sources)."""
    problems = []
    try:
        require(material.get_editor_property('material_domain') == unreal.MaterialDomain.MD_SURFACE, 'domain is not Surface')
        require(material.get_editor_property('blend_mode') == unreal.BlendMode.BLEND_OPAQUE, 'blend mode is not Opaque')
        require(material.get_editor_property('shading_model') == unreal.MaterialShadingModel.MSM_UNLIT, 'shading model is not Unlit')
        require(bool(material.get_editor_property('is_sky')), 'Is Sky is off')
        require(bool(material.get_editor_property('two_sided')), 'Two Sided is off')
        points = []
        for desc in SKY_POINT_DESCS:
            node = by_description(material, desc)
            require(node is not None, 'Custom node %s missing (or not unique)' % desc)
            require(norm(node.get_editor_property('code')) == norm(codes['point']), 'Custom node %s has other code' % desc)
            points.append(node)
        sky = by_description(material, 'FogMS_WeatherSky')
        require(sky is not None, 'Custom node FogMS_WeatherSky missing (or not unique)')
        require(norm(sky.get_editor_property('code')) == norm(codes['sky']), 'Custom node FogMS_WeatherSky has other code')
        P = {}
        for name, default in SKY_VECTORS + SKY_SCALARS:
            P[name] = find_param(material, name)
            require(P[name] is not None, 'parameter %s missing' % name)
        require(abs(mel.get_material_default_scalar_parameter_value(material, 'FogMS_SkyOn')) < 1e-6, 'FogMS_SkyOn default is not 0')
        for name, path in SKY_TEXTURES:
            t = find_param(material, name)
            require(isinstance(t, unreal.MaterialExpressionTextureObjectParameter), 'texture object %s missing' % name)
            tex = t.get_editor_property('texture')
            require(tex is not None and tex.get_path_name().split('.')[0] == path, 'texture object %s default is %s' % (name, tex.get_path_name() if tex else None))
            P[name] = t
        E = list(mel.get_material_expressions(material))

        def one(cls):
            found = [e for e in E if isinstance(e, cls)]
            require(len(found) == 1, 'expected one %s, found %d' % (cls.__name__, len(found)))
            return found[0]
        camvec, campos = one(unreal.MaterialExpressionCameraVectorWS), one(unreal.MaterialExpressionCameraPositionWS)
        sundir, disk = one(unreal.MaterialExpressionSkyAtmosphereLightDirection), one(unreal.MaterialExpressionSkyAtmosphereLightDiskLuminance)
        view, amb = one(unreal.MaterialExpressionSkyAtmosphereViewLuminance), one(unreal.MaterialExpressionSkyAtmosphereDistantLightScatteredLuminance)
        sw = one(unreal.MaterialExpressionReflectionCapturePassSwitch)
        for node in (sundir, disk):
            require(int(node.get_editor_property('light_index')) == 0, '%s light index is not 0' % node.get_name())
        consts = {}
        for e in E:
            if isinstance(e, unreal.MaterialExpressionConstant):
                v = float(e.get_editor_property('r'))
                require(v in (0.0, 1.0, 2.0) and v not in consts, 'unexpected constant %s = %g' % (e.get_name(), v))
                consts[v] = e
        require(sorted(consts) == [0.0, 1.0, 2.0], 'constants 0/1/2 missing: %s' % sorted(consts))
        verify_single_output_sources(material, sw, {'Default': consts[1.0], 'Reflection': consts[0.0]})
        aps = [e for e in E if isinstance(e, unreal.MaterialExpressionSkyAtmosphereAerialPerspective)]
        suns = [e for e in E if isinstance(e, unreal.MaterialExpressionSkyAtmosphereLightIlluminance)]
        require(len(aps) == 3 and len(suns) == 3, 'expected 3 aerial perspective + 3 light illuminance nodes, found %d + %d' % (len(aps), len(suns)))

        def fed_by(nodes, point):
            found = [n for n in nodes if dict(input_sources(material, n)).get(position_pin(n)) == point]
            require(len(found) == 1, '%d nodes read %s' % (len(found), point.get_name()))
            verify_single_output_sources(material, found[0], {position_pin(found[0]): point})
            return found[0]
        ap_of, sun_of = [], []
        for i, pt in enumerate(points):
            wanted = {'CamVec': (camvec, first_output(camvec)), 'CamWS': (campos, first_output(campos)), 'Which': (consts[float(i)], first_output(consts[float(i)]))}
            for pin in ('L0', 'L1', 'L2', 'March'):
                name, out = SKY_PARAM_PINS[pin]
                wanted[pin] = (P[name], out)
            verify_custom(material, pt, wanted)
            ap_of.append(fed_by(aps, pt))
            s = fed_by(suns, pt)
            require(int(s.get_editor_property('light_index')) == 0, '%s light index is not 0' % s.get_name())
            sun_of.append(s)
        sources = {'CamVec': camvec, 'CamWS': campos, 'SunDir': sundir, 'SunNear': sun_of[0], 'SunFar': sun_of[1], 'SunCirrus': sun_of[2],
                   'APNear': ap_of[0], 'APFar': ap_of[1], 'APCirrus': ap_of[2], 'Ambient': amb, 'SkyBg': view, 'SunDisk': disk, 'Quality': sw}
        wanted = {}
        for pin in SKY_PINS:
            if pin in sources:
                wanted[pin] = (sources[pin], first_output(sources[pin]))
            else:
                name, out = SKY_PARAM_PINS[pin]
                wanted[pin] = (P[name], out)
        verify_custom(material, sky, wanted)
        src = mel.get_material_property_input_node(material, unreal.MaterialProperty.MP_EMISSIVE_COLOR)
        require(src == sky, 'MP_EMISSIVE_COLOR is fed by %s, wanted FogMS_WeatherSky' % (src.get_name() if src else None))
        if g is not None:
            for node, links in g.links.items():
                verify_custom(material, node, links)
            for node, links in g.plain.items():
                verify_single_output_sources(material, node, links)
    except Exception as e:
        problems.append(str(e))
    return problems


def material_settings(material, blend):
    material.set_editor_property('blend_mode', blend)
    material.set_editor_property('shading_model', unreal.MaterialShadingModel.MSM_UNLIT)
    material.set_editor_property('material_domain', unreal.MaterialDomain.MD_SURFACE)
    material.set_editor_property('use_translucency_vertex_fog', False)


def verify_material(material, kind, codes, g=None):
    """[] when current. kind 'compose' | 'sun' | 'sky' (W49, verify_sky)."""
    if kind == 'sky':
        return verify_sky(material, codes, g)
    sun_code = codes['sun']
    problems = []
    try:
        blend = unreal.BlendMode.BLEND_ALPHA_COMPOSITE if kind == 'compose' else unreal.BlendMode.BLEND_OPAQUE
        require(material.get_editor_property('material_domain') == unreal.MaterialDomain.MD_SURFACE, 'domain is not Surface')
        require(material.get_editor_property('blend_mode') == blend, 'blend mode is not %s' % blend)
        require(material.get_editor_property('shading_model') == unreal.MaterialShadingModel.MSM_UNLIT, 'shading model is not Unlit')
        require(not material.get_editor_property('use_translucency_vertex_fog'), 'Apply Fogging is on')
        if kind == 'compose':
            codes = {'FogMS_WeatherCompose': COMPOSE_CODE, 'FogMS_WeatherComposeRGB': COMPOSE_RGB_CODE, 'FogMS_WeatherComposeOpacity': COMPOSE_OPACITY_CODE}
            vectors, textures = COMPOSE_VECTORS, (('FogMS_WeatherPattern', WEATHER_DIR + '/T_FogMS_WeatherPattern'),)
        else:
            codes = {'FogMS_WeatherSunOD': sun_code}
            vectors = SUN_VECTORS
            textures = (('FogMS_WeatherMap', BLACK_TEXTURE), ('FogMS_WeatherTypeLUT', WEATHER_DIR + '/T_FogMS_CloudTypeLUT'),
                        ('FogMS_WeatherPattern', WEATHER_DIR + '/T_FogMS_WeatherPattern'), ('FogMS_WeatherCurl', WEATHER_DIR + '/T_FogMS_Curl2D'))
        nodes = {}
        for desc, code in codes.items():
            node = by_description(material, desc)
            require(node is not None, 'Custom node %s missing (or not unique)' % desc)
            require(norm(node.get_editor_property('code')) == norm(code), 'Custom node %s has other code' % desc)
            nodes[desc] = node
        P = {}
        for name, default in vectors:
            P[name] = find_param(material, name)
            require(P[name] is not None, 'vector %s missing' % name)
        for name, path in textures:
            t = find_param(material, name)
            require(isinstance(t, unreal.MaterialExpressionTextureObjectParameter), 'texture object %s missing' % name)
            tex = t.get_editor_property('texture')
            require(tex is not None and tex.get_path_name().split('.')[0] == path, 'texture object %s default is %s' % (name, tex.get_path_name() if tex else None))
            P[name] = t
        uvs = [e for e in mel.get_material_expressions(material) if isinstance(e, unreal.MaterialExpressionTextureCoordinate)]
        require(len(uvs) == 1, 'expected one TextureCoordinate, found %d' % len(uvs))
        uv_outs = [str(o) for o in mel.get_material_expression_output_names(uvs[0])]
        uv_out = uv_outs[0] if uv_outs else ''
        if kind == 'compose':
            verify_custom(material, nodes['FogMS_WeatherCompose'], {'UV': (uvs[0], uv_out), 'Pattern': (P['FogMS_WeatherPattern'], ''),
                                                                    'P0': (P['FogMS_WeatherComposeP0'], 'RGBA'), 'P1': (P['FogMS_WeatherComposeP1'], 'RGBA')})
            verify_custom(material, nodes['FogMS_WeatherComposeRGB'], {'Value': (nodes['FogMS_WeatherCompose'], '')})
            verify_custom(material, nodes['FogMS_WeatherComposeOpacity'], {'Value': (nodes['FogMS_WeatherCompose'], '')})
            for prop, desc in (('MP_EMISSIVE_COLOR', 'FogMS_WeatherComposeRGB'), ('MP_OPACITY', 'FogMS_WeatherComposeOpacity')):
                src = mel.get_material_property_input_node(material, getattr(unreal.MaterialProperty, prop))
                require(src == nodes[desc], '%s is fed by %s, wanted %s' % (prop, src.get_name() if src else None, desc))
        else:
            wanted = {'UV': (uvs[0], uv_out), 'Domain': (P['FogMS_WeatherDomain'], 'RGBA'), 'L0': (P['FogMS_WeatherL0'], 'RGBA'),
                      'L1': (P['FogMS_WeatherL1'], 'RGBA'), 'SunDir': (P['FogMS_WeatherSunDir'], 'RGBA'), 'Map': (P['FogMS_WeatherMap'], ''),
                      'TypeLUT': (P['FogMS_WeatherTypeLUT'], ''), 'Pattern': (P['FogMS_WeatherPattern'], ''), 'Curl': (P['FogMS_WeatherCurl'], '')}
            verify_custom(material, nodes['FogMS_WeatherSunOD'], wanted)
            src = mel.get_material_property_input_node(material, unreal.MaterialProperty.MP_EMISSIVE_COLOR)
            require(src == nodes['FogMS_WeatherSunOD'], 'MP_EMISSIVE_COLOR is fed by %s' % (src.get_name() if src else None))
        if g is not None:
            for node, links in g.links.items():
                verify_custom(material, node, links)
    except Exception as e:
        problems.append(str(e))
    return problems


def ensure_material(path, name, kind, codes, report_only):
    """Returns (status, detail): 'current' | 'built' | 'report' | 'failed'."""
    material = unreal.load_asset(path) if unreal.EditorAssetLibrary.does_asset_exist(path) else None
    problems = verify_material(material, kind, codes) if material is not None else ['missing']
    if report_only or not problems:
        return ('current' if not problems else 'report'), problems
    if material is None:
        material = unreal.AssetToolsHelpers.get_asset_tools().create_asset(name, WEATHER_DIR, unreal.Material, unreal.MaterialFactoryNew())
        require(material is not None, 'Cannot create ' + path)
        print('CREATED', path)
    else:
        print('REBUILD %s: %s' % (path, problems))
        clear_expressions(material)
    try:
        if kind == 'sky':
            sky_settings(material)
            g = build_sky(material, codes)
        else:
            material_settings(material, unreal.BlendMode.BLEND_ALPHA_COMPOSITE if kind == 'compose' else unreal.BlendMode.BLEND_OPAQUE)
            g = build_compose(material) if kind == 'compose' else build_sun(material, codes['sun'])
        errors, note = compile_and_check(material, 'FOGMS_MATEDIT_WEATHER_%s_%d' % (kind.upper(), int(time.time() * 1000)))
        print('COMPILE %s: %s' % (name, note))
        require(not errors, 'Compile errors: %s' % errors)
        after = verify_material(material, kind, codes, g)
        require(not after, 'Readback after build: %s' % after)
        after = verify_material(material, kind, codes)
        require(not after, 'Name-based readback after build: %s' % after)
    except Exception:
        print('TRACE', traceback.format_exc())
        rollback(material, path)
        return 'failed', ['see TRACE']
    saved = unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False)
    return 'built', ['saved=%s' % saved]


# ------------------------------------------------------------------ 4. presets
def ensure_presets(report_only):
    out = []
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    for name, preset in PRESETS:
        path = WEATHER_DIR + '/' + name
        want = getattr(unreal.FogMSWeatherPreset, preset)
        if unreal.EditorAssetLibrary.does_asset_exist(path):
            asset = unreal.load_asset(path)
            require(isinstance(asset, unreal.FogMSWeatherState), '%s is not a FogMS Weather State' % path)
            got = asset.get_editor_property('preset')
            # W49: a preset asset saved by W48 has no cirrus fields; UFogMSWeatherState::PostLoad gives it the preset's (nothing saved).
            values = asset.get_editor_property('values')
            out.append('%s %s (cirrus %.2f at %.1f km)' % (name, 'present' if got == want else 'LEFT AS IS (preset %s, edited by hand?)' % got,
                                                          values.get_editor_property('cirrus_coverage'), values.get_editor_property('cirrus_altitude_km')))
            continue
        if report_only:
            out.append('%s missing' % name)
            continue
        factory = unreal.DataAssetFactory()
        factory.set_editor_property('data_asset_class', unreal.FogMSWeatherState)
        asset = tools.create_asset(name, WEATHER_DIR, unreal.FogMSWeatherState, factory)
        require(asset is not None, 'Cannot create ' + path)
        asset.apply_preset(want)
        require(asset.get_editor_property('preset') == want, 'apply_preset readback failed on ' + name)
        saved = unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False)
        values = asset.get_editor_property('values')
        out.append('%s created saved=%s coverage=%.2f type=%.2f base=%.2f top=%.2f deck=%.2f' % (
            name, saved, values.get_editor_property('coverage'), values.get_editor_property('cloud_type'),
            values.get_editor_property('base_km'), values.get_editor_property('top_km'), values.get_editor_property('deck_coverage')))
    return out


def main():
    report_only = os.environ.get('FOGMS_MATEDIT_MODE', '') == 'report'
    weather_fn = cloud_constant('WEATHER_FN_CODE')
    codes = {'sun': weather_fn + SUN_OD_BODY, 'sky': weather_fn + SKY_GEO_CODE + SKY_BODY, 'point': SKY_GEO_CODE + SKY_POINT_BODY}
    lines = []
    changed = False
    # 1. textures
    for name, png_name, address, mips in TEXTURES:
        png = os.path.join(TEXGEN_DIR, png_name)
        require(os.path.isfile(png), 'Missing %s: run `python texgen/gen_weather_textures.py --out %s` first' % (png, TEXGEN_DIR))
        digest = sha256(png)
        note = '' if TEXGEN_SHA256.get(png_name) == digest else ' NOTE: not the seed-11 default output (%s)' % digest[:12]
        problems = texture_problems(name, png, address, mips)
        if not problems:
            lines.append('%s current%s' % (name, note))
            continue
        if report_only:
            lines.append('%s: %s%s' % (name, problems, note))
            continue
        import_texture(name, png, address, mips)
        after = texture_problems(name, png, address, mips)
        require(not after, '%s after import: %s' % (name, after))
        saved = unreal.EditorAssetLibrary.save_asset(WEATHER_DIR + '/' + name, only_if_is_dirty=False)
        lines.append('%s imported from %s (was: %s) saved=%s%s' % (name, png, problems, saved, note))
        changed = True
    # 2./3./5. materials (W49: the sky dome material after the two W48 ones)
    for path, name, kind in ((COMPOSE_PATH, COMPOSE_NAME, 'compose'), (SUN_PATH, SUN_NAME, 'sun'), (SKY_PATH, SKY_NAME, 'sky')):
        status, detail = ensure_material(path, name, kind, codes, report_only)
        lines.append('%s %s %s' % (name, status, detail))
        require(status != 'failed', '%s failed (rolled back, nothing saved)' % name)
        changed = changed or status == 'built'
    # 4. presets
    preset_lines = ensure_presets(report_only)
    lines += preset_lines
    changed = changed or any(' created ' in l for l in preset_lines)
    for line in lines:
        print(line)
    if report_only:
        print('REPORT done')
    else:
        print(('WEATHER_OK' if changed else 'ALREADY_PATCHED') + ' %s (%s, %s)' % (WEATHER_DIR, MARKER, SKY_MARKER))


try:
    main()
except Exception:
    print('TRACE', traceback.format_exc())
