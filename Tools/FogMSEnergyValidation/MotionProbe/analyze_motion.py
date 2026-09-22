"""Measure actual native PNG settling; no rendered acceptance is inferred from mocks.

Output metrics are display-referred screenshot metrics, not an energy-balance test.
Absent explicit budgets, status is MEASURED, never visual PASS.
"""
from __future__ import annotations
import argparse
import hashlib
import itertools
import json
import pathlib
import numpy as np
from PIL import Image


def linear_rgb(path):
    a = np.asarray(Image.open(path).convert('RGB'), dtype=np.float64)/255.0
    return np.where(a <= .04045, a/12.92, ((a+.055)/1.055)**2.4)


def metrics(image, reference):
    if image.shape != reference.shape:
        raise ValueError('Viewport resolution changed between captures')
    luma = image@np.array([.2126, .7152, .0722])
    ref_luma = reference@np.array([.2126, .7152, .0722])
    grad = np.sqrt(np.mean(np.diff(luma, axis=0)**2)+np.mean(np.diff(luma, axis=1)**2))
    ref_grad = np.sqrt(np.mean(np.diff(ref_luma, axis=0)**2)+np.mean(np.diff(ref_luma, axis=1)**2))
    return {'rgb_relative_rms': float(np.sqrt(np.mean((image-reference)**2))/max(np.sqrt(np.mean(reference**2)), .02)),
            'mean_luma_relative_error': float((luma.mean()-ref_luma.mean())/max(ref_luma.mean(), .02)),
            'luma_p95_absolute_error': float(np.quantile(abs(luma-ref_luma), .95)),
            'contrast_ratio': float(luma.std()/max(ref_luma.std(), 1.e-8)),
            'gradient_ratio': float(grad/max(ref_grad, 1.e-8)),
            'reference_mean_luma': float(ref_luma.mean()), 'reference_contrast': float(ref_luma.std())}


def same_camera(a, b):
    return all(np.allclose(a[key], b[key], rtol=0, atol=.002) for key in ('position', 'rotation', 'fov'))


