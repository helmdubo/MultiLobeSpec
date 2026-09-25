"""FogMS P1 (FogMS_PerPixelClouds_Design.md section 4): the cloud-host prototype material
/MultiLobeSpec/FogMS/Proto/M_FogMS_CloudBox_P1, built in the editor by this script and not committed (it is recreated here,
idempotently). Run in the editor, with no other session editing the material: cloudproto_session.py sends this file through
the UE-MCP bridge (it prepends P1_PROBE_DIR, the folder of matedit_density.py), or `py "<this file>"` in the Python console.

Data flow per ray-march step of the native Volumetric Cloud (VolumetricCloud.usf, RenderVolumetricCloudRenderViewCS), for the
one Box whose parameters cloudproto_session.py copies from the Box MID into this material's MID:
  WorldPosition node, DEFAULT mode (= MaterialParameters.AbsoluteWorldPosition, the step's sample; the cloud does not set the
    *_NoOffsets positions the Box material reads: VolumetricCloudMaterialPixelCommon.ush UpdateMaterialCloudParam)
    - P1_BoxCenter                  = WorldOffset [cm] (M_FogMS_Density: WorldPosition - ObjectPositionWS)
    -> P1_BoxLocal (rows P1_WorldToLocal0..2 = the density cube's world-to-local matrix relative to P1_BoxCenter)
                                    = LocalPosition in cube space -50..50 (M_FogMS_Density: TransformPosition World -> Local)
    -> three noise UVW nodes (the code of M_FogMS_Density's UVW Custom nodes) -> FogMS_Noise sampled at mip 0, three times
    -> FogMS_Extinction_v3 (EXTINCTION_CODE_V3 read from matedit_density.py, Footprint 0 = w 0: the solver's formula)
                                    = sigma_t [1/m] -> SubsurfaceColor (the cloud's extinction, usf SampleExtinctionCoefficients)
  FogMS_Albedo -> BaseColor: the cloud scatters Albedo * sigma_t of the sun itself, with a march toward the sun on every step
    (Volumetric Advanced Output, RayMarchVolumeShadow on) and the host phase HG(P1_PhaseG, P1_PhaseG2, P1_PhaseBlend).
  FogMS_TransportField at LocalPosition * 0.01 + 0.5 (the Box's hybrid field: RGB = J minus the uncollided sun term,
    A = 0.5 + 0.5 * T_sun * k)
    -> FogMS_EmissiveInjection (valid = Mode > 0.5 and A >= 0.5; valid * Field.rgb * Albedo * sigma_t * P1_FieldGain)
    -> FogMS_ForwardLobe (FORWARD_LOBE_CODE_V2 from matedit_density.py; FieldA <- the field's ALPHA, S = T_sun * k)
    -> Emissive. The cloud integrates Emissive as luminance per metre (usf: ScatteredLuminance = SunSky * sigma_s + Emissive),
       the units of sigma_s[1/m] * J; the fog path scales extinction and Emissive by 1/100 per cm, so the same value lights both.
  P1_SkyAO: 0 with a valid field (the field carries the sky with the medium's self-shadowing), 1 without -> AmbientOcclusion,
    which replaces the cloud's own sky-bottom gradient (usf: DistantLightLuminance *= AO).
  P1_Conservative -> ConservativeDensity.x: 1 inside the density cube and, with the height profile on, inside the profile band
    when detail alone cannot reach the threshold band; else 0 (usf: conservative density <= 0 skips the material).
  Volumetric Advanced Output: phase per pixel, multiple-scattering octaves 0 (Emissive is added once per octave, usf 1359-1384),
    ground contribution off, ray-marched volume shadow on (off = the cloud shadow map, which needs Cast Cloud Shadows),
    colour (grey-scale keeps only the red channel of Emissive and extinction).
Material: Volume, Additive, DefaultLit (Unlit zeroes albedo and extinction, usf 109-159), Used with Volumetric Cloud.

Idempotent: an existing asset whose P1_BoxLocal node carries MARKER and whose extinction / lobe nodes carry the current
matedit_density.py code prints ALREADY_PATCHED and changes nothing. An older or different graph is rebuilt in place (all
expressions deleted and recreated, the asset itself is kept). The build recompiles and scans the editor log for
'Failed to compile Material ... M_FogMS_CloudBox_P1' (as matedit_density.compile_and_check); on any error nothing is saved.
Output: MATERIAL_OK saved=<bool> | ALREADY_PATCHED | TRACE <traceback>."""
import ast
import glob
import os
import shlex
import time
import traceback

