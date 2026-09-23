"""FogMS S1 (edge erosion) + S2 (height profile): patch /MultiLobeSpec/FogMS/M_FogMS_Density in place.

Run in the editor (Python console or `py <this file>`), with no other session editing the material:
  1. creates the new scalar/vector parameters (defaults = identity: erosion off, profile off);
  2. creates a NEW Custom node, description 'FogMS_Extinction', code EXTINCTION_CODE_V2, whose inputs are the
     inputs of the old extinction node MaterialExpressionCustom_3 (same source expressions and output pins,
     found by walking Custom_3's inputs) plus the new parameters;
  3. re-routes every consumer of Custom_3's output (material properties such as MP_SUBSURFACE_COLOR = extinction,
     and expression pins such as the injection node's 'Extinction') to the new node;
  4. leaves Custom_3 in the graph with its inputs but without any consumer (restore = re-route back);
  5. recompiles and saves; on compile errors it re-routes back, deletes what it created and does not save.
Idempotent: a Custom node with description 'FogMS_Extinction' means already patched (prints a summary only).

EXTINCTION_CODE_V2 must stay formula-identical to FogMS_IndirectLocalDensity in Shaders/Private/FogMS_Indirect.ush;
the .ush carries a verbatim copy between 'BEGIN EXTINCTION_CODE_V2' / 'END EXTINCTION_CODE_V2'. When the .ush is
found next to this script (repo layout), a mismatch aborts before any change.
Default identity: HeightProfile 0 gives P = 1 (n * 1 == n) and ErosionStrength 0 skips the erosion block, so the
defaults evaluate exactly the V1 formula.
"""
import os
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

mel = unreal.MaterialEditingLibrary


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def ush_sync_check():
    """Compare EXTINCTION_CODE_V2 with the verbatim copy in the .ush (repo layout only)."""
    here = globals().get('__file__')
    if not here:
        return 'SKIPPED (no __file__)'
    path = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(here)), '..', '..', '..',
                                         'Shaders', 'Private', 'FogMS_Indirect.ush'))
    if not os.path.isfile(path):
        return 'SKIPPED (%s not found)' % path
    with open(path, 'r', encoding='utf-8') as f:
        text = f.read().replace('\r\n', '\n')
    begin, end = 'BEGIN EXTINCTION_CODE_V2\n', '\nEND EXTINCTION_CODE_V2'
    require(begin in text and end in text, 'The .ush has no EXTINCTION_CODE_V2 markers: ' + path)
    copy = text.split(begin, 1)[1].split(end, 1)[0]
    require(copy.strip('\n') == EXTINCTION_CODE_V2.strip('\n'),
            'EXTINCTION_CODE_V2 differs from the copy in ' + path + ': update both together.')
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


def find_old_node(exprs):
    by_name = [e for e in exprs if e.get_name() == OLD_NODE_NAME]
    candidates = by_name or [e for e in custom_nodes(exprs)
                             if EXTINCTION_CODE_V1_TAIL in str(e.get_editor_property('code'))
                             and str(e.get_editor_property('description')) != NEW_DESCRIPTION]
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
    new = find_new_node(exprs)
    print('FOGMS_DENSITY material=%s nodes=%d FogMS_Extinction=%s' % (
        MATERIAL_PATH, len(exprs), new.get_name() if new else None))
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


def main():
    material = unreal.load_asset(MATERIAL_PATH)
    require(material is not None and isinstance(material, unreal.Material), 'Material not found: ' + MATERIAL_PATH)
    exprs = expressions(material)
    if find_new_node(exprs):
        print('ALREADY_PATCHED')
        summary(material, exprs)
        return
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

    for name, default, _ in NEW_SCALARS:
        value = mel.get_material_default_scalar_parameter_value(material, name)
        require(abs(value - default) < 1e-6, 'Default readback %s = %s' % (name, value))
    saved = unreal.EditorAssetLibrary.save_asset(MATERIAL_PATH, only_if_is_dirty=False)
    print('PATCHED saved=%s' % saved)
    summary(material, expressions(material))


try:
    main()
except Exception:
    print('TRACE', traceback.format_exc())
