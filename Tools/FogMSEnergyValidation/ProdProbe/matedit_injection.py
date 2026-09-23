import unreal, traceback
try:
    mel = unreal.MaterialEditingLibrary
    m = unreal.load_asset('/MultiLobeSpec/FogMS/M_FogMS_Density')
    exprs = list(mel.get_material_expressions(m)) if hasattr(mel, 'get_material_expressions') else list(m.get_editor_property('expressions'))
    byname = {e.get_name(): e for e in exprs}
    def param(name):
        for e in exprs:
            try:
                if str(e.get_editor_property('parameter_name')) == name: return e
            except Exception: pass
        return None
    if param('FogMS_InjectionMode'):
        print('ALREADY_PATCHED')
    else:
        local_pos = byname['MaterialExpressionTransformPosition_0']
        density = byname['MaterialExpressionCustom_3']
        albedo = param('FogMS_Albedo'); zero = param('FogMS_ZeroEmission'); noise = param('FogMS_Noise')
        assert local_pos and density and albedo and zero and noise
        default_vt = noise.get_editor_property('texture')
        print('placeholder volume texture:', default_vt.get_path_name() if default_vt else None)
        inj = mel.create_material_expression(m, unreal.MaterialExpressionScalarParameter, 120, 260)
        inj.set_editor_property('parameter_name', 'FogMS_InjectionMode'); inj.set_editor_property('default_value', 0.0)
        inj.set_editor_property('group', 'FogMS')
        uvw = mel.create_material_expression(m, unreal.MaterialExpressionCustom, -1300, 900)
        uvw.set_editor_property('code', '// Box-local cube space is -50..50; transport field texel (x,y,z) = Box-local cell (x,y,z), so uvw = local01.\nreturn LocalPosition * 0.01f + 0.5f;')
        uvw.set_editor_property('output_type', unreal.CustomMaterialOutputType.CMOT_FLOAT3)
        uvw.set_editor_property('description', 'FogMS_TransportUVW')
        ci = unreal.CustomInput(); ci.set_editor_property('input_name', 'LocalPosition'); uvw.set_editor_property('inputs', [ci])
        assert mel.connect_material_expressions(local_pos, '', uvw, 'LocalPosition'), 'connect localpos'
        tex = mel.create_material_expression(m, unreal.MaterialExpressionTextureSampleParameterVolume, -1050, 900)
        tex.set_editor_property('parameter_name', 'FogMS_TransportField'); tex.set_editor_property('group', 'FogMS')
        tex.set_editor_property('sampler_type', unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR)
        if default_vt: tex.set_editor_property('texture', default_vt)
        ok = mel.connect_material_expressions(uvw, '', tex, 'UVs') or mel.connect_material_expressions(uvw, '', tex, 'Coordinates')
        assert ok, 'connect uvw->tex'
        emi = mel.create_material_expression(m, unreal.MaterialExpressionCustom, 420, 120)
        emi.set_editor_property('code', '// Emissive injection: native voxelization scales by 1/100, so sigma_s[1/m] * J lands as radiance per cm in VBufferB.\nreturn Zero.xxx + saturate(Mode) * Field * Albedo * max(Extinction, 0.0f);')
        emi.set_editor_property('output_type', unreal.CustomMaterialOutputType.CMOT_FLOAT3)
        emi.set_editor_property('description', 'FogMS_EmissiveInjection')
        ins = []
        for n in ('Field', 'Albedo', 'Extinction', 'Mode', 'Zero'):
            c = unreal.CustomInput(); c.set_editor_property('input_name', n); ins.append(c)
        emi.set_editor_property('inputs', ins)
        assert mel.connect_material_expressions(tex, 'RGB', emi, 'Field'), 'field'
        assert mel.connect_material_expressions(albedo, 'RGB', emi, 'Albedo'), 'albedo'
        assert mel.connect_material_expressions(density, '', emi, 'Extinction'), 'ext'
        assert mel.connect_material_expressions(inj, '', emi, 'Mode'), 'mode'
        assert mel.connect_material_expressions(zero, '', emi, 'Zero'), 'zero'
        assert mel.connect_material_property(emi, '', unreal.MaterialProperty.MP_EMISSIVE_COLOR), 'emissive prop'
        bc = mel.create_material_expression(m, unreal.MaterialExpressionCustom, 420, -200)
        bc.set_editor_property('code', '// Injection mode: albedo 0 so the native path adds no single scattering for this Box; extinction still occludes.\nreturn Albedo * (1.0f - saturate(Mode));')
        bc.set_editor_property('output_type', unreal.CustomMaterialOutputType.CMOT_FLOAT3)
        bc.set_editor_property('description', 'FogMS_InjectionAlbedo')
        ins = []
        for n in ('Albedo', 'Mode'):
            c = unreal.CustomInput(); c.set_editor_property('input_name', n); ins.append(c)
        bc.set_editor_property('inputs', ins)
        assert mel.connect_material_expressions(albedo, 'RGB', bc, 'Albedo'), 'bc albedo'
        assert mel.connect_material_expressions(inj, '', bc, 'Mode'), 'bc mode'
        assert mel.connect_material_property(bc, '', unreal.MaterialProperty.MP_BASE_COLOR), 'basecolor prop'
        mel.recompile_material(m)
        saved = unreal.EditorAssetLibrary.save_asset('/MultiLobeSpec/FogMS/M_FogMS_Density', only_if_is_dirty=False)
        print('PATCHED saved=%s' % saved)
    for prop in ('MP_BASE_COLOR', 'MP_EMISSIVE_COLOR', 'MP_SUBSURFACE_COLOR'):
        src = mel.get_material_property_input_node(m, getattr(unreal.MaterialProperty, prop))
        print('OUTPUT', prop, '<-', src.get_name() if src else None)
except Exception:
    print('TRACE', traceback.format_exc())