import unreal

ASSET_DIR = '/MultiLobeSpec/FogMS/Proto'
ASSET_NAME = 'M_FogMS_CloudBox_P1'
MATERIAL_PATH = ASSET_DIR + '/' + ASSET_NAME
MARKER = 'FogMS_CloudBox_P1 v1'
DEFAULT_VOLUME = '/MultiLobeSpec/FogMS/T_FogMS_DefaultVolume'
mel = unreal.MaterialEditingLibrary

# (name, default). FogMS_* names and meanings are the Box MID's (FogMS_BoxVolume.cpp UpdateDensity): the session copies them.
SCALARS = (('FogMS_WorldAligned', 0.0), ('FogMS_DetailScale', 4.0), ('FogMS_Threshold', 0.5), ('FogMS_Softness', 0.1),
           ('FogMS_Density', 0.1), ('FogMS_DensityFeather', 100.0), ('FogMS_DetailStrength', 0.0), ('FogMS_DetailSecondOctave', 0.5),
           ('FogMS_InjectionMode', 0.0), ('FogMS_ErosionStrength', 0.0), ('FogMS_ErosionDepth', 0.15), ('FogMS_HeightProfile', 0.0),
           ('FogMS_HeightBottom', 0.0), ('FogMS_HeightTop', 1.0), ('FogMS_HeightBottomSoftness', 0.05),
           ('FogMS_HeightTopSoftness', 0.1), ('FogMS_HeightAnvilStrength', 0.0), ('FogMS_ForwardStrength', 0.0),
           ('FogMS_ForwardG', 0.6), ('FogMS_ForwardDepth', 0.5), ('FogMS_ForwardFloor', 0.25), ('FogMS_ForwardEcc', 0.5),
           # host look (one phase per pixel for the whole cloud host) and a debug gain on the field (0 = sun single scattering only)
           ('P1_PhaseG', 0.6), ('P1_PhaseG2', 0.0), ('P1_PhaseBlend', 0.0), ('P1_FieldGain', 1.0))
VECTORS = (('FogMS_TileScale', (1.0, 1.0, 1.0, 0.0)), ('FogMS_WorldFrequencies', (0.0005, 0.002, 0.004, 0.0)),
           ('FogMS_WorldPhase0', (0.0, 0.0, 0.0, 0.0)), ('FogMS_WorldPhase1', (0.0, 0.0, 0.0, 0.0)),
           ('FogMS_WorldPhase2', (0.0, 0.0, 0.0, 0.0)), ('FogMS_ChannelMask', (1.0, 0.0, 0.0, 0.0)),
           ('FogMS_WorldExtent', (1000.0, 1000.0, 1000.0, 0.0)), ('FogMS_Albedo', (1.0, 1.0, 1.0, 1.0)),
           ('FogMS_ErosionMask', (0.0, 1.0, 0.0, 0.0)), ('P1_BoxCenter', (0.0, 0.0, 0.0, 0.0)),
           # a 1000-cm cube at the origin: local = world * 50 / 1000
           ('P1_WorldToLocal0', (0.05, 0.0, 0.0, 0.0)), ('P1_WorldToLocal1', (0.0, 0.05, 0.0, 0.0)),
           ('P1_WorldToLocal2', (0.0, 0.0, 0.05, 0.0)))

