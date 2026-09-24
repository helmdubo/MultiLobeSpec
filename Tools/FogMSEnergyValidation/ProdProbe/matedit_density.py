"""FogMS S1 (edge erosion) + S2 (height profile): patch /MultiLobeSpec/FogMS/M_FogMS_Density in place.

Run in the editor (Python console or `py <this file>`), with no other session editing the material:
  1. creates the new scalar/vector parameters (defaults = identity: erosion off, profile off);
  2. creates a NEW Custom node, description 'FogMS_Extinction', code EXTINCTION_CODE_V2, whose inputs are the
     inputs of the old extinction node MaterialExpressionCustom_3 (same source expressions and output pins,
     found by walking Custom_3's inputs) plus the new parameters;
  3. re-routes every consumer of Custom_3's output (material properties such as MP_SUBSURFACE_COLOR = extinction,
     and expression pins such as the injection node's 'Extinction') to the new node;
  4. leaves Custom_3 in the graph with its inputs but without any consumer (restore = re-route back);
  5. recompiles; on compile errors it re-routes back, deletes what it created and does not save.
Idempotent: a Custom node with description 'FogMS_Extinction' means steps 1-5 are already done (skipped).

Then (field contract v4, W34) the existing Custom node 'FogMS_InjectionAlbedo' (BaseColor) gets FogMS_InjectionMode 3,
the Box debug view 'Field Only (Debug)': its code is replaced INJECTION_ALBEDO_CODE_V3 -> INJECTION_ALBEDO_CODE_V4 (same
inputs Albedo/Mode/FieldA, same wiring). Mode 3 = BaseColor 0 always (native single scattering off even without a valid
field), extinction unchanged; Emissive is the unchanged 'FogMS_EmissiveInjection' node, which already emits
valid * Field * Albedo * sigma_t for every Mode > 0.5 (the Box publishes the full field J in mode 3). Modes 0/1/2 evaluate
exactly as in v3 (the v4 factor is 1.0 there). Mode 3 does not change the extinction formula.
Idempotent: code already carrying 'field contract v4' is left alone; code that is neither v3 nor v4 aborts without changes.

Then (W36 depth prefilter) the extinction goes v2 -> v3, the same way as v1 -> v2 (patch_extinction_v3):
  1. creates FogMS_DepthPrefilter (scalar, default 0 = v2 math) and FogMS_PrefilterWavelengths (vector, default 0) if missing,
     a PixelDepth node and a Custom node 'FogMS_DepthFootprint' (FOOTPRINT_CODE_V1, float2: prefilter width w [cm], mip bias);
  2. creates a NEW Custom node, description 'FogMS_Extinction_v3', code EXTINCTION_CODE_V3, whose inputs are the v2 node's
     (same source expressions and output pins) plus Footprint <- FogMS_DepthFootprint and Wavelengths <- the new vector;
  3. the base noise sample (the texture sample feeding the v2 node's 'Noise' pin) gets Mip Value Mode 'Mip Bias' fed by a
     ComponentMask (G) of the footprint node, but only if it is a texture sample in mode None that feeds neither Detail pin;
     otherwise this step is skipped with a message (the prefilter then works without the base mip: (a) and (c) only);
  4. re-routes every consumer of the v2 node (MP_SUBSURFACE_COLOR, the injection node's 'Extinction') to the v3 node and
     leaves the v2 node in the graph without a consumer; recompiles. On a compile error (or any failure) it re-routes back to
     v2, restores the base sample's mip mode, deletes what it created, recompiles and does not save.
  Idempotent: a Custom node 'FogMS_Extinction_v3' that feeds MP_SUBSURFACE_COLOR means the step is done (skipped). A v2 node
  whose code is not EXTINCTION_CODE_V2 aborts without changes. With FogMS_DepthPrefilter 0 the v3 node is v2 exactly, and the
  mip bias is 0 (Sample with bias 0 == Sample): the material's own defaults render as before; the Box MID sets its Depth Prefilter.
Then (W37 F1a forward lobe, patch_forward_lobe) a Custom node 'FogMS_ForwardLobe' (FORWARD_LOBE_CODE_V1, float3) goes between
the unchanged 'FogMS_EmissiveInjection' node and MP_EMISSIVE_COLOR:
  1. creates FogMS_ForwardStrength (default 0 = off), FogMS_ForwardG (0.6), FogMS_ForwardDepth (0.5), FogMS_ForwardFloor
     (0.25) if missing, and a CameraVectorWS node;
  2. the new node's inputs: Emissive <- FogMS_EmissiveInjection, FieldA / Mode <- the same sources and output pins as the
     injection node's FieldA / Mode pins, Strength / G / Depth / BackFloor <- the four scalars, CameraVector <- CameraVectorWS;
  3. re-routes MP_EMISSIVE_COLOR to it, recompiles and waits for the shader result (compile_and_check). On a compile error
     (or any failure) MP_EMISSIVE_COLOR goes back to the injection node, what this step created is deleted, the material is
     recompiled and nothing is saved.
  Idempotent: a Custom node 'FogMS_ForwardLobe' carrying 'FogMS_ForwardLobe v1' that feeds MP_EMISSIVE_COLOR means done
  (skipped). With FogMS_ForwardStrength 0 (material default; the Box MID sets Forward Scattering, default 0) or an injection
  mode other than 2 the node returns its Emissive input unchanged (uniform branch): modes 0/1/3 and the default look are as
  before. The math (mean over view directions 1, minimum >= 1 - Strength * (1 - Floor)) is checked on the CPU by
  fwd_lobe_check.py, which evaluates FORWARD_LOBE_CODE_V1 itself (read from this file as text).
Default readback: only parameters created by this run must read back their script default; a pre-existing parameter keeps
its material default (for example FogMS_ErosionDepth 0.3 from round 32) and is reported, since the Box MID sets it anyway.
The material is saved once, only if a step changed it; on any error nothing is saved (reload the asset to discard memory state).

EXTINCTION_CODE_V3 must stay formula-identical to FogMS_IndirectLocalDensity in Shaders/Private/FogMS_Indirect.ush at
Footprint.x = 0 (the solver always evaluates w = 0); the .ush carries a verbatim copy between 'BEGIN EXTINCTION_CODE_V3' /
'END EXTINCTION_CODE_V3'. When the .ush is found next to this script (repo layout), a mismatch aborts before any change.
EXTINCTION_CODE_V2 is kept only to build v2 on a v1 material and to identify the v2 node.
Default identity: HeightProfile 0 gives P = 1 (n * 1 == n) and ErosionStrength 0 skips the erosion block, so the
defaults evaluate exactly the V1 formula.
"""
import glob
import os
import shlex
import time
import traceback

import unreal

MATERIAL_PATH = '/MultiLobeSpec/FogMS/M_FogMS_Density'
OLD_NODE_NAME = 'MaterialExpressionCustom_3'
NEW_DESCRIPTION = 'FogMS_Extinction'
OLD_INPUTS = ('Noise', 'Detail0', 'Detail1', 'ChannelMask', 'DetailStrength', 'SecondOctave',
              'LocalPosition', 'WorldExtent', 'Threshold', 'Softness', 'Density', 'Feather')
# (parameter name, default, Custom input pin). Names match the MID setters in FogMS_BoxVolume.cpp.
NEW_SCALARS = (('FogMS_ErosionStrength', 0.0, 'ErosionStrength'),
               ('FogMS_ErosionDepth', 0.15, 'ErosionDepth'),
               ('FogMS_HeightProfile', 0.0, 'HeightProfile'),
               ('FogMS_HeightBottom', 0.0, 'HeightBottom'),
               ('FogMS_HeightTop', 1.0, 'HeightTop'),
               ('FogMS_HeightBottomSoftness', 0.05, 'BottomSoftness'),
               ('FogMS_HeightTopSoftness', 0.1, 'TopSoftness'),
               ('FogMS_HeightAnvilStrength', 0.0, 'AnvilStrength'))