def analyze(receipt_path, roi_override=None):
    receipt_path = pathlib.Path(receipt_path)
    receipt = json.loads(receipt_path.read_text(encoding='utf-8-sig'))
    config = receipt['config']; captures = receipt['captures']
    roi = roi_override or config.get('roi', [0.15, 0.15, .85, .9])
    if len(roi) != 4 or not (0 <= roi[0] < roi[2] <= 1 and 0 <= roi[1] < roi[3] <= 1):
        raise ValueError('ROI must be normalized x0,y0,x1,y1')
    issues, limitations, images = [], [], {}
    environment = receipt.get('viewport_environment')
    if environment is None:
        limitations.append('Legacy receipt lacks game-view metadata; editor/game show-mode equivalence requires image review.')
    elif (not isinstance(environment.get('requested_game_view'), bool)
            or environment.get('during_game_view') != environment.get('requested_game_view')):
        issues.append('Viewport game-view mode was not verified against the requested mode')
    elif not receipt.get('restoration', {}).get('checks', {}).get('game_view_exact'):
        issues.append('Viewport game-view restoration was not verified')
    if receipt['status'] != 'COMPLETED' or not receipt.get('restoration', {}).get('ok') or receipt.get('errors'):
        issues.append('Run did not complete with verified restoration')
    expected = {(c, str(d)) for c in config['cases'] for d in [*config['sample_delays'], 'reference']}
    actual = [(c['case'], str(c['target_delay_frames'])) for c in captures]
    if set(actual) != expected or len(set(actual)) != len(actual):
        issues.append('Missing or duplicate case/delay captures')
    for c in captures:
        if environment is not None and c.get('game_view') != environment.get('requested_game_view'):
            issues.append('Capture game-view mode differs from requested mode: '+str(c.get('sequence')))
        file = pathlib.Path(c['filename'])
        if not file.exists() or hashlib.sha256(file.read_bytes()).hexdigest() != c['sha256']:
            issues.append('Screenshot absent or hash changed: '+str(file)); continue
        a = linear_rgb(file); height, width = a.shape[:2]
        if [width, height] != c['png_size']:
            issues.append('PNG dimensions disagree with receipt')
        box = (int(roi[0]*width), int(roi[1]*height), int(roi[2]*width), int(roi[3]*height))
        if box[2]-box[0] < 8 or box[3]-box[1] < 8:
            raise ValueError('ROI too small')
        images[(c['case'], str(c['target_delay_frames']))] = a[box[1]:box[3], box[0]:box[2]]
        if c['capture_offset_interval'] != [c['request_engine_frame']-c['stop_engine_frame'], c['file_observed_engine_frame']-c['stop_engine_frame']]:
            issues.append('Timing interval inconsistent')
        if c['capture_offset_interval'][1] < c['capture_offset_interval'][0]:
            issues.append('Negative capture timing interval')
    rows = []
    for c in captures:
        key = (c['case'], str(c['target_delay_frames'])); ref_key = (c['case'], 'reference')
        if key[1] == 'reference' or key not in images or ref_key not in images:
            continue
        ref = next(r for r in captures if (r['case'], str(r['target_delay_frames'])) == ref_key)
        if not same_camera(c['camera'], ref['camera']) or c['animation_time'] != ref['animation_time']:
            issues.append('Capture and warmed reference differ in camera or frozen phase: '+str(key))
            continue
        rows.append({'case': c['case'], 'requested_delay': c['target_delay_frames'],
                     'capture_offset_interval': c['capture_offset_interval'],
                     'reference_offset_interval': ref['capture_offset_interval'], **metrics(images[key], images[ref_key])})
    refs = [c for c in captures if c['target_delay_frames'] == 'reference']
    drift = []
    for a, b in itertools.combinations(refs, 2):
        ka, kb = (a['case'], 'reference'), (b['case'], 'reference')
        if ka in images and kb in images and same_camera(a['camera'], b['camera']) and a['animation_time'] == b['animation_time']:
            drift.append({'a': a['case'], 'b': b['case'], **metrics(images[ka], images[kb])})
    budgets = config.get('acceptance_thresholds')
    checks = []
    if budgets:
        for row in rows:
            if row['requested_delay'] >= budgets.get('settled_requested_delay', 8):
                checks.append({'case': row['case'], 'requested_delay': row['requested_delay'],
                               'pass': row['rgb_relative_rms'] <= budgets['rgb_relative_rms_max']
                               and abs(row['mean_luma_relative_error']) <= budgets['mean_luma_relative_error_max']
                               and abs(1-row['contrast_ratio']) <= budgets['contrast_relative_error_max']})
    status = 'INVALID' if issues else ('MEASURED' if not budgets else ('PASS' if checks and all(x['pass'] for x in checks) else 'FAIL'))
    return {'status': status, 'issues': issues, 'limitations': limitations, 'receipt': str(receipt_path.resolve()), 'roi': roi,
            'interpretation': 'Native LDR screenshots decoded approximately from sRGB; display-referred comparison, not radiometric energy. Timing is request-to-file-observed interval, not exact GPU frame.',
            'exact_gpu_frame_claim': False, 'metrics': rows, 'warmed_reference_drift': drift, 'budget_checks': checks}


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('receipt', type=pathlib.Path)
    parser.add_argument('--roi', nargs=4, type=float)
    args = parser.parse_args()
    result = analyze(args.receipt, args.roi)
    output = args.receipt.with_name('analysis.json')
    output.write_text(json.dumps(result, indent=2, allow_nan=False), encoding='utf-8')
    print(json.dumps({'status': result['status'], 'issues': result['issues'], 'output': str(output)}, indent=2))
    raise SystemExit(1 if result['status'] in ('INVALID', 'FAIL') else 0)
