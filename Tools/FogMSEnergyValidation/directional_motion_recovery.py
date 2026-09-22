"""Explicit receipt inspection/recovery. Running the file is READ ONLY for UE.

For mutation call run(restore=True) explicitly after inspecting the read receipt.
The source receipt is never rewritten; each invocation emits a fresh report.
No map save, camera edits, renderer changes, or implicit startup execution.
"""

SOURCE_RECEIPT = 'E:/GITHUB/MultiLobeSpec/.codex-build/FogMS_Motion_20260921/directional-223215-365936.json'
REFERENCE_FIELDS = ('initialized', 'time', 'displacement0', 'displacement1',
                    'displacement2', 'velocity0', 'velocity1', 'velocity2')


def differences(expected, actual, prefix=''):
    result = {}
    if isinstance(expected, dict) and isinstance(actual, dict):
        for key in sorted(set(expected) | set(actual)):
            path = prefix + '.' + key if prefix else key
            result.update(differences(expected.get(key), actual.get(key), path))
    elif expected != actual:
        entry = {'expected': expected, 'actual': actual}
        if isinstance(expected, list) and isinstance(actual, list) and len(expected) == len(actual):
            if all(isinstance(x, (int, float)) for x in expected + actual):
                entry['max_absolute_delta'] = max((abs(x-y) for x,y in zip(expected,actual)), default=0.)
        elif isinstance(expected, (int, float)) and isinstance(actual, (int, float)):
            entry['absolute_delta'] = abs(expected-actual)
        result[prefix] = entry
    return result


def run(restore=False, source_receipt=SOURCE_RECEIPT):
    import datetime
    import hashlib
    import json
    import pathlib
    import traceback
    import unreal

    source = pathlib.Path(source_receipt)
    source_bytes = source.read_bytes()
    receipt = json.loads(source_bytes)
    original = receipt['original']
    expected_arrow = receipt['original_arrow']
    expected_actor_path = expected_arrow['path'].rsplit('.', 1)[0]
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()
    matches = [a for a in actors if a.get_path_name() == expected_actor_path]
    if len(matches) != 1 or matches[0].get_class().get_name() != 'FogMSBoxVolume':
        raise RuntimeError('Exact receipt actor is not loaded; refusing another actor: '+expected_actor_path)
    box = matches[0]
    if restore and not callable(getattr(box, 'restore_density_motion_reference', None)):
        raise RuntimeError('Missing motion reference restore API; no changes made')

    def encode(value):
        if isinstance(value, (bool, float, int, str)):
            return value
        for fields in [('pitch','yaw','roll'), ('x','y','z')]:
            if all(hasattr(value, f) for f in fields):
                return [float(getattr(value, f)) for f in fields]
        if isinstance(value, unreal.FogMSDensityMotionReference):
            return {k:encode(value.get_editor_property(k)) for k in REFERENCE_FIELDS}
        return str(value)

    def snapshot():
        wind = box.get_editor_property('wind_direction_component')
        return {'properties': {k:encode(box.get_editor_property(k)) for k in original},
                'arrow': {'path': wind.get_path_name(), 'class': wind.get_class().get_name(),
                          'rotation': encode(wind.get_world_rotation()),
                          'absolute_location': bool(wind.get_editor_property('absolute_location')),
                          'absolute_rotation': bool(wind.get_editor_property('absolute_rotation')),
                          'absolute_scale': bool(wind.get_editor_property('absolute_scale'))}}

    expected = {'properties': original, 'arrow': expected_arrow}
    before = snapshot()
    stamp = datetime.datetime.now().strftime('%H%M%S-%f')
    output = source.parent / ('directional-recovery-'+stamp+'.json')
    report = {'status': 'RUNNING', 'mode': 'RESTORE' if restore else 'READ_ONLY',
              'source_receipt': str(source), 'source_sha256': hashlib.sha256(source_bytes).hexdigest(),
              'before': before, 'differences_before': differences(expected, before)}
    # Durable snapshot is created before the first allowed mutation.
    with output.open('x', encoding='utf-8') as handle:
        json.dump(report, handle, indent=2)
    try:
        if restore:
            # Recreate values from immutable JSON, never reuse Unreal wrappers.
            box.set_editor_property('animate_density', False)
            if 'LEGACY_VECTORS' in original['density_motion_mode']:
                box.use_legacy_motion()
            elif 'DIRECTIONAL' in original['density_motion_mode']:
                box.use_directional_motion()
            else:
                raise RuntimeError('Unknown saved motion mode')
            for key, value in original.items():
                if key not in ('animate_density', 'density_motion_mode', 'density_motion_reference'):
                    box.set_editor_property(key, unreal.Vector(*value) if isinstance(value, list) else value)
            wind = box.get_editor_property('wind_direction_component')
            for key in ('absolute_location', 'absolute_rotation', 'absolute_scale'):
                if bool(wind.get_editor_property(key)) != expected_arrow[key]:
                    wind.set_editor_property(key, expected_arrow[key])
            pitch, yaw, roll = expected_arrow['rotation']
            wind.set_world_rotation(unreal.Rotator(pitch=pitch, yaw=yaw, roll=roll), False, False)
            box.set_editor_property('animate_density', original['animate_density'])
            reference = unreal.FogMSDensityMotionReference()
            for key, value in original['density_motion_reference'].items():
                reference.set_editor_property(key, unreal.Vector(*value) if isinstance(value, list) else value)
            if not box.restore_density_motion_reference(reference):
                raise RuntimeError('Motion reference restore was rejected')
            box.update_density()
        report['after'] = snapshot()
        report['differences_after'] = differences(expected, report['after'])
        report['exactly_restored'] = not report['differences_after']
        report['source_unchanged'] = source.read_bytes() == source_bytes
        report['status'] = ('PASS' if report['exactly_restored'] else 'MISMATCH') if restore else 'READ_COMPLETE'
    except BaseException:
        report['status'] = 'FAILED'
        report['error'] = traceback.format_exc()
        report['after'] = snapshot()
        report['differences_after'] = differences(expected, report['after'])
    finally:
        output.write_text(json.dumps(report, indent=2), encoding='utf-8')
        unreal.log('FOGMS_DIRECTIONAL_RECOVERY '+str(output)+' '+report['status'])
    return str(output)


if __name__ == '__main__':
    run()