# One-hot RGBA; default G (the Box default Erosion Channel).
NEW_VECTORS = (('FogMS_ErosionMask', (0.0, 1.0, 0.0, 0.0), 'ErosionMask'),)
# Visible inputs of a Volume/Additive/DefaultLit material that can consume the extinction node.
PROPERTIES = ('MP_SUBSURFACE_COLOR', 'MP_BASE_COLOR', 'MP_EMISSIVE_COLOR', 'MP_OPACITY', 'MP_AMBIENT_OCCLUSION')

# V1 (upgrade_fog_material.py EXTINCTION_CODE), only used to identify the old node if it was renamed.
EXTINCTION_CODE_V1_TAIL = 'return max(Density, 0.0f) * mask * fade;'

EXTINCTION_CODE_V2 = r"""// FogMS_Extinction v2 (S1 edge erosion, S2 height profile); formula-identical to FogMS_IndirectLocalDensity.
// LocalPosition is Box-local cube space -50..50: LocalPosition * 0.01f + 0.5f == Local / Extent * UVSign * 0.5f + 0.5f.
struct FogMSDensityFn
{
    float SS(float a, float w, float x) { return smoothstep(a, a + max(w, 1e-4f), x); }
};
FogMSDensityFn Fn;
float P = 1.0f;
if (HeightProfile > 0.5f)
{
    float h = LocalPosition.z * 0.01f + 0.5f;
    float M = HeightBottom + 0.5f * (HeightTop - HeightBottom);
    P = Fn.SS(HeightBottom, BottomSoftness, h) * (1.0f - Fn.SS(HeightTop - TopSoftness, TopSoftness, h))
        * (1.0f + AnvilStrength * Fn.SS(M, max(HeightTop - TopSoftness - M, 1e-4f), h));
}
float n = dot(Noise, ChannelMask) * P;
if (DetailStrength > 0.0f)
{
    float d0 = dot(Detail0, ChannelMask);
    float d1 = dot(Detail1, ChannelMask);
    float detail = ((2.0f * d0 - 1.0f) + 0.5f * SecondOctave * (2.0f * d1 - 1.0f))
        / (1.0f + 0.5f * SecondOctave);
    n += DetailStrength * detail;
}
if (ErosionStrength > 0.0f)
{
    float EdgeW = 1.0f - saturate((n - (Threshold - Softness * 0.5f)) / ErosionDepth);
    n -= ErosionStrength * EdgeW * dot(Detail1, ErosionMask);
}
float mask = Softness > 0.0f ? smoothstep(Threshold - Softness * 0.5f, Threshold + Softness * 0.5f, n) : step(Threshold, n);
float3 edge = (1.0f - abs(LocalPosition) * 0.02f) * WorldExtent;
float inside = min(edge.x, min(edge.y, edge.z));
float width = min(Feather, min(WorldExtent.x, min(WorldExtent.y, WorldExtent.z)));
float fade = width > 0.0f ? smoothstep(0.0f, width, inside) : step(0.0f, inside);
return max(Density, 0.0f) * mask * fade;
"""

# W36 depth prefilter: FogMS_Extinction v3 + FogMS_DepthFootprint (patch_extinction_v3).
V3_DESCRIPTION = 'FogMS_Extinction_v3'
V3_MARKER = 'FogMS_Extinction v3'
FOOTPRINT_DESCRIPTION = 'FogMS_DepthFootprint'
# Inputs of the v2 node (carried over to v3 with the same sources).
V2_INPUTS = OLD_INPUTS + tuple(pin for _, _, pin in NEW_SCALARS) + tuple(pin for _, _, pin in NEW_VECTORS)
# (parameter name, default, pin). Material defaults 0 = v2 math; the Box MID sets them (FogMS_BoxVolume.cpp UpdateDensity:
# Depth Prefilter, default 0 since W37 (1 in W36); wavelengths = world feature size [cm] of base / detail 0 / detail 1 noise
# = tile period / 4).
V3_SCALARS = (('FogMS_DepthPrefilter', 0.0, 'DepthPrefilter'),)
V3_VECTORS = (('FogMS_PrefilterWavelengths', (0.0, 0.0, 0.0, 0.0), 'Wavelengths'),)
FOOTPRINT_INPUTS = ('PixelDepth', 'DepthPrefilter')

FOOTPRINT_CODE_V1 = r"""// FogMS_DepthFootprint v1 (W36). Froxel footprint of the native volumetric-fog voxelization at this sample.
// Returns float2(w, MipBias). w = DepthPrefilter * max(dz, dxy) [cm] is the prefilter width of FogMS_Extinction v3;
// MipBias = max(0, log2(w / dxy)) goes to the base noise sample, whose hardware LOD already covers dxy.
// dz = depth-slice thickness at view depth d. Slices (VolumetricFog.cpp GetVolumetricFogGridZParams, CalculateGridZParams):
//   Z(d) = S * log2(d * B + O), (B, O, S) = View.VolumetricFogGridZParams, so dz = dd/dZ = ln2 * (d * B + O) / (S * B).
// dxy = froxel width at d: the voxelization pass sets View.ViewSizeAndInvSize to the fog grid (VoxelizeFogVolumePrimitives,
//   one pixel = one froxel), so dxy = 2 * d / (ViewToClip[0][0] * GridSizeX).
// d = PixelDepth = SvPosition.w of the jittered slice. DepthPrefilter 0 (material default), volumetric fog off
// (GridZParams 0) or an orthographic view: float2(0, 0), the previous material exactly.
float Depth = max(PixelDepth, 0.0f);
float3 GridZ = View.VolumetricFogGridZParams;
bool Valid = DepthPrefilter > 0.0f && GridZ.x > 0.0f && GridZ.z > 0.0f && View.ViewToClip[3][3] < 1.0f;
float SliceDz = Valid ? 0.693147f * (Depth * GridZ.x + GridZ.y) / (GridZ.z * GridZ.x) : 0.0f;
float FroxelDxy = Valid ? 2.0f * Depth * View.ViewSizeAndInvSize.z / max(View.ViewToClip[0][0], 1e-6f) : 0.0f;
float FootW = Valid ? DepthPrefilter * max(SliceDz, FroxelDxy) : 0.0f;
float MipBias = FroxelDxy > 0.0f ? clamp(log2(max(FootW, FroxelDxy) / FroxelDxy), 0.0f, 12.0f) : 0.0f;
return float2(FootW, MipBias);
"""

