"""Explicit native FSSS controls and metadata; importing this file does nothing.

Call run() through the editor bridge. No screenshots, map saves, actor creation,
camera changes, shader edits or asynchronous callbacks are performed.
"""
from __future__ import annotations

import datetime
import hashlib
import json
import math
import pathlib
import uuid


PROPERTY_CANDIDATES = {
    'enabled': ('bEnableFSSS', 'enable_fsss'),
    'scale': ('FSSSSceneColorScatteringAmountScale', 'fsss_scene_color_scattering_amount_scale'),
    'power': ('FSSSSceneColorScatteringAmountPower', 'fsss_scene_color_scattering_amount_power'),
    'spread': ('FSSSSpreadScale', 'fsss_spread_scale'),
    'blur': ('FSSSBlurControl', 'fsss_blur_control'),
}
CVAR_TARGET = {
    'r.Fog.ScreenSpaceScattering': 1,
    'r.Fog.SeparateComposition': -1,
    'r.Fog.ScreenSpaceScattering.TAA': 0,
}
DEFAULT_WORLD = '/Game/FogMS_Test/FogMS_Box.FogMS_Box'


def _same(a, b):
    """Float UPROPERTY round trips are permitted; identities and booleans are exact."""
    if isinstance(a, dict) and isinstance(b, dict):
        return a.keys() == b.keys() and all(_same(a[k], b[k]) for k in a)
    if isinstance(a, list) and isinstance(b, list):
        return len(a) == len(b) and all(_same(x, y) for x, y in zip(a, b))
    if isinstance(a, bool) or isinstance(b, bool):
        return type(a) is type(b) and a == b
    if isinstance(a, (int, float)) and isinstance(b, (int, float)):
        return math.isclose(a, b, rel_tol=1e-6, abs_tol=1e-7)
    return a == b


def _target(before, action, mode, settings):
    result = json.loads(json.dumps(before))
    if action == 'disable':
        result['properties']['enabled'] = False
        return result
    if mode not in ('separate_only', 'fog_only', 'fog_and_scene'):
        raise ValueError('mode must be separate_only, fog_only or fog_and_scene')
    values = {'scale': 0.0 if mode == 'fog_only' else 1.0,
              'power': 1.0, 'spread': 0.1, 'blur': 0.5}
    settings = dict(settings or {})
    if settings.keys() - values.keys():
        raise ValueError('Unknown FSSS setting: ' + str(sorted(settings.keys() - values.keys())))
    values.update(settings)
    for name, value in values.items():
        if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
            raise ValueError('Finite numeric setting required: ' + name)
        if value < 0 or (name == 'power' and value <= 0) or (name == 'blur' and value > 1):
            raise ValueError('Setting outside supported range: ' + name)
    if mode == 'fog_only' and values['scale'] != 0:
        raise ValueError('fog_only requires scale=0')
    result['properties'].update(values, enabled=(mode != 'separate_only'))
    result['cvars'].update(CVAR_TARGET)
    if mode == 'separate_only':
        result['cvars']['r.Fog.SeparateComposition'] = 1
    return result


def _write_new(path, value):
    with pathlib.Path(path).open('x', encoding='utf-8') as handle:
        json.dump(value, handle, indent=2, allow_nan=False)