LOCAL_CODE = """// %s (P1 cloud-host prototype, cloudproto_material.py). Box density-cube space of this ray-march sample, -50..50 per axis:
// the LocalPosition M_FogMS_Density gets from TransformPosition(World -> Local) of its cube. Row_i.xyz = world axis i of the cube
// divided by the cube's world scale on that axis (so +-50 at the Box faces), Row_i.w = offset; WorldOffset = sample - P1_BoxCenter [cm].
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
EMISSIVE_CODE = """// P1 copy of FogMS_EmissiveInjection (field contract v3) with a debug gain. valid = Mode > 0.5 and FieldA >= 0.5 (hybrid:
// A = 0.5 + 0.5 * T_sun * k); Field = J minus the uncollided sun (the cloud marches the sun itself). The cloud adds Emissive as
// luminance per metre (VolumetricCloud.usf), the units of sigma_s[1/m] * J: the same value the Box material gives the fog.
float V = (Mode > 0.5f && FieldA >= 0.5f) ? 1.0f : 0.0f;
return V * Field * Albedo * max(Extinction, 0.0f) * Gain;"""
SKY_AO_CODE = """// P1: the cloud's own sky light is replaced by the field (which carries the sky with the medium's self-shadowing): AO 0 with a
// valid field, 1 without (then the cloud lights the Box with the unshadowed distant sky, VolumetricCloud.usf DistantLightLuminance).
return (Mode > 0.5f && FieldA >= 0.5f) ? 0.0f : 1.0f;"""
CONSERVATIVE_CODE = """// P1 conservative density (Volumetric Advanced Output, x): 1 where the Box can hold density, 0 where it provably cannot, so empty
// ray-march steps skip the material (VolumetricCloud.usf: conservative density <= 0 -> next step). Outside the cube the edge fade
// of FogMS_Extinction is 0. Height profile on: P = 0 below HeightBottom and above HeightTop, so there n = detail only with
// |detail| <= DetailStrength (erosion only subtracts); below the threshold band's lower edge Lo = Threshold - Softness / 2 the
// mask is 0 as well, so the band limits the conservative region only when DetailStrength < Lo.
bool Inside = all(abs(LocalPosition) <= 50.0f);
float h = LocalPosition.z * 0.01f + 0.5f;
bool Band = HeightProfile < 0.5f || DetailStrength >= Threshold - 0.5f * Softness || (h >= HeightBottom && h <= HeightTop);
return float3((Inside && Band) ? 1.0f : 0.0f, 0.0f, 0.0f);"""

EXTINCTION_PINS = ('Noise', 'Detail0', 'Detail1', 'ChannelMask', 'DetailStrength', 'SecondOctave', 'LocalPosition', 'WorldExtent',
                   'Threshold', 'Softness', 'Density', 'Feather', 'ErosionStrength', 'ErosionDepth', 'HeightProfile', 'HeightBottom',
                   'HeightTop', 'BottomSoftness', 'TopSoftness', 'AnvilStrength', 'ErosionMask', 'Footprint', 'Wavelengths')
FORWARD_PINS = ('Emissive', 'FieldA', 'Mode', 'Strength', 'G', 'Depth', 'BackFloor', 'CameraVector', 'Ecc')


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def probe_dir():
    here = globals().get('__file__')
    if here:
        return os.path.dirname(os.path.abspath(here))
    path = globals().get('P1_PROBE_DIR')
    require(path, 'P1_PROBE_DIR (folder of matedit_density.py) is not set')
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


def custom_nodes(material):
    return [e for e in mel.get_material_expressions(material) if isinstance(e, unreal.MaterialExpressionCustom)]


def by_description(material, description):
    found = [e for e in custom_nodes(material) if str(e.get_editor_property('description')) == description]
    return found[0] if len(found) == 1 else None


def is_current(material, extinction_code, lobe_code):
    local = by_description(material, 'P1_BoxLocal')
    ext = by_description(material, 'FogMS_Extinction_v3')
    lobe = by_description(material, 'FogMS_ForwardLobe')
    if not (local and ext and lobe):
        return False
    if norm(local.get_editor_property('code')) != norm(LOCAL_CODE) or norm(ext.get_editor_property('code')) != norm(extinction_code) \
            or norm(lobe.get_editor_property('code')) != norm(lobe_code):
        return False
    types = dict(zip(mel.get_material_expression_input_names(lobe), mel.get_material_expression_input_types(lobe)))
    return types.get('FieldA') not in (4, 8) and mel.has_material_usage(material, unreal.MaterialUsage.MATUSAGE_VOLUMETRIC_CLOUD)


class Graph:
    def __init__(self, material):
        self.m = material
        self.params = {}

    def node(self, cls, x, y, **props):
        e = mel.create_material_expression(self.m, cls, x, y)
        require(e is not None, 'Cannot create ' + cls.__name__)
        for k, v in props.items():
            e.set_editor_property(k, v)
        return e

    def link(self, src, out, dst, pin):
        require(mel.connect_material_expressions(src, out, dst, pin), 'Cannot wire %s.%s -> %s.%s' % (src.get_name(), out, dst.get_name(), pin))

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
        e.set_editor_property('group', 'FogMS P1')
        self.params[name] = e
        return e

    def vector(self, name, default, x, y):
        e = self.node(unreal.MaterialExpressionVectorParameter, x, y)
        e.set_editor_property('parameter_name', name)
        e.set_editor_property('default_value', unreal.LinearColor(*default))
        e.set_editor_property('group', 'FogMS P1')
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


def build(material, extinction_code, lobe_code):
    """Creates the whole graph (module docstring) in an empty material."""
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
    g.link(wp, 'XYZ', offset, 'A'); g.link(P['P1_BoxCenter'], 'RGB', offset, 'B')
    local = g.custom('P1_BoxLocal', LOCAL_CODE, unreal.CustomMaterialOutputType.CMOT_FLOAT3,
                     ('WorldOffset', 'Row0', 'Row1', 'Row2'), -1600, -400)
    g.link(offset, '', local, 'WorldOffset')
    for i in range(3):
        g.link(P['P1_WorldToLocal%d' % i], 'RGBA', local, 'Row%d' % i)
    uvw = []
    for i, code in enumerate((UVW0_CODE, UVW1_CODE, UVW2_CODE)):
        pins = ('WorldAligned', 'WorldOffset', 'WorldFrequencies', 'WorldPhase') + (('LocalPosition', 'TileScale') if i == 0 else ('UVW', 'DetailScale'))
        node = g.custom('P1_NoiseUVW%d' % i, code, unreal.CustomMaterialOutputType.CMOT_FLOAT3, pins, -1300, -200 + 260 * i)
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
        s.set_editor_property('parameter_name', 'FogMS_Noise'); s.set_editor_property('group', 'FogMS P1')
        s.set_editor_property('texture', volume)
        s.set_editor_property('sampler_type', unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR)
        s.set_editor_property('mip_value_mode', unreal.TextureMipValueMode.TMVM_MIP_LEVEL)
        g.link(uvw[i], '', s, 'UVs'); g.link(mip0, '', s, 'Level')
        samples.append(s)
    footprint = g.node(unreal.MaterialExpressionConstant2Vector, -800, 700, r=0.0, g=0.0)
    wavelengths = g.node(unreal.MaterialExpressionConstant4Vector, -800, 800, constant=unreal.LinearColor(0.0, 0.0, 0.0, 0.0))
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
               'Footprint': (footprint, ''), 'Wavelengths': (wavelengths, '')}
    for pin in EXTINCTION_PINS:
        g.link(sources[pin][0], sources[pin][1], ext, pin)
    fuvw = g.custom('FogMS_TransportUVW', FIELD_UVW_CODE, unreal.CustomMaterialOutputType.CMOT_FLOAT3, ('LocalPosition',), -600, 1000)
    g.link(local, '', fuvw, 'LocalPosition')
    field = g.node(unreal.MaterialExpressionTextureSampleParameterVolume, -350, 1000)
    field.set_editor_property('parameter_name', 'FogMS_TransportField'); field.set_editor_property('group', 'FogMS P1')
    field.set_editor_property('texture', volume)
    field.set_editor_property('sampler_type', unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR)
    g.link(fuvw, '', field, 'UVs')
    emissive = g.custom('FogMS_EmissiveInjection', EMISSIVE_CODE, unreal.CustomMaterialOutputType.CMOT_FLOAT3,
                        ('Field', 'FieldA', 'Albedo', 'Extinction', 'Mode', 'Gain'), -50, 700)
    g.link(field, 'RGB', emissive, 'Field'); g.link(field, 'A', emissive, 'FieldA'); g.link(P['FogMS_Albedo'], 'RGB', emissive, 'Albedo')
    g.link(ext, '', emissive, 'Extinction'); g.link(P['FogMS_InjectionMode'], '', emissive, 'Mode'); g.link(P['P1_FieldGain'], '', emissive, 'Gain')
    camera = g.node(unreal.MaterialExpressionCameraVectorWS, -50, 1300)
    lobe = g.custom('FogMS_ForwardLobe', lobe_code, unreal.CustomMaterialOutputType.CMOT_FLOAT3, FORWARD_PINS, 250, 700)
    lobe_sources = {'Emissive': (emissive, ''), 'FieldA': (field, 'A'), 'Mode': (P['FogMS_InjectionMode'], ''),
                    'Strength': (P['FogMS_ForwardStrength'], ''), 'G': (P['FogMS_ForwardG'], ''), 'Depth': (P['FogMS_ForwardDepth'], ''),
                    'BackFloor': (P['FogMS_ForwardFloor'], ''), 'CameraVector': (camera, ''), 'Ecc': (P['FogMS_ForwardEcc'], '')}
    for pin in FORWARD_PINS:
        g.link(lobe_sources[pin][0], lobe_sources[pin][1], lobe, pin)
    sky = g.custom('P1_SkyAO', SKY_AO_CODE, unreal.CustomMaterialOutputType.CMOT_FLOAT1, ('FieldA', 'Mode'), 250, 1100)
    g.link(field, 'A', sky, 'FieldA'); g.link(P['FogMS_InjectionMode'], '', sky, 'Mode')
    cons = g.custom('P1_Conservative', CONSERVATIVE_CODE, unreal.CustomMaterialOutputType.CMOT_FLOAT3,
                    ('LocalPosition', 'HeightProfile', 'HeightBottom', 'HeightTop', 'Threshold', 'Softness', 'DetailStrength'), 250, -500)
    for pin, src in (('LocalPosition', local), ('HeightProfile', P['FogMS_HeightProfile']), ('HeightBottom', P['FogMS_HeightBottom']),
                     ('HeightTop', P['FogMS_HeightTop']), ('Threshold', P['FogMS_Threshold']), ('Softness', P['FogMS_Softness']),
                     ('DetailStrength', P['FogMS_DetailStrength'])):
        g.link(src, '', cons, pin)
    vao = g.node(unreal.MaterialExpressionVolumetricAdvancedMaterialOutput, 600, -300)
    set_node_bool(vao, ('ray_march_volume_shadow', 'bRayMarchVolumeShadow'), True)
    set_node_bool(vao, ('ground_contribution', 'bGroundContribution'), False)
    set_node_bool(vao, ('gray_scale_material', 'bGrayScaleMaterial'), False)
    set_node_bool(vao, ('clamp_multi_scattering_contribution', 'bClampMultiScatteringContribution'), True)
    vao.set_editor_property('multi_scattering_approximation_octave_count', 0)
    vao.set_editor_property('per_sample_phase_evaluation', False)
    names = list(mel.get_material_expression_input_names(vao))
    key = dict((str(n).replace(' ', '').lower(), str(n)) for n in names)
    for want, src in (('phaseg', P['P1_PhaseG']), ('phaseg2', P['P1_PhaseG2']), ('phaseblend', P['P1_PhaseBlend']), ('conservativedensity', cons)):
        require(want in key, 'Volumetric Advanced Output has no input like %s: %s' % (want, names))
        g.link(src, '', vao, key[want])
    for prop, src, out in (('MP_BASE_COLOR', P['FogMS_Albedo'], 'RGB'), ('MP_SUBSURFACE_COLOR', ext, ''), ('MP_EMISSIVE_COLOR', lobe, ''),
                           ('MP_AMBIENT_OCCLUSION', sky, '')):
        require(mel.connect_material_property(src, out, getattr(unreal.MaterialProperty, prop)), 'Cannot route ' + prop)
    types = dict(zip(mel.get_material_expression_input_names(lobe), mel.get_material_expression_input_types(lobe)))
    require(types.get('FieldA') not in (4, 8), 'Lobe FieldA is not one channel (value type %s)' % types.get('FieldA'))
    print('GRAPH %d expressions; VAO inputs %s; lobe FieldA value type %s (one channel)' % (
        len(mel.get_material_expressions(material)), names, types.get('FieldA')))


def editor_log_path():
    """The running editor's log file: -ABSLOG=<path>, -LOG=<name> (project log dir), else the newest *.log there."""
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
    failed = [i for i, line in enumerate(lines) if 'Failed to compile Material' in line and ASSET_NAME in line]
    if failed:
        return ['\n'.join(lines[i:i + 12]) for i in failed], 'shader (log %s)' % path
    return [], 'shader compiled (log %s)' % path