EXTINCTION_CODE_V3 = r"""// FogMS_Extinction v3 (W36 depth prefilter; S1 edge erosion, S2 height profile). With Footprint.x = 0 it is v2, formula-
// identical to FogMS_IndirectLocalDensity (the solver samples 32^3 cells, never froxels: it always evaluates w = 0).
// LocalPosition is Box-local cube space -50..50: LocalPosition * 0.01f + 0.5f == Local / Extent * UVSign * 0.5f + 0.5f.
// Prefilter (Nubis-style, width w = Footprint.x [cm] from FogMS_DepthFootprint; Wavelengths.xyz = world feature size
// lambda of the base / detail 0 / detail 1 noise [cm], MID FogMS_PrefilterWavelengths, 0 = unknown: band unfiltered):
//   r_i = saturate(w / lambda_i): removed fraction of band i's std (box filter of width w; bundled Perlin-Worley R, 64^3:
//         0.21 / 0.43 / 0.76 measured at w = lambda/4, lambda/2, lambda).
//   (a) detail octaves: amplitude * (1 - r_i), the main lobe of the box-filter response sinc(w / lambda_i); the erosion
//       pattern e -> e * (1 - r_2) + 0.5 * r_2 (fades to its mean).
//   (b) base noise: its texture sample gets MipBias = log2(w / froxel width) (FogMS_DepthFootprint.y), removing ~r_0.
//   (c) threshold band: the removed parts act as zero-mean noise of variance
//       dn^2 = (0.2 P r_0)^2 + (0.4 A_0 r_1)^2 + (0.4 A_1 r_2)^2 + (0.2 E EdgeW r_2)^2,
//       sigma = 0.2 per texture channel (bundled Perlin-Worley, every channel), 0.4 for (2d - 1); A_0, A_1 = detail octave
//       amplitudes, E = ErosionStrength. The expected mask is the smoothstep band convolved with that noise; the smoothstep
//       derivative has variance Softness^2 / 20 and variances add: S_eff = sqrt(Softness^2 + 20 dn^2), k = sqrt(20).
//   w = 0: r_i = 0, dn^2 = 0, S_eff = Softness, every factor an exact identity (x * 1, + 0).
struct FogMSDensityFn
{
    float SS(float a, float w, float x) { return smoothstep(a, a + max(w, 1e-4f), x); }
};
FogMSDensityFn Fn;
float FootW = max(Footprint.x, 0.0f);
float R0 = Wavelengths.x > 0.0f ? saturate(FootW / Wavelengths.x) : 0.0f;
float R1 = Wavelengths.y > 0.0f ? saturate(FootW / Wavelengths.y) : 0.0f;
float R2 = Wavelengths.z > 0.0f ? saturate(FootW / Wavelengths.z) : 0.0f;
float P = 1.0f;
if (HeightProfile > 0.5f)
{
    float h = LocalPosition.z * 0.01f + 0.5f;
    float M = HeightBottom + 0.5f * (HeightTop - HeightBottom);
    P = Fn.SS(HeightBottom, BottomSoftness, h) * (1.0f - Fn.SS(HeightTop - TopSoftness, TopSoftness, h))
        * (1.0f + AnvilStrength * Fn.SS(M, max(HeightTop - TopSoftness - M, 1e-4f), h));
}
float n = dot(Noise, ChannelMask) * P;
float Dn2 = (0.2f * P * R0) * (0.2f * P * R0);
if (DetailStrength > 0.0f)
{
    float d0 = dot(Detail0, ChannelMask);
    float d1 = dot(Detail1, ChannelMask);
    float detail = ((2.0f * d0 - 1.0f) * (1.0f - R1) + 0.5f * SecondOctave * (2.0f * d1 - 1.0f) * (1.0f - R2))
        / (1.0f + 0.5f * SecondOctave);
    n += DetailStrength * detail;
    float A0 = 0.4f * DetailStrength / (1.0f + 0.5f * SecondOctave);
    float A1 = A0 * 0.5f * SecondOctave;
    Dn2 += (A0 * R1) * (A0 * R1) + (A1 * R2) * (A1 * R2);
}
if (ErosionStrength > 0.0f)
{
    float EdgeW = 1.0f - saturate((n - (Threshold - Softness * 0.5f)) / ErosionDepth);
    n -= ErosionStrength * EdgeW * (dot(Detail1, ErosionMask) * (1.0f - R2) + 0.5f * R2);
    Dn2 += (0.2f * ErosionStrength * EdgeW * R2) * (0.2f * ErosionStrength * EdgeW * R2);
}
float SEff = Dn2 > 0.0f ? sqrt(Softness * Softness + 20.0f * Dn2) : Softness;
float mask = SEff > 0.0f ? smoothstep(Threshold - SEff * 0.5f, Threshold + SEff * 0.5f, n) : step(Threshold, n);
float3 edge = (1.0f - abs(LocalPosition) * 0.02f) * WorldExtent;
float inside = min(edge.x, min(edge.y, edge.z));
float width = min(Feather, min(WorldExtent.x, min(WorldExtent.y, WorldExtent.z)));
float fade = width > 0.0f ? smoothstep(0.0f, width, inside) : step(0.0f, inside);
return max(Density, 0.0f) * mask * fade;
"""

# BaseColor node of the emissive-injection graph (FogMS_BoxVolume.cpp, MID FogMS_InjectionMode). V3 is the code saved in
# the repo's M_FogMS_Density.uasset (field contract v3, commit 59ac907); V4 adds mode 3 (debug field only).
INJECTION_ALBEDO_DESCRIPTION = 'FogMS_InjectionAlbedo'
INJECTION_ALBEDO_INPUTS = ('Albedo', 'Mode', 'FieldA')
INJECTION_ALBEDO_CODE_V3 = (
    "// Injection albedo (field contract v3). Mode 1: albedo 0 so the native path adds no single scattering (extinction still occludes).\n"
    "// Mode 2 (hybrid): native single scattering stays on, scaled by the per-cell sun transmittance T = 2*FieldA - 1 so the cloud self-shadows.\n"
    "float V = (Mode > 0.5f && FieldA >= 0.5f) ? 1.0f : 0.0f;\n"
    "float T = saturate(2.0f * FieldA - 1.0f);\n"
    "float S = (Mode > 1.5f) ? T : 0.0f;\n"
    "return Albedo * lerp(1.0f, S, V);")
INJECTION_ALBEDO_CODE_V4 = (
    "// Injection albedo (field contract v4). Mode 1: albedo 0 so the native path adds no single scattering (extinction still occludes).\n"
    "// Mode 2 (hybrid): native single scattering stays on, scaled by the per-cell sun transmittance T = 2*FieldA - 1 so the cloud self-shadows.\n"
    "// Mode 3 (debug field only): albedo 0 always, also without a valid field: only the injected full field J lights this Box.\n"
    "float V = (Mode > 0.5f && FieldA >= 0.5f) ? 1.0f : 0.0f;\n"
    "float T = saturate(2.0f * FieldA - 1.0f);\n"
    "float S = (Mode > 1.5f && Mode < 2.5f) ? T : 0.0f;\n"
    "float Native = (Mode > 2.5f) ? 0.0f : 1.0f;\n"
    "return Albedo * lerp(1.0f, S, V) * Native;")
INJECTION_ALBEDO_V4_MARKER = 'field contract v4'
# The Emissive node must emit for every mode > 0.5 with a valid field (mode 3 included); checked, never modified.
EMISSIVE_DESCRIPTION = 'FogMS_EmissiveInjection'
EMISSIVE_REQUIRED = 'float V = (Mode > 0.5f && FieldA >= 0.5f) ? 1.0f : 0.0f;'

# W37 F1a forward lobe (patch_forward_lobe). (parameter name, default, pin); names match the MID setters in
# FogMS_BoxVolume.cpp (Forward Scattering / Forward Anisotropy / Forward Depth / Back Floor).
FORWARD_DESCRIPTION = 'FogMS_ForwardLobe'
FORWARD_MARKER = 'FogMS_ForwardLobe v1'
FORWARD_SCALARS = (('FogMS_ForwardStrength', 0.0, 'Strength'),
                   ('FogMS_ForwardG', 0.6, 'G'),
                   ('FogMS_ForwardDepth', 0.5, 'Depth'),
                   ('FogMS_ForwardFloor', 0.25, 'BackFloor'))