def run(action, output_dir, *, expected_world=DEFAULT_WORLD, mode='fog_and_scene',
        settings=None, force_restore=False):
    """Return a durable controls-only receipt path.

    action: inspect (or probe), enable, disable, restore.
    output_dir: one absolute immutable-snapshot session directory.
    force_restore is only for an explicit recovery after inspecting control drift.
    No render-thread completion or visual quality is asserted by a PASS receipt.
    """
    import unreal
    import traceback

    if action not in ('inspect', 'probe', 'enable', 'disable', 'restore'):
        raise ValueError('Unsupported action')
    if force_restore and action != 'restore':
        raise ValueError('force_restore is only valid for restore')
    root = pathlib.Path(output_dir)
    if not root.is_absolute():
        raise ValueError('output_dir must be an absolute path')
    root.mkdir(parents=True, exist_ok=True)
    ident = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%S%fZ') + '-' + uuid.uuid4().hex[:8]
    receipt_path = root / ('receipt-' + ident + '.json')
    result = {'schema': 1, 'action': action, 'status': 'FAIL', 'mode': mode,
              'scope': 'Native component/CVar control metadata only; no GPU or visual acceptance',
              'cvar_priority_flags_restored': False,
              'cvar_priority_limit': 'Console writes restore numeric values only, not original SetBy flags',
              'force_restore': bool(force_restore), 'error': None}
    before = None
    mutation_started = False
    component = None
    resolved = {}

    def encode(value):
        if value is None or isinstance(value, (bool, float, int, str)):
            return value
        if isinstance(value, (list, tuple)):
            return [encode(v) for v in value]
        for fields in (('pitch', 'yaw', 'roll'), ('x', 'y', 'z')):
            if all(hasattr(value, k) for k in fields):
                return [float(getattr(value, k)) for k in fields]
        return str(value)

    def controls():
        return {'properties': {key: encode(component.get_editor_property(name)) for key, name in resolved.items()},
                'cvars': {key: int(unreal.SystemLibrary.get_console_variable_int_value(key)) for key in CVAR_TARGET}}

    def apply(values):
        # Reflected editor assignment invokes the native property-change path.
        # These FSSS fields have no public SetFSSS* UFUNCTION setters in UE 5.8.
        for key, value in values['properties'].items():
            if not _same(encode(component.get_editor_property(resolved[key])), value):
                component.set_editor_property(resolved[key], value)
        for key, value in values['cvars'].items():
            if int(unreal.SystemLibrary.get_console_variable_int_value(key)) != int(value):
                unreal.SystemLibrary.execute_console_command(world, key + ' ' + str(int(value)))

    try:
        levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
        if levels.is_in_play_in_editor():
            raise RuntimeError('PIE must be stopped before this editor control operation')
        world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
        result['world'] = world.get_path_name()
        if result['world'] != expected_world:
            raise RuntimeError('WORLD_MISMATCH: ' + result['world'])
        actor_api = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
        actors = actor_api.get_all_level_actors()
        candidates = [(a, c) for a in actors for c in a.get_components_by_class(unreal.ExponentialHeightFogComponent)]
        result['fog_candidates'] = [{'actor': a.get_path_name(), 'component': c.get_path_name()} for a, c in candidates]
        # Scene->ExponentialFogs[0] is authoritative. Actor enumeration order does
        # not expose that renderer order; a unique component is unambiguous.
        if len(candidates) != 1:
            raise RuntimeError('AMBIGUOUS_FIRST_HEIGHT_FOG: expected exactly one component, found ' + str(len(candidates)))
        actor, component = candidates[0]
        result['actor'] = actor.get_path_name()
        result['component'] = component.get_path_name()
        for key, names in PROPERTY_CANDIDATES.items():
            for name in names:
                try:
                    component.get_editor_property(name)
                    resolved[key] = name
                    break
                except Exception:
                    pass
            if key not in resolved:
                raise RuntimeError('MISSING_NATIVE_PROPERTY: ' + key)
        result['resolved_property_names'] = dict(resolved)
        viewport = levels.get_active_viewport_config_key()

        def scene_guard():
            return {'camera': encode(levels.get_level_viewport_camera_info(viewport)),
                    'fov': encode(levels.get_level_viewport_fov(viewport)),
                    'game_view': bool(levels.editor_get_game_view(viewport)),
                    'actors': sorted([[a.get_path_name(), encode(a.get_actor_location()),
                                       encode(a.get_actor_rotation()), encode(a.get_actor_scale3d()), a.is_hidden_ed()]
                                      for a in actor_api.get_all_level_actors()])}

        package = result['world'].split('.')[0]
        if not package.startswith('/Game/'):
            raise RuntimeError('A saved /Game/ map is required')
        map_file = pathlib.Path(unreal.Paths.project_dir()) / 'Content' / (package[6:] + '.umap')
        map_hash = hashlib.sha256(map_file.read_bytes()).hexdigest()
        guard_before = scene_guard()
        before = controls()
        result['before'] = before
        result['guard_before'] = guard_before
        result['map_sha256_before'] = map_hash
        result['metadata'] = {
            'forward_shading': unreal.SystemLibrary.get_console_variable_int_value('r.ForwardShading'),
            'fog_enabled': unreal.SystemLibrary.get_console_variable_int_value('r.Fog'),
            'max_exposed_luminance': unreal.SystemLibrary.get_console_variable_float_value('r.Fog.ScreenSpaceScattering.MaxExposedLuminance'),
            'actor_hidden_editor': bool(actor.is_hidden_ed()),
            'component_visible': bool(component.get_editor_property('visible')),
            'frame': int(unreal.SystemLibrary.get_frame_count()),
            'render_support_verified': False,
        }
        original_path = root / 'original.json'
        original = json.loads(original_path.read_text(encoding='utf-8')) if original_path.exists() else None
        if original and (original['world'] != result['world'] or original['component'] != result['component']):
            raise RuntimeError('SNAPSHOT_TARGET_MISMATCH')
        if action in ('inspect', 'probe'):
            result['after'] = before
            result['status'] = 'OBSERVED'
        else:
            if action == 'restore' and original is None:
                raise RuntimeError('No original snapshot exists for restore')
            previous = []
            for file in root.glob('receipt-*.json'):
                prior = json.loads(file.read_text(encoding='utf-8'))
                if prior.get('status') == 'PASS' and prior.get('action') in ('enable', 'disable', 'restore'):
                    previous.append((file.name, prior))
            expected = sorted(previous)[-1][1]['after'] if previous else (original['controls'] if original else before)
            if not force_restore and not _same(before, expected):
                raise RuntimeError('CONTROL_STATE_DIVERGED: inspect before explicit recovery; no mutation performed')
            desired = original['controls'] if action == 'restore' else _target(before, action, mode, settings)
            result['requested'] = desired
            if original is None:
                original = {'schema': 1, 'world': result['world'], 'actor': result['actor'],
                            'component': result['component'], 'controls': before,
                            'resolved_property_names': dict(resolved), 'map_sha256': map_hash,
                            'guard': guard_before, 'created_utc': ident}
                _write_new(original_path, original)
            # Unique intent survives a crash before the final receipt. Neither it
            # nor original.json is ever rewritten by this harness.
            _write_new(root / ('intent-' + ident + '.json'),
                       {'action': action, 'before': before, 'requested': desired,
                        'world': result['world'], 'component': result['component']})
            mutation_started = True
            apply(desired)
            result['after'] = controls()
            if not _same(result['after'], desired):
                raise RuntimeError('CONTROL_READBACK_MISMATCH')
            result['status'] = 'PASS'
        result['guard_after'] = scene_guard()
        result['map_sha256_after'] = hashlib.sha256(map_file.read_bytes()).hexdigest()
        result['scene_unchanged'] = _same(guard_before, result['guard_after'])
        result['map_unsaved'] = map_hash == result['map_sha256_after']
        if not result['scene_unchanged'] or not result['map_unsaved']:
            raise RuntimeError('SCENE_OR_MAP_CHANGED_DURING_CONTROL_OPERATION')
    except BaseException:
        result['status'] = 'FAIL'
        result['error'] = traceback.format_exc()
        if mutation_started and before is not None:
            try:
                apply(before)
                result['rollback_after'] = controls()
                result['rollback_controls_restored'] = _same(result['rollback_after'], before)
            except BaseException:
                result['rollback_controls_restored'] = False
                result['rollback_error'] = traceback.format_exc()
    _write_new(receipt_path, result)
    unreal.log('FOGMS_NATIVE_FSSS ' + result['status'] + ' ' + str(receipt_path))
    if result['status'] == 'FAIL':
        raise RuntimeError('Native FSSS control failed; retained receipt: ' + str(receipt_path))
    return str(receipt_path)