def main():
    extinction_code = matedit_code('EXTINCTION_CODE_V3')
    lobe_code = matedit_code('FORWARD_LOBE_CODE_V2')
    material = unreal.load_asset(MATERIAL_PATH) if unreal.EditorAssetLibrary.does_asset_exist(MATERIAL_PATH) else None
    if material is not None and is_current(material, extinction_code, lobe_code):
        print('ALREADY_PATCHED %s (%s)' % (MATERIAL_PATH, MARKER))
        return
    if material is None:
        tools = unreal.AssetToolsHelpers.get_asset_tools()
        material = tools.create_asset(ASSET_NAME, ASSET_DIR, unreal.Material, unreal.MaterialFactoryNew())
        require(material is not None, 'Cannot create ' + MATERIAL_PATH)
        print('CREATED', MATERIAL_PATH)
    else:
        print('REBUILD %s: graph differs from %s' % (MATERIAL_PATH, MARKER))
        mel.delete_all_material_expressions(material)
    # Blend mode before the domain: a new material is Opaque, and Volume + Opaque logs 'Failed to compile Material ... Volume
    # materials must use an Additive blend mode' for the intermediate state (seen in the first build, round 39).
    material.set_editor_property('blend_mode', unreal.BlendMode.BLEND_ADDITIVE)
    material.set_editor_property('shading_model', unreal.MaterialShadingModel.MSM_DEFAULT_LIT)
    material.set_editor_property('material_domain', unreal.MaterialDomain.MD_VOLUME)
    usage = getattr(mel, 'set_base_material_usage', None) or mel.set_material_usage
    usage(material, unreal.MaterialUsage.MATUSAGE_VOLUMETRIC_CLOUD)
    require(mel.has_material_usage(material, unreal.MaterialUsage.MATUSAGE_VOLUMETRIC_CLOUD), 'Used with Volumetric Cloud not set')
    build(material, extinction_code, lobe_code)
    errors, note = compile_and_check(material, 'FOGMS_CLOUDPROTO_%d' % int(time.time() * 1000))
    print('COMPILE cloud prototype:', note)
    require(not errors, 'Compile errors (asset NOT saved; reload it to discard): %s' % errors)
    require(is_current(material, extinction_code, lobe_code), 'Readback after build is not current')
    saved = unreal.EditorAssetLibrary.save_asset(MATERIAL_PATH, only_if_is_dirty=False)
    print('MATERIAL_OK saved=%s %s domain=%s blend=%s cloud_usage=%s' % (
        saved, MATERIAL_PATH, material.get_editor_property('material_domain'), material.get_editor_property('blend_mode'),
        mel.has_material_usage(material, unreal.MaterialUsage.MATUSAGE_VOLUMETRIC_CLOUD)))


try:
    main()
except Exception:
    print('TRACE', traceback.format_exc())