FORWARD_INPUTS = ('Emissive', 'FieldA', 'Mode') + tuple(pin for _, _, pin in FORWARD_SCALARS) + ('CameraVector',)
# One statement per line (fwd_lobe_check.py translates the lines between the braces to numpy and integrates them).
FORWARD_LOBE_CODE_V1 = r"""// FogMS_ForwardLobe v1 (W37 F1a). Forward lobe of the multiply scattered light, hybrid mode 2 only. Emissive = the output of
// FogMS_EmissiveInjection (sigma_s * J_ms, J_ms = J minus the uncollided sun term: isotropic); returns Emissive * Lobe(mu):
//   Lobe = 1 + (1 - BackFloor) * (f1 * (p(g1) - 1) + f2 * (p(g2) - 1)),  mu = dot(L, -CameraVector) (1 = looking at the sun)
//   p(g) = (1 - g^2) / (1 + g^2 - 2 g mu)^1.5 = 4 pi * Henyey-Greenstein, whose mean over the sphere is exactly 1
//   f1 = Strength * 2/3 * S^Depth, f2 = Strength * 1/3 * S^(Depth^2), g1 = G, g2 = G / 2: two octaves (Wrenninge 2013, a = c = 1/2;
//   octave i sees the sun transmittance T^(b^i), b = Depth); S = saturate(2 FieldA - 1) = T_sun * k, the hybrid field alpha.
// Mean over view directions = 1 exactly, the field's energy is unchanged (each octave phase is BackFloor * 1 + (1 - BackFloor) * p,
// mean 1). Minimum >= 1 - Strength * (1 - BackFloor) (p > 0, f1 + f2 <= Strength). No sun or night (k = 0) or an invalid field:
// S = 0 and Lobe = 1 exactly (explicit select: max(S, eps)^b would leave eps^0.05 = 0.5); deep shadow: S -> 0, Lobe -> 1.
// L = View.AtmosphereLightDirection[0], the solver's sun (FScene::AtmosphereLights[0]); the volumetric-fog voxelization pass copies
// the cached View uniform buffer (VolumetricFogVoxelization.cpp), so it is valid there. CPU proof: ProdProbe/fwd_lobe_check.py.
// Strength 0 (material default) or FogMS_InjectionMode != 2: returns Emissive unchanged (uniform branch, the lobe ALU is skipped).
float3 Result = Emissive;
BRANCH
if (Strength > 0.0f && Mode > 1.5f && Mode < 2.5f)
{
    float s = saturate(Strength);
    float g1 = clamp(G, 0.0f, 0.9f);
    float g2 = 0.5f * g1;
    float b = clamp(Depth, 0.05f, 1.0f);
    float fl = saturate(BackFloor);
    float S = saturate(2.0f * FieldA - 1.0f);
    float3 L = normalize(View.AtmosphereLightDirection[0].xyz);
    float mu = clamp(dot(L, -CameraVector), -1.0f, 1.0f);
    float f1 = S > 0.0f ? s * (2.0f / 3.0f) * pow(S, b) : 0.0f;
    float f2 = S > 0.0f ? s * (1.0f / 3.0f) * pow(S, b * b) : 0.0f;
    float h1 = 1.0f + g1 * g1 - 2.0f * g1 * mu;
    float h2 = 1.0f + g2 * g2 - 2.0f * g2 * mu;
    float p1 = (1.0f - g1 * g1) * rsqrt(h1) / h1;
    float p2 = (1.0f - g2 * g2) * rsqrt(h2) / h2;
    Result = Emissive * (1.0f + (1.0f - fl) * (f1 * (p1 - 1.0f) + f2 * (p2 - 1.0f)));
}
return Result;
"""

mel = unreal.MaterialEditingLibrary


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def ush_sync_check():
    """Compare EXTINCTION_CODE_V3 with the verbatim copy in the .ush (repo layout only)."""
    here = globals().get('__file__')
    if not here:
        return 'SKIPPED (no __file__)'
    path = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(here)), '..', '..', '..',
                                         'Shaders', 'Private', 'FogMS_Indirect.ush'))
    if not os.path.isfile(path):
        return 'SKIPPED (%s not found)' % path
    with open(path, 'r', encoding='utf-8') as f:
        text = f.read().replace('\r\n', '\n')
    begin, end = 'BEGIN EXTINCTION_CODE_V3\n', '\nEND EXTINCTION_CODE_V3'
    require(begin in text and end in text, 'The .ush has no EXTINCTION_CODE_V3 markers: ' + path)
    copy = text.split(begin, 1)[1].split(end, 1)[0]
    require(copy.strip('\n') == EXTINCTION_CODE_V3.strip('\n'),
            'EXTINCTION_CODE_V3 differs from the copy in ' + path + ': update both together.')
    return 'OK (' + path + ')'


def expressions(material):
    return list(mel.get_material_expressions(material))


def find_parameter(exprs, name):
    for e in exprs:
        try:
            if str(e.get_editor_property('parameter_name')) == name:
                return e
        except Exception:
            pass
    return None


def custom_nodes(exprs):
    return [e for e in exprs if isinstance(e, unreal.MaterialExpressionCustom)]


def find_new_node(exprs):
    found = [e for e in custom_nodes(exprs) if str(e.get_editor_property('description')) == NEW_DESCRIPTION]
    require(len(found) <= 1, 'Several Custom nodes are named %s' % NEW_DESCRIPTION)
    return found[0] if found else None


def find_v3_node(exprs):
    found = [e for e in custom_nodes(exprs) if str(e.get_editor_property('description')) == V3_DESCRIPTION]
    require(len(found) <= 1, 'Several Custom nodes are named %s' % V3_DESCRIPTION)
    return found[0] if found else None


def find_old_node(exprs):
    by_name = [e for e in exprs if e.get_name() == OLD_NODE_NAME]
    candidates = by_name or [e for e in custom_nodes(exprs)
                             if EXTINCTION_CODE_V1_TAIL in str(e.get_editor_property('code'))
                             and str(e.get_editor_property('description')) not in (NEW_DESCRIPTION, V3_DESCRIPTION)]
    require(len(candidates) == 1, 'Cannot identify the V1 extinction node (%s): %s' % (
        OLD_NODE_NAME, [e.get_name() for e in candidates]))
    old = candidates[0]
    require(isinstance(old, unreal.MaterialExpressionCustom), OLD_NODE_NAME + ' is not a Custom expression')
    require(EXTINCTION_CODE_V1_TAIL in str(old.get_editor_property('code')),
            OLD_NODE_NAME + ' does not end with the V1 extinction formula')
    names = list(mel.get_material_expression_input_names(old))
    require(sorted(names) == sorted(OLD_INPUTS), 'Unexpected V1 inputs: %s' % names)
    return old


def input_links(material, node):
    """[(pin, source expression or None, source output name)] in pin order."""
    names = list(mel.get_material_expression_input_names(node))
    sources = list(mel.get_inputs_for_material_expression(material, node))
    require(len(names) == len(sources), 'Input name/source count mismatch on ' + node.get_name())
    links = []
    for pin, src in zip(names, sources):
        out = None
        if src is not None:
            out = mel.get_input_node_output_name_for_material_expression(node, src)
            out = '' if out is None else str(out)
        links.append((pin, src, out))
    return links


def consumers_of(material, exprs, target):
    """Expression pins and material properties fed by `target`."""
    pins, props = [], []
    for e in exprs:
        if e == target:
            continue
        for pin, src, _ in input_links(material, e):
            if src == target:
                pins.append((e, pin))
    for prop in PROPERTIES:
        if mel.get_material_property_input_node(material, getattr(unreal.MaterialProperty, prop)) == target:
            props.append(prop)
    return pins, props


