"""Compare measured settling before/after without fabricating visual acceptance."""
import argparse
import importlib.util
import json
import math
import pathlib

spec = importlib.util.spec_from_file_location('_motion_compare_analysis', pathlib.Path(__file__).with_name('analyze_motion.py'))
analysis = importlib.util.module_from_spec(spec); spec.loader.exec_module(analysis)


def state_differences(a, b, path=''):
    """Allow serialization roundoff, but keep flags and authored scene changes visible."""
    if isinstance(a, dict) and isinstance(b, dict):
        result = []
        for key in a.keys() | b.keys():
            if key not in a or key not in b:
                result.append(path+'/'+key)
            else:
                result.extend(state_differences(a[key], b[key], path+'/'+key))
        return result
    if isinstance(a, list) and isinstance(b, list) and len(a) == len(b):
        return [p for i, (x, y) in enumerate(zip(a, b)) for p in state_differences(x, y, path+'/'+str(i))]
    if isinstance(a, bool) or isinstance(b, bool):
        return [] if type(a) is type(b) and a == b else [path]
    if isinstance(a, (float, int)) and isinstance(b, (float, int)):
        return [] if math.isclose(a, b, rel_tol=0., abs_tol=1.e-5) else [path]
    return [] if a == b else [path]


def compare(before_path, after_path, roi=None):
    before = json.loads(pathlib.Path(before_path).read_text())
    after = json.loads(pathlib.Path(after_path).read_text())
    left = analysis.analyze(before_path, roi); right = analysis.analyze(after_path, roi)
    issues, limitations = [], []
    if left['status'] in ('INVALID', 'FAIL') or right['status'] in ('INVALID', 'FAIL'):
        issues.append('One input run is invalid or fails its explicit budgets')
    required = ('cases', 'sample_delays', 'warm_frames', 'motion_frames', 'reference_frames', 'yaw_degrees', 'dolly_cm', 'fov_degrees')
    for key in required:
        if before['config'][key] != after['config'][key]: issues.append('Motion protocol changed: '+key)
    if 'pan' in before['config']['cases'] and before['config'].get('pan_cm') != after['config'].get('pan_cm'):
        issues.append('Motion protocol changed: pan_cm')
    env_a, env_b = before.get('viewport_environment'), after.get('viewport_environment')
    if env_a is None or env_b is None:
        limitations.append('Game-view metadata is absent in at least one legacy receipt. Review both native images for editor bounds/grid/icons; same game-view conditions are unproven by metadata.')
    elif env_a.get('during_game_view') != env_b.get('during_game_view'):
        issues.append('Viewport game-view mode changed between runs')
    original_a = pathlib.Path(before_path).with_name('original.json')
    original_b = pathlib.Path(after_path).with_name('original.json')
    if original_a.exists() and original_b.exists():
        snapshots = [json.loads(p.read_text(encoding='utf-8-sig')) for p in (original_a, original_b)]
        for scope in ('authored', 'exposure'):
            changes = state_differences(snapshots[0][scope], snapshots[1][scope], scope)
            if changes:
                issues.append('Scene conditions changed: '+', '.join(changes))
    else:
        limitations.append('Original authored-scene/exposure snapshots unavailable for comparison.')
    ca = {(c['case'], str(c['target_delay_frames'])): c for c in before['captures']}
    cb = {(c['case'], str(c['target_delay_frames'])): c for c in after['captures']}
    for key in ca.keys() & cb.keys():
        if not analysis.same_camera(ca[key]['camera'], cb[key]['camera']) or ca[key]['png_size'] != cb[key]['png_size'] or ca[key]['animation_time'] != cb[key]['animation_time']:
            issues.append('Camera, resolution or density phase changed: '+str(key))
    rows = []
    right_by_key = {(r['case'], r['requested_delay']): r for r in right['metrics']}
    for a in left['metrics']:
        b = right_by_key.get((a['case'], a['requested_delay']))
        if not b: continue
        ia, ib = a['capture_offset_interval'], b['capture_offset_interval']
        rows.append({'case': a['case'], 'requested_delay': a['requested_delay'], 'before_interval': ia, 'after_interval': ib,
            'timing_intervals_overlap': max(ia[0], ib[0]) <= min(ia[1], ib[1]),
            'before_rgb_relative_rms': a['rgb_relative_rms'], 'after_rgb_relative_rms': b['rgb_relative_rms'],
            'rms_ratio_after_over_before': b['rgb_relative_rms']/max(a['rgb_relative_rms'], 1.e-8),
            'before_contrast_ratio': a['contrast_ratio'], 'after_contrast_ratio': b['contrast_ratio'],
            'before_gradient_ratio': a['gradient_ratio'], 'after_gradient_ratio': b['gradient_ratio']})
    return {'status': 'INVALID' if issues else ('LIMITED' if limitations else 'MEASURED'),
            'issues': issues, 'limitations': limitations, 'comparison': rows,
            'before_reference_drift': left['warmed_reference_drift'], 'after_reference_drift': right['warmed_reference_drift'],
            'note': 'Before and after compare to their own warmed reference. Overlapping request-to-observation intervals do not prove identical GPU timing; this is not an exact frame-matched claim.'}


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('before', type=pathlib.Path); parser.add_argument('after', type=pathlib.Path)
    parser.add_argument('--roi', nargs=4, type=float)
    parser.add_argument('--output', type=pathlib.Path, required=True)
    args = parser.parse_args()
    result = compare(args.before, args.after, args.roi)
    args.output.write_text(json.dumps(result, indent=2, allow_nan=False), encoding='utf-8')
    print(json.dumps({'status': result['status'], 'issues': result['issues'], 'output': str(args.output)}, indent=2))
    raise SystemExit(1 if result['status'] == 'INVALID' else 0)
