"""Explicit native CPU/MID contract probe; running the script calls run().

No camera, density-shape, quality, map, asset or preference changes. Each run
writes a unique durable receipt before mutation and restores plain saved values.
This checks actor phases/velocities, not a rendered GPU image or packet readback.
"""
from __future__ import annotations


def run(output_root='E:/GITHUB/MultiLobeSpec/.codex-build/FogMS_Motion_20260921'):
    import datetime
    import json
    import pathlib
    import traceback
    import unreal

    root = pathlib.Path(output_root)
    root.mkdir(parents=True, exist_ok=True)
    output = root / ('edge-flow-' + datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f') + '.json')
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    if world.get_path_name() != '/Game/FogMS_Test/FogMS_Box.FogMS_Box':
        raise RuntimeError('Load the existing FogMS_Box test map before this explicit probe')
    boxes = [a for a in unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()
             if a.get_class().get_name() == 'FogMSBoxVolume']
    if len(boxes) != 1:
        raise RuntimeError('Expected exactly one existing FogMSBoxVolume')
    box = boxes[0]
    for name in ('use_directional_motion', 'use_legacy_motion', 'reset_motion_origin',
                 'restore_density_motion_reference', 'freeze_density_animation', 'resume_density_animation'):
        if not callable(getattr(box, name, None)):
            raise RuntimeError('Missing required public API before mutation: ' + name)
    names = ('animate_density', 'use_manual_animation_time', 'manual_animation_time', 'animation_time_offset',
             'density_wind_velocity', 'density_detail_velocity', 'density_evolution_velocity',
             'wind_speed', 'edge_flow_speed', 'density_motion_mode', 'density_motion_reference')
    reference_names = ('initialized', 'time', 'displacement0', 'displacement1', 'displacement2',
                       'velocity0', 'velocity1', 'velocity2')

    def encode(value):
        if isinstance(value, (bool, float, int, str)):
            return value
        for fields in (('pitch', 'yaw', 'roll'), ('x', 'y', 'z')):
            if all(hasattr(value, f) for f in fields):
                return [float(getattr(value, f)) for f in fields]
        if isinstance(value, unreal.FogMSDensityMotionReference):
            return {k: encode(value.get_editor_property(k)) for k in reference_names}
        return str(value)

    def properties():
        return {k: encode(box.get_editor_property(k)) for k in names}

    def arrow():
        component = box.get_editor_property('wind_direction_component')
        return {'path': component.get_path_name(),
                'relative_rotation': encode(component.get_editor_property('relative_rotation')),
                'absolute_location': bool(component.get_editor_property('absolute_location')),
                'absolute_rotation': bool(component.get_editor_property('absolute_rotation')),
                'absolute_scale': bool(component.get_editor_property('absolute_scale'))}

    def bounds():
        return {'location': encode(box.get_actor_location()), 'rotation': encode(box.get_actor_rotation()),
                'scale': encode(box.get_actor_scale3d())}

    def phases():
        mid = box.get_editor_property('density_component').get_material(0)
        return [[float(getattr(mid.get_vector_parameter_value('FogMS_WorldPhase' + str(i)), axis))
                 for axis in ('r', 'g', 'b')] for i in range(3)]

    def phase_delta(a, b):
        return max(abs((x-y+.5) % 1.-.5) for av, bv in zip(a, b) for x, y in zip(av, bv))

    def vector_error(a, b):
        return max(abs(x-y) for x, y in zip(a, b))

    def add(*vectors):
        return [sum(v[i] for v in vectors) for i in range(3)]

    def mul(v, s):
        return [x*s for x in v]

    def reference():
        return encode(box.get_editor_property('density_motion_reference'))

    def expected_velocities(flow):
        component = box.get_editor_property('wind_direction_component')
        v0 = mul(encode(component.get_forward_vector()), float(box.get_editor_property('wind_speed')))
        manual1 = encode(box.get_editor_property('density_detail_velocity'))
        manual2 = encode(box.get_editor_property('density_evolution_velocity'))
        return [v0, add(v0, manual1, mul(encode(component.get_right_vector()), flow)),
                add(v0, manual1, manual2, mul(encode(component.get_up_vector()), -.5*flow))]

    def setp(**values):
        for name, value in values.items():
            box.set_editor_property(name, value)
        box.update_density()

    # Python reflected struct wrappers alias their UPROPERTY. Keep only plain
    # values, including the arrow's original relative rotator, before mutation.
    original = properties()
    original_arrow = arrow()
    original_bounds = bounds()
    original_phase = phases()
    data = {'status': 'RUNNING', 'world': world.get_path_name(), 'actor': box.get_path_name(),
            'original': original, 'original_arrow': original_arrow, 'original_bounds': original_bounds,
            'checks': {}, 'samples': {}, 'scope': 'Native actor CPU velocity/reference and MID phase contracts only'}
    with output.open('x', encoding='utf-8') as handle:
        json.dump(data, handle, indent=2)

    def write():
        output.write_text(json.dumps(data, indent=2), encoding='utf-8')

    def check(name, condition):
        data['checks'][name] = bool(condition)
        write()
        if not condition:
            raise AssertionError(name)

    def sample(name):
        data['samples'][name] = {'properties': properties(), 'phases': phases(), 'arrow': arrow(),
                                 'status': str(box.get_editor_property('density_animation_status'))}
        write()

    def check_velocities(name, flow):
        actual = reference()
        expected = expected_velocities(flow)
        errors = [vector_error(actual['velocity'+str(i)], expected[i]) for i in range(3)]
        data.setdefault('velocity_errors', {})[name] = errors
        check(name, max(errors) < 1.e-9)

    try:
        check('new_actor_default_flow_zero', float(unreal.get_default_object(box.get_class()).get_editor_property('edge_flow_speed')) == 0.)
        setp(animate_density=False, use_manual_animation_time=True, manual_animation_time=10000.,
             animation_time_offset=0., edge_flow_speed=0.)
        box.use_directional_motion()
        setp(wind_speed=125., density_detail_velocity=unreal.Vector(1., 2., 3.),
             density_evolution_velocity=unreal.Vector(-4., 5., -6.))
        component = box.get_editor_property('wind_direction_component')
        component.set_world_rotation(unreal.Rotator(pitch=0., yaw=0., roll=0.), False, False)
        box.update_density()
        disabled_phase = phases()
        setp(edge_flow_speed=11.)
        check('disabled_animation_unchanged_by_flow', phase_delta(disabled_phase, phases()) == 0.)
        setp(edge_flow_speed=0., animate_density=True)
        box.reset_motion_origin()
        box.update_density()
        check('animation_active', 'Static:' not in str(box.get_editor_property('density_animation_status'))
              and reference()['initialized'])
        check_velocities('flow_zero_matches_original_cumulative_velocity', 0.)
        start_phase = phases()
        base_velocity = reference()['velocity0']
        setp(edge_flow_speed=11.)
        sample('flow_enabled_at_manual_10000')
        check('enable_flow_preserves_phase', phase_delta(start_phase, phases()) == 0.)
        check('common_wind_unchanged', reference()['velocity0'] == base_velocity)
        check_velocities('flow_expected_with_nonzero_advanced_vectors', 11.)
        setp(edge_flow_speed=23.)
        check('flow_speed_edit_preserves_phase', phase_delta(start_phase, phases()) == 0.)
        check_velocities('edited_flow_expected_velocities', 23.)
        component = box.get_editor_property('wind_direction_component')
        component.set_world_rotation(unreal.Rotator(pitch=15., yaw=65., roll=10.), False, False)
        box.update_density()
        sample('arrow_rotated_at_manual_10000')
        check('arrow_edit_preserves_phase', phase_delta(start_phase, phases()) == 0.)
        check_velocities('rotated_arrow_expected_velocities', 23.)
        before_invalid = reference()
        setp(edge_flow_speed=-1.)
        sample('negative_flow_rejected')
        check('negative_flow_static_status', 'nonnegative' in str(box.get_editor_property('density_animation_status')))
        check('invalid_flow_preserves_reference', reference() == before_invalid)
        setp(edge_flow_speed=23.)
        check('valid_flow_recovery_preserves_phase', phase_delta(start_phase, phases()) == 0.)
        setp(manual_animation_time=10000.25)
        moved_phase = phases()
        check('detail_phases_advance_with_time', phase_delta(start_phase[1:], moved_phase[1:]) > 1.e-7)
        setp(edge_flow_speed=0.)
        check('flow_disable_preserves_current_phase', phase_delta(moved_phase, phases()) == 0.)
        check_velocities('flow_disable_restores_original_velocity_formula', 0.)
        setp(edge_flow_speed=13.)
        frozen_phase = phases()
        box.freeze_density_animation()
        box.update_density()
        check('freeze_preserves_phase', phase_delta(frozen_phase, phases()) < 1.e-7)
        box.resume_density_animation()
        box.update_density()
        check('resume_preserves_phase', phase_delta(frozen_phase, phases()) < 1.e-7)
        # Legacy's original absolute-time formula ignores the new property,
        # including validation of a value irrelevant to this mode.
        setp(use_manual_animation_time=True, manual_animation_time=10000., animation_time_offset=0.,
             density_wind_velocity=unreal.Vector(31., -7., 2.), edge_flow_speed=0.)
        box.use_legacy_motion()
        box.update_density()
        legacy_phase = phases()
        setp(edge_flow_speed=23.)
        check('legacy_ignores_nonzero_flow', phase_delta(legacy_phase, phases()) == 0.)
        setp(edge_flow_speed=-1.)
        check('legacy_ignores_invalid_irrelevant_flow', phase_delta(legacy_phase, phases()) == 0.
              and 'Static:' not in str(box.get_editor_property('density_animation_status')))
        sample('legacy_parity')
        data['status'] = 'PASS'
    except BaseException:
        data['status'] = 'FAIL'
        data['error'] = traceback.format_exc()
    finally:
        try:
            box.set_editor_property('animate_density', False)
            if 'LEGACY_VECTORS' in original['density_motion_mode']:
                box.use_legacy_motion()
            else:
                box.use_directional_motion()
            for name, value in original.items():
                if name not in ('animate_density', 'density_motion_mode', 'density_motion_reference'):
                    box.set_editor_property(name, unreal.Vector(*value) if isinstance(value, list) else value)
            pitch, yaw, roll = original_arrow['relative_rotation']
            component = box.get_editor_property('wind_direction_component')
            component.set_editor_property('relative_rotation', unreal.Rotator(pitch=pitch, yaw=yaw, roll=roll))
            box.set_editor_property('animate_density', original['animate_density'])
            restored_reference = unreal.FogMSDensityMotionReference()
            for name, value in original['density_motion_reference'].items():
                restored_reference.set_editor_property(name, unreal.Vector(*value) if isinstance(value, list) else value)
            if not box.restore_density_motion_reference(restored_reference):
                raise RuntimeError('Reference restore rejected')
            box.update_density()
            actual = properties()
            data['actual_after_restore'] = actual
            data['arrow_after_restore'] = arrow()
            data['bounds_after_restore'] = bounds()
            checks = {name: actual[name] == original[name] for name in names}
            checks['arrow_exact'] = arrow() == original_arrow
            checks['bounds_exact'] = bounds() == original_bounds
            if not original['animate_density']:
                checks['original_static_phase_exact'] = phase_delta(original_phase, phases()) == 0.
            data['restore_checks'] = checks
            data['restore_differences'] = {name: {'expected': original[name], 'actual': actual[name]}
                                           for name in names if actual[name] != original[name]}
            data['restored'] = all(checks.values())
            if not data['restored']:
                data['status'] = 'RESTORE_FAILED'
        except BaseException:
            data['status'] = 'RESTORE_FAILED'
            data['restore_error'] = traceback.format_exc()
        write()
        unreal.log('FOGMS_EDGE_FLOW_PROBE ' + str(output) + ' ' + data['status'])
    return str(output)


if __name__ == '__main__':
    run()