def summary(material, exprs):
    v2 = find_new_node(exprs)
    v3 = find_v3_node(exprs)
    new = v3 or v2
    print('FOGMS_DENSITY material=%s nodes=%d FogMS_Extinction(v2)=%s FogMS_Extinction_v3=%s' % (
        MATERIAL_PATH, len(exprs), v2.get_name() if v2 else None, v3.get_name() if v3 else None))
    if v3:
        footprint = [e for e in custom_nodes(exprs) if str(e.get_editor_property('description')) == FOOTPRINT_DESCRIPTION]
        for node in footprint:
            for pin, src, out in input_links(material, node):
                print('  FOOTPRINT %s.%-14s <- %s.%s' % (node.get_name(), pin, src.get_name() if src else None, out))
        base = dict((pin, src) for pin, src, _ in input_links(material, v3)).get('Noise')
        if base is not None and isinstance(base, unreal.MaterialExpressionTextureSample):
            links = [(pin, src.get_name() if src else None) for pin, src, _ in input_links(material, base)]
            print('  BASE SAMPLE %s mip_value_mode=%s inputs=%s' % (base.get_name(), base.get_editor_property('mip_value_mode'), links))
        for name, _, _ in V3_SCALARS:
            print('  PARAM %s = %s' % (name, mel.get_material_default_scalar_parameter_value(material, name)))
        for name, _, _ in V3_VECTORS:
            print('  PARAM %s = %s' % (name, mel.get_material_default_vector_parameter_value(material, name)))
    if new:
        for pin, src, out in input_links(material, new):
            print('  IN  %-15s <- %s.%s' % (pin, src.get_name() if src else None, out))
        pins, props = consumers_of(material, exprs, new)
        for e, pin in pins:
            print('  OUT -> %s.%s (%s)' % (e.get_name(), pin, str(e.get_editor_property('description'))
                                          if isinstance(e, unreal.MaterialExpressionCustom) else ''))
        for prop in props:
            print('  OUT -> %s' % prop)
    for name, _, _ in NEW_SCALARS:
        print('  PARAM %s = %s' % (name, mel.get_material_default_scalar_parameter_value(material, name)))
    for name, _, _ in NEW_VECTORS:
        print('  PARAM %s = %s' % (name, mel.get_material_default_vector_parameter_value(material, name)))
    for prop in ('MP_BASE_COLOR', 'MP_EMISSIVE_COLOR', 'MP_SUBSURFACE_COLOR'):
        src = mel.get_material_property_input_node(material, getattr(unreal.MaterialProperty, prop))
        print('  PROPERTY %s <- %s' % (prop, src.get_name() if src else None))
    print('  %s field contract: %s' % (INJECTION_ALBEDO_DESCRIPTION, injection_albedo_version(exprs)))
    forward = find_forward_node(exprs)
    print('  %s: %s' % (FORWARD_DESCRIPTION, ('%s (v1=%s)' % (forward.get_name(), FORWARD_MARKER in normalized_code(forward)))
                                              if forward else None))
    if forward:
        for pin, src, out in input_links(material, forward):
            print('  FORWARD IN %-12s <- %s.%s' % (pin, src.get_name() if src else None, out))
        for name, _, _ in FORWARD_SCALARS:
            print('  PARAM %s = %s' % (name, mel.get_material_default_scalar_parameter_value(material, name)))


def normalized_code(node):
    return str(node.get_editor_property('code')).replace('\r\n', '\n').strip()


def find_single_custom(exprs, description):
    found = [e for e in custom_nodes(exprs) if str(e.get_editor_property('description')) == description]
    require(len(found) == 1, 'Expected one Custom node %s, found %d' % (description, len(found)))
    return found[0]


def injection_albedo_version(exprs):
    found = [e for e in custom_nodes(exprs) if str(e.get_editor_property('description')) == INJECTION_ALBEDO_DESCRIPTION]
    if len(found) != 1:
        return 'missing' if not found else 'several'
    code = normalized_code(found[0])
    if INJECTION_ALBEDO_V4_MARKER in code:
        return 'v4'
    return 'v3' if code == INJECTION_ALBEDO_CODE_V3.strip() else 'unknown'


def patch_injection_albedo(material):
    """Field contract v3 -> v4 (FogMS_InjectionMode 3) on the existing BaseColor node. Returns True if the code changed."""
    exprs = expressions(material)
    node = find_single_custom(exprs, INJECTION_ALBEDO_DESCRIPTION)
    code = normalized_code(node)
    if INJECTION_ALBEDO_V4_MARKER in code:
        print('INJECTION_ALBEDO already v4 (%s)' % node.get_name())
        return False
    require(code == INJECTION_ALBEDO_CODE_V3.strip(),
            '%s code is neither field contract v3 nor v4; not changed:\n%s' % (node.get_name(), code))
    names = list(mel.get_material_expression_input_names(node))
    require(sorted(names) == sorted(INJECTION_ALBEDO_INPUTS), 'Unexpected %s inputs: %s' % (node.get_name(), names))
    require(all(src is not None for _, src, _ in input_links(material, node)), node.get_name() + ' has unconnected inputs')
    require(mel.get_material_property_input_node(material, unreal.MaterialProperty.MP_BASE_COLOR) == node,
            'MP_BASE_COLOR is not fed by ' + node.get_name())
    emissive = find_single_custom(exprs, EMISSIVE_DESCRIPTION)
    require(EMISSIVE_REQUIRED in normalized_code(emissive),
            '%s does not emit for every Mode > 0.5 with a valid field; mode 3 would show nothing' % emissive.get_name())
    old_code = str(node.get_editor_property('code'))
    try:
        node.set_editor_property('code', INJECTION_ALBEDO_CODE_V4)
        require(sorted(mel.get_material_expression_input_names(node)) == sorted(INJECTION_ALBEDO_INPUTS), 'Inputs changed')
        errors = [str(err) for err in (mel.recompile_material(material) or [])]
        require(not errors, 'Compile errors: %s' % errors)
    except Exception:
        print('ROLLBACK', traceback.format_exc())
        node.set_editor_property('code', old_code)
        mel.recompile_material(material)
        print('NOT SAVED; %s is back on field contract v3 (unsaved in-memory state, reload the asset to discard).' % node.get_name())
        raise
    print('INJECTION_ALBEDO v3 -> v4 (%s)' % node.get_name())
    return True


def find_forward_node(exprs):
    found = [e for e in custom_nodes(exprs) if str(e.get_editor_property('description')) == FORWARD_DESCRIPTION]
    require(len(found) <= 1, 'Several Custom nodes are named %s' % FORWARD_DESCRIPTION)
    return found[0] if found else None


