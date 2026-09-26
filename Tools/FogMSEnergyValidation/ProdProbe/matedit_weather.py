"""FogMS W48 (FogMS_Weather_Design.md 3.3, 3.6, 5.3-5.4, 6 'W48'): the plugin's weather assets in /MultiLobeSpec/FogMS/Weather, created or
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
SUN_OD_BODY = r"""// FogMS_WeatherSunOD v1 (W48 thin layer, matedit_weather.py): one texel of RT_FogMS_WeatherSun (R16F over the weather domain,
// wind-free like RT_FogMS_WeatherMap: the host shifts both lookups by the wind) = the optical depth [-] of every weather layer along the
// sun line through the texel's ground point G (altitude 0):
//   OD(G) = sum over N = 24 midpoint altitudes of [Bottom, Top] of the active layers of
//           FogMSWeatherFn.Sigma(G + SunDir.xy * Alt / SunDir.z, Alt) * dAlt / SunDir.z * 0.01  (sigma [1/m], lengths [cm])
// (flat ground: over a layer's path the planet's curvature is below a metre). SunDir.xyz = toward the atmosphere sun, z clamped to
// >= 0.05 (a sun below 3 degrees is treated as 3 degrees).
FogMSWeatherFn W;
float Up = max(SunDir.z, 0.05f);
float2 G = UV / Domain.x;
float Bottom = 1.0e9f;
float Top = -1.0e9f;
if (L0.z > 0.0f) { Bottom = min(Bottom, L0.x); Top = max(Top, L0.y); }
if (L1.z > 0.0f) { Bottom = min(Bottom, L1.x); Top = max(Top, L1.y); }
if (Top <= Bottom) return float3(0.0f, 0.0f, 0.0f);
const int N = 24;
float dAlt = (Top - Bottom) / N;
float OD = 0.0f;
for (int i = 0; i < N; ++i)
{
    float Alt = Bottom + (i + 0.5f) * dAlt;
    OD += W.Sigma(G + SunDir.xy * (Alt / Up), Alt, Domain, L0, L1, Map, MapSampler, TypeLUT, TypeLUTSampler, Pattern, PatternSampler, Curl, CurlSampler);
}
return float3(OD * dAlt / Up * 0.01f, 0.0f, 0.0f);
"""
COMPOSE_PINS = ('UV', 'Pattern', 'P0', 'P1')
SUN_PINS = ('UV', 'Domain', 'L0', 'L1', 'SunDir', 'Map', 'TypeLUT', 'Pattern', 'Curl')
SUN_VECTORS = (('FogMS_WeatherDomain', (5.0e-7, 4.0e-6, 0.05, 0.0)), ('FogMS_WeatherL0', (0.0, 0.0, 0.0, 0.0)),
               ('FogMS_WeatherL1', (0.0, 0.0, 0.0, 0.0)), ('FogMS_WeatherSunDir', (0.0, 0.0, 1.0, 0.0)))
COMPOSE_VECTORS = (('FogMS_WeatherComposeP0', (0.0, 0.5, 0.04, 0.0)), ('FogMS_WeatherComposeP1', (0.0, 0.0, 0.0, 0.0)))


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


class Graph:
    def __init__(self, material):
        self.m = material
        self.params = {}
        self.links = {}

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
        require(isinstance(dst, unreal.MaterialExpressionCustom), 'only Custom destinations are wired through Graph.link here')
        self.links.setdefault(dst, {})[pin] = (src, out if out else (outs[0] if outs else ''))

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

    def vector(self, name, default, x, y):
        e = self.node(unreal.MaterialExpressionVectorParameter, x, y)
        e.set_editor_property('parameter_name', name)
        e.set_editor_property('default_value', unreal.LinearColor(*default))
        e.set_editor_property('group', GROUP)
        self.params[name] = e
        return e

    def texture(self, name, path, x, y):
        e = self.node(unreal.MaterialExpressionTextureObjectParameter, x, y)
        e.set_editor_property('parameter_name', name)
        e.set_editor_property('group', GROUP)
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


def material_settings(material, blend):
    material.set_editor_property('blend_mode', blend)
    material.set_editor_property('shading_model', unreal.MaterialShadingModel.MSM_UNLIT)
    material.set_editor_property('material_domain', unreal.MaterialDomain.MD_SURFACE)
    material.set_editor_property('use_translucency_vertex_fog', False)


def verify_material(material, kind, sun_code, g=None):
    """[] when current. kind 'compose' | 'sun'."""
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


def ensure_material(path, name, kind, sun_code, report_only):
    """Returns (status, detail): 'current' | 'built' | 'report' | 'failed'."""
    material = unreal.load_asset(path) if unreal.EditorAssetLibrary.does_asset_exist(path) else None
    problems = verify_material(material, kind, sun_code) if material is not None else ['missing']
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
        material_settings(material, unreal.BlendMode.BLEND_ALPHA_COMPOSITE if kind == 'compose' else unreal.BlendMode.BLEND_OPAQUE)
        g = build_compose(material) if kind == 'compose' else build_sun(material, sun_code)
        errors, note = compile_and_check(material, 'FOGMS_MATEDIT_WEATHER_%s_%d' % (kind.upper(), int(time.time() * 1000)))
        print('COMPILE %s: %s' % (name, note))
        require(not errors, 'Compile errors: %s' % errors)
        after = verify_material(material, kind, sun_code, g)
        require(not after, 'Readback after build: %s' % after)
        after = verify_material(material, kind, sun_code)
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
            out.append('%s %s' % (name, 'present' if got == want else 'LEFT AS IS (preset %s, edited by hand?)' % got))
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
    sun_code = cloud_constant('WEATHER_FN_CODE') + SUN_OD_BODY
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
    # 2./3. materials
    for path, name, kind in ((COMPOSE_PATH, COMPOSE_NAME, 'compose'), (SUN_PATH, SUN_NAME, 'sun')):
        status, detail = ensure_material(path, name, kind, sun_code, report_only)
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
        print(('WEATHER_OK' if changed else 'ALREADY_PATCHED') + ' %s (%s)' % (WEATHER_DIR, MARKER))


try:
    main()
except Exception:
    print('TRACE', traceback.format_exc())