def patch_forward_lobe(material):
    """W37 F1a: FogMS_ForwardLobe between FogMS_EmissiveInjection and MP_EMISSIVE_COLOR (module docstring). Returns True if it
    changed the material; the caller saves. Any failure rolls back in memory and raises (nothing saved)."""
    exprs = expressions(material)
    emissive_prop = unreal.MaterialProperty.MP_EMISSIVE_COLOR
    node = find_forward_node(exprs)
    if node is not None:
        require(FORWARD_MARKER in normalized_code(node), '%s is named %s but its code is not %s' % (
            node.get_name(), FORWARD_DESCRIPTION, FORWARD_MARKER))
        require(mel.get_material_property_input_node(material, emissive_prop) == node,
                '%s exists but does not feed MP_EMISSIVE_COLOR; fix the graph by hand (no change made)' % node.get_name())
        print('FORWARD_LOBE already v1 (%s)' % node.get_name())
        return False
    emissive = find_single_custom(exprs, EMISSIVE_DESCRIPTION)
    require(EMISSIVE_REQUIRED in normalized_code(emissive),
            '%s is not field contract v3 (no FieldA validity); not changed' % emissive.get_name())
    require(mel.get_material_property_input_node(material, emissive_prop) == emissive,
            'MP_EMISSIVE_COLOR is not fed by %s; graph differs from the expected layout (no change made)' % emissive.get_name())
    sources = dict((pin, (src, out)) for pin, src, out in input_links(material, emissive))
    for pin in ('FieldA', 'Mode'):
        require(pin in sources and sources[pin][0] is not None, '%s.%s is not connected' % (emissive.get_name(), pin))
        src, out = sources[pin]
        if out == '':
            require(list(mel.get_material_expression_output_names(src)).count('') <= 1,
                    'Ambiguous unnamed output on %s (pin %s)' % (src.get_name(), pin))
    pins, props = consumers_of(material, exprs, emissive)
    print('%s consumers: pins=%s props=%s; FieldA <- %s.%s, Mode <- %s.%s' % (
        emissive.get_name(), [(e.get_name(), p) for e, p in pins], props, sources['FieldA'][0].get_name(), sources['FieldA'][1],
        sources['Mode'][0].get_name(), sources['Mode'][1]))

    created = []
    x, y = mel.get_material_expression_node_position(emissive)
    try:
        params = []
        for index, (name, default, pin) in enumerate(FORWARD_SCALARS):
            param = find_parameter(exprs, name)
            if param is None:
                param = mel.create_material_expression(material, unreal.MaterialExpressionScalarParameter, x, y + 320 + 110 * index)
                require(param is not None, 'Cannot create scalar ' + name)
                created.append(param)
                param.set_editor_property('parameter_name', name)
                param.set_editor_property('default_value', default)
                param.set_editor_property('group', 'FogMS')
            params.append((pin, param, ''))
        camera = mel.create_material_expression(material, unreal.MaterialExpressionCameraVectorWS, x, y + 320 + 110 * len(FORWARD_SCALARS))
        require(camera is not None, 'Cannot create the CameraVectorWS node')
        created.append(camera)
        lobe = mel.create_material_expression(material, unreal.MaterialExpressionCustom, x + 380, y)
        require(lobe is not None, 'Cannot create the FogMS_ForwardLobe Custom node')
        created.append(lobe)
        lobe.set_editor_property('code', FORWARD_LOBE_CODE_V1)
        lobe.set_editor_property('output_type', unreal.CustomMaterialOutputType.CMOT_FLOAT3)
        lobe.set_editor_property('description', FORWARD_DESCRIPTION)
        items = []
        for pin in FORWARD_INPUTS:
            item = unreal.CustomInput()
            item.set_editor_property('input_name', pin)
            items.append(item)
        lobe.set_editor_property('inputs', items)
        links = ([('Emissive', emissive, ''), ('FieldA',) + sources['FieldA'], ('Mode',) + sources['Mode']] + params
                 + [('CameraVector', camera, '')])
        require(sorted(pin for pin, _, _ in links) == sorted(FORWARD_INPUTS), 'Forward-lobe pin list mismatch')
        for pin, src, out in links:
            require(mel.connect_material_expressions(src, out, lobe, pin),
                    'Cannot wire %s.%s -> FogMS_ForwardLobe.%s' % (src.get_name(), out, pin))
            back = mel.get_input_node_output_name_for_material_expression(lobe, src)
            require(back is not None and str(back) == out, 'Readback mismatch on pin %s: %s != %s' % (pin, back, out))
        require(mel.connect_material_property(lobe, '', emissive_prop), 'Cannot route FogMS_ForwardLobe -> MP_EMISSIVE_COLOR')
        require(mel.get_material_property_input_node(material, emissive_prop) == lobe, 'MP_EMISSIVE_COLOR readback mismatch')

        errors, note = compile_and_check(material, 'FOGMS_MATEDIT_FWD_%d' % int(time.time() * 1000))
        print('COMPILE forward lobe:', note)
        require(not errors, 'Compile errors: %s' % errors)
    except Exception:
        # Roll back in memory: MP_EMISSIVE_COLOR back to the injection node, delete what this step created, recompile, do not save.
        print('ROLLBACK', traceback.format_exc())
        mel.connect_material_property(emissive, '', emissive_prop)
        for node in reversed(created):
            mel.delete_material_expression(material, node)
        mel.recompile_material(material)
        print('NOT SAVED; MP_EMISSIVE_COLOR is back on %s (unsaved in-memory state, reload the asset to discard).' % emissive.get_name())
        raise
    print('FORWARD_LOBE patched (%s -> %s -> MP_EMISSIVE_COLOR; camera %s)' % (emissive.get_name(), lobe.get_name(), camera.get_name()))
    return True


def parameter_names(exprs):
    names = set()
    for e in exprs:
        try:
            names.add(str(e.get_editor_property('parameter_name')))
        except Exception:
            pass
    return names


def main():
    material = unreal.load_asset(MATERIAL_PATH)
    require(material is not None and isinstance(material, unreal.Material), 'Material not found: ' + MATERIAL_PATH)
    existing = parameter_names(expressions(material))
    changed = False
    if find_new_node(expressions(material)) or find_v3_node(expressions(material)):
        print('EXTINCTION already patched (FogMS_Extinction v2 or later)')
    else:
        patch_extinction(material)
        changed = True
    changed = patch_extinction_v3(material) or changed
    changed = patch_injection_albedo(material) or changed
    changed = patch_forward_lobe(material) or changed
    if changed:
        # Exact defaults only for parameters this run created. A pre-existing one keeps its material default (the Box MID
        # sets every one of them); a difference is reported, not fatal (round 36: FogMS_ErosionDepth 0.3 from round 32).
        for name, default, _ in NEW_SCALARS + V3_SCALARS + FORWARD_SCALARS:
            value = mel.get_material_default_scalar_parameter_value(material, name)
            if name in existing:
                if abs(value - default) >= 1e-6:
                    print('NOTE pre-existing %s default %s (script default %s): kept, the Box MID overrides it' % (name, value, default))
            else:
                require(abs(value - default) < 1e-6, 'Default readback %s = %s (created by this run with %s)' % (name, value, default))
        saved = unreal.EditorAssetLibrary.save_asset(MATERIAL_PATH, only_if_is_dirty=False)
        print('PATCHED saved=%s' % saved)
    else:
        print('ALREADY_PATCHED')
    summary(material, expressions(material))


def patch_extinction(material):
    """Steps 1-5 of the module docstring (FogMS_Extinction v2). Recompiles; the caller saves."""
    exprs = expressions(material)
    print('USH_SYNC', ush_sync_check())
    old = find_old_node(exprs)
    old_links = input_links(material, old)
    require(all(src is not None for _, src, _ in old_links),
            'V1 node has unconnected inputs: %s' % [pin for pin, src, _ in old_links if src is None])
    for pin, src, out in old_links:
        # An empty output name reconnects output 0; refuse if that would be ambiguous.
        if out == '':
            require(list(mel.get_material_expression_output_names(src)).count('') <= 1,
                    'Ambiguous unnamed output on %s (pin %s)' % (src.get_name(), pin))
    pins, props = consumers_of(material, exprs, old)
    require('MP_SUBSURFACE_COLOR' in props,
            'MP_SUBSURFACE_COLOR (extinction) is not fed by %s; graph differs from the expected A1e layout' % old.get_name())
    print('V1 node %s consumers: pins=%s props=%s' % (old.get_name(), [(e.get_name(), p) for e, p in pins], props))

    created = []
    x, y = mel.get_material_expression_node_position(old)
    try:
        params = []
        for index, (name, default, pin) in enumerate(NEW_SCALARS):
            node = find_parameter(exprs, name)
            if node is None:
                node = mel.create_material_expression(material, unreal.MaterialExpressionScalarParameter, x - 400, y + 700 + 110 * index)
                require(node is not None, 'Cannot create scalar ' + name)
                created.append(node)
                node.set_editor_property('parameter_name', name)
                node.set_editor_property('default_value', default)
                node.set_editor_property('group', 'FogMS')
            params.append((node, '', pin))
        for index, (name, default, pin) in enumerate(NEW_VECTORS):
            node = find_parameter(exprs, name)
            if node is None:
                node = mel.create_material_expression(material, unreal.MaterialExpressionVectorParameter,
                                                      x - 400, y + 700 + 110 * (len(NEW_SCALARS) + index))
                require(node is not None, 'Cannot create vector ' + name)
                created.append(node)
                node.set_editor_property('parameter_name', name)
                node.set_editor_property('default_value', unreal.LinearColor(*default))
                node.set_editor_property('group', 'FogMS')
            params.append((node, 'RGBA', pin))

        new = mel.create_material_expression(material, unreal.MaterialExpressionCustom, x, y + 700)
        require(new is not None, 'Cannot create the FogMS_Extinction Custom node')
        created.append(new)
        new.set_editor_property('code', EXTINCTION_CODE_V2)
        new.set_editor_property('output_type', unreal.CustomMaterialOutputType.CMOT_FLOAT1)
        new.set_editor_property('description', NEW_DESCRIPTION)
        inputs = []
        for pin in [p for p, _, _ in old_links] + [p for _, _, p in params]:
            item = unreal.CustomInput()
            item.set_editor_property('input_name', pin)
            inputs.append(item)
        new.set_editor_property('inputs', inputs)
        for pin, src, out in old_links + [(p, n, o) for n, o, p in params]:
            require(mel.connect_material_expressions(src, out, new, pin),
                    'Cannot wire %s.%s -> FogMS_Extinction.%s' % (src.get_name(), out, pin))
            back = mel.get_input_node_output_name_for_material_expression(new, src)
            require(back is not None and str(back) == out, 'Readback mismatch on pin %s: %s != %s' % (pin, back, out))
        # Same source types on the carried-over pins as on the V1 node.
        old_types = dict(zip(mel.get_material_expression_input_names(old), mel.get_material_expression_input_types(old)))
        new_types = dict(zip(mel.get_material_expression_input_names(new), mel.get_material_expression_input_types(new)))
        require(all(old_types[p] == new_types[p] for p in OLD_INPUTS), 'Carried-over input types differ')

        for e, pin in pins:
            require(mel.connect_material_expressions(new, '', e, pin), 'Cannot re-route %s.%s' % (e.get_name(), pin))
        for prop in props:
            require(mel.connect_material_property(new, '', getattr(unreal.MaterialProperty, prop)), 'Cannot re-route ' + prop)
        left_pins, left_props = consumers_of(material, expressions(material), old)
        require(not left_pins and not left_props, 'V1 node still has consumers: %s %s' % (left_pins, left_props))

        errors = [str(err) for err in (mel.recompile_material(material) or [])]
        require(not errors, 'Compile errors: %s' % errors)
    except Exception:
        # Roll back in memory: consumers back to V1, delete what this run created, recompile, do not save.
        print('ROLLBACK', traceback.format_exc())
        for e, pin in pins:
            mel.connect_material_expressions(old, '', e, pin)
        for prop in props:
            mel.connect_material_property(old, '', getattr(unreal.MaterialProperty, prop))
        for node in created:
            mel.delete_material_expression(material, node)
        mel.recompile_material(material)
        print('NOT SAVED; the material is back on %s (unsaved in-memory state, reload the asset to discard).' % old.get_name())
        raise
    print('EXTINCTION patched (FogMS_Extinction v2)')


def editor_log_path():
    """The running editor's log file: -ABSLOG=<path>, -LOG=<name> (in the project log dir), else the newest *.log there."""
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
    """Recompiles and waits for this material's shaders on the running platform, then returns its errors.
    RecompileMaterial returns only the synchronous (translation) errors; a Custom node's HLSL fails later, in the async
    shader compile. MaterialEditingLibrary.get_statistics runs FMaterialResource::FinishCompilation, which processes the
    result and logs 'Failed to compile Material <name> ...' on failure; the editor log after `marker` is scanned for it.
    Returns (errors, note): errors = list of strings (empty = compiled), note = how the shader result was checked."""
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
        return [], 'shader result NOT verified (editor log not found): check the Output Log for "Failed to compile Material"'
    with open(path, 'rb') as f:
        f.seek(max(0, os.path.getsize(path) - (16 << 20)))  # the marker is recent; editor logs can be large
        text = f.read().decode('utf-8', errors='replace')
    if marker not in text:
        return [], 'shader result NOT verified (marker not in %s): check the Output Log for "Failed to compile Material"' % path
    tail = text.rsplit(marker, 1)[1]
    failed = [i for i, line in enumerate(tail.splitlines()) if 'Failed to compile Material' in line and 'M_FogMS_Density' in line]
    if failed:
        lines = tail.splitlines()
        return ['\n'.join(lines[i:i + 12]) for i in failed], 'shader (log %s)' % path
    return [], 'shader compiled (log %s)' % path


def mip_mode(tail):
    """unreal.TextureMipValueMode member ending in `tail` (TMVM_NONE, TMVM_MIP_BIAS), or None."""
    enum = getattr(unreal, 'TextureMipValueMode', None)
    if enum is None:
        return None
    for name in ('TMVM_' + tail, tail):
        if hasattr(enum, name):
            return getattr(enum, name)
    return None


def patch_extinction_v3(material):
    """W36: FogMS_Extinction v2 -> v3 (depth prefilter), module docstring. Returns True if it changed the material; the caller
    saves. Any failure rolls back in memory and raises (nothing saved)."""
    exprs = expressions(material)
    v3 = find_v3_node(exprs)
    if v3 is not None:
        require(V3_MARKER in normalized_code(v3), '%s is named %s but its code is not v3' % (v3.get_name(), V3_DESCRIPTION))
        require(mel.get_material_property_input_node(material, unreal.MaterialProperty.MP_SUBSURFACE_COLOR) == v3,
                '%s exists but does not feed MP_SUBSURFACE_COLOR; fix the graph by hand (no change made)' % v3.get_name())
        print('EXTINCTION already v3 (%s)' % v3.get_name())
        return False
    print('USH_SYNC', ush_sync_check())
    v2 = find_new_node(exprs)
    require(v2 is not None, 'No FogMS_Extinction (v2) node to upgrade')
    require(normalized_code(v2) == EXTINCTION_CODE_V2.strip(),
            '%s code is not EXTINCTION_CODE_V2 (edited by hand?); not changed' % v2.get_name())
    v2_links = input_links(material, v2)
    require(sorted(pin for pin, _, _ in v2_links) == sorted(V2_INPUTS), 'Unexpected v2 inputs: %s' % [p for p, _, _ in v2_links])
    require(all(src is not None for _, src, _ in v2_links),
            'v2 node has unconnected inputs: %s' % [pin for pin, src, _ in v2_links if src is None])
    for pin, src, out in v2_links:
        if out == '':
            require(list(mel.get_material_expression_output_names(src)).count('') <= 1,
                    'Ambiguous unnamed output on %s (pin %s)' % (src.get_name(), pin))
    pins, props = consumers_of(material, exprs, v2)
    require('MP_SUBSURFACE_COLOR' in props, 'MP_SUBSURFACE_COLOR (extinction) is not fed by %s' % v2.get_name())
    print('v2 node %s consumers: pins=%s props=%s' % (v2.get_name(), [(e.get_name(), p) for e, p in pins], props))

    # (b) base noise mip bias: only on a texture sample in mode None that feeds neither detail pin.
    sources = dict((pin, src) for pin, src, _ in v2_links)
    base = sources['Noise']
    bias_skip = None
    mode_none, mode_bias = mip_mode('NONE'), mip_mode('MIP_BIAS')
    if not isinstance(base, unreal.MaterialExpressionTextureSample):
        bias_skip = '%s (Noise) is not a texture sample' % base.get_name()
    elif base in (sources['Detail0'], sources['Detail1']):
        bias_skip = '%s also feeds a detail pin' % base.get_name()
    elif mode_none is None or mode_bias is None:
        bias_skip = 'unreal.TextureMipValueMode NONE/MIP_BIAS not found'
    elif base.get_editor_property('mip_value_mode') != mode_none:
        bias_skip = '%s mip_value_mode is %s, not None' % (base.get_name(), base.get_editor_property('mip_value_mode'))
    if bias_skip:
        print('MIP_BIAS skipped: %s. The prefilter keeps (a) detail fade and (c) band widening only.' % bias_skip)
    else:
        other = [(e.get_name(), pin) for e, pin in consumers_of(material, exprs, base)[0] if e not in (v2,)]
        print('MIP_BIAS on %s; its other consumers (see the same biased sample): %s' % (base.get_name(), other))

    created, mip_changed, bias_pin = [], False, None
    x, y = mel.get_material_expression_node_position(v2)
    try:
        params = {}
        for index, (name, default, pin) in enumerate(V3_SCALARS):
            node = find_parameter(exprs, name)
            if node is None:
                node = mel.create_material_expression(material, unreal.MaterialExpressionScalarParameter, x - 400, y + 700 + 110 * index)
                require(node is not None, 'Cannot create scalar ' + name)
                created.append(node)
                node.set_editor_property('parameter_name', name)
                node.set_editor_property('default_value', default)
                node.set_editor_property('group', 'FogMS')
            params[pin] = (node, '')
        for index, (name, default, pin) in enumerate(V3_VECTORS):
            node = find_parameter(exprs, name)
            if node is None:
                node = mel.create_material_expression(material, unreal.MaterialExpressionVectorParameter,
                                                      x - 400, y + 700 + 110 * (len(V3_SCALARS) + index))
                require(node is not None, 'Cannot create vector ' + name)
                created.append(node)
                node.set_editor_property('parameter_name', name)
                node.set_editor_property('default_value', unreal.LinearColor(*default))
                node.set_editor_property('group', 'FogMS')
            params[pin] = (node, 'RGBA')

        depth = mel.create_material_expression(material, unreal.MaterialExpressionPixelDepth, x - 700, y + 950)
        require(depth is not None, 'Cannot create the PixelDepth node')
        created.append(depth)
        footprint = mel.create_material_expression(material, unreal.MaterialExpressionCustom, x - 400, y + 950)
        require(footprint is not None, 'Cannot create the FogMS_DepthFootprint Custom node')
        created.append(footprint)
        footprint.set_editor_property('code', FOOTPRINT_CODE_V1)
        footprint.set_editor_property('output_type', unreal.CustomMaterialOutputType.CMOT_FLOAT2)
        footprint.set_editor_property('description', FOOTPRINT_DESCRIPTION)
        items = []
        for pin in FOOTPRINT_INPUTS:
            item = unreal.CustomInput()
            item.set_editor_property('input_name', pin)
            items.append(item)
        footprint.set_editor_property('inputs', items)
        require(mel.connect_material_expressions(depth, '', footprint, 'PixelDepth'), 'Cannot wire PixelDepth')
        node, out = params['DepthPrefilter']
        require(mel.connect_material_expressions(node, out, footprint, 'DepthPrefilter'), 'Cannot wire DepthPrefilter')

        new = mel.create_material_expression(material, unreal.MaterialExpressionCustom, x, y + 700)
        require(new is not None, 'Cannot create the FogMS_Extinction_v3 Custom node')
        created.append(new)
        new.set_editor_property('code', EXTINCTION_CODE_V3)
        new.set_editor_property('output_type', unreal.CustomMaterialOutputType.CMOT_FLOAT1)
        new.set_editor_property('description', V3_DESCRIPTION)
        new_links = v2_links + [('Footprint', footprint, ''), ('Wavelengths',) + params['Wavelengths']]
        items = []
        for pin, _, _ in new_links:
            item = unreal.CustomInput()
            item.set_editor_property('input_name', pin)
            items.append(item)
        new.set_editor_property('inputs', items)
        for pin, src, out in new_links:
            require(mel.connect_material_expressions(src, out, new, pin),
                    'Cannot wire %s.%s -> FogMS_Extinction_v3.%s' % (src.get_name(), out, pin))
            back = mel.get_input_node_output_name_for_material_expression(new, src)
            require(back is not None and str(back) == out, 'Readback mismatch on pin %s: %s != %s' % (pin, back, out))
        v2_types = dict(zip(mel.get_material_expression_input_names(v2), mel.get_material_expression_input_types(v2)))
        v3_types = dict(zip(mel.get_material_expression_input_names(new), mel.get_material_expression_input_types(new)))
        require(all(v2_types[p] == v3_types[p] for p in V2_INPUTS), 'Carried-over input types differ')

        if not bias_skip:
            mask = mel.create_material_expression(material, unreal.MaterialExpressionComponentMask, x - 150, y + 950)
            require(mask is not None, 'Cannot create the mip-bias ComponentMask')
            created.append(mask)
            for channel, on in (('r', False), ('g', True), ('b', False), ('a', False)):
                mask.set_editor_property(channel, on)
            require(mel.connect_material_expressions(footprint, '', mask, ''), 'Cannot wire FogMS_DepthFootprint -> mask')
            base.set_editor_property('mip_value_mode', mode_bias)
            mip_changed = True
            for candidate in ('Bias', 'MipBias'):
                if mel.connect_material_expressions(mask, '', base, candidate):
                    bias_pin = candidate
                    break
            require(bias_pin is not None, 'Cannot wire the mip bias into %s: inputs %s' % (
                base.get_name(), list(mel.get_material_expression_input_names(base))))

        for e, pin in pins:
            require(mel.connect_material_expressions(new, '', e, pin), 'Cannot re-route %s.%s' % (e.get_name(), pin))
        for prop in props:
            require(mel.connect_material_property(new, '', getattr(unreal.MaterialProperty, prop)), 'Cannot re-route ' + prop)
        left_pins, left_props = consumers_of(material, expressions(material), v2)
        require(not left_pins and not left_props, 'v2 node still has consumers: %s %s' % (left_pins, left_props))

        errors, note = compile_and_check(material, 'FOGMS_MATEDIT_V3_%d' % int(time.time() * 1000))
        print('COMPILE v3:', note)
        require(not errors, 'Compile errors: %s' % errors)
    except Exception:
        # Roll back in memory: consumers back to v2, base sample back to its mip mode, delete what this run created,
        # recompile, do not save.
        print('ROLLBACK', traceback.format_exc())
        for e, pin in pins:
            mel.connect_material_expressions(v2, '', e, pin)
        for prop in props:
            mel.connect_material_property(v2, '', getattr(unreal.MaterialProperty, prop))
        if mip_changed:
            # Break the bias link while its pin is visible (mode None hides MipValue from the input iterator).
            disconnect = getattr(mel, 'disconnect_material_expressions', None)
            for candidate in (bias_pin,) if bias_pin else ():
                if disconnect:
                    disconnect(base, candidate)
            base.set_editor_property('mip_value_mode', mode_none)
        for node in reversed(created):
            mel.delete_material_expression(material, node)
        mel.recompile_material(material)
        print('NOT SAVED; the material is back on %s (unsaved in-memory state, reload the asset to discard).' % v2.get_name())
        raise
    print('EXTINCTION v2 -> v3 (%s; footprint %s; base mip bias %s)' % (
        new.get_name(), footprint.get_name(), ('%s.%s' % (base.get_name(), bias_pin)) if bias_pin else 'skipped'))
    return True


try:
    main()
except Exception:
    print('TRACE', traceback.format_exc())
