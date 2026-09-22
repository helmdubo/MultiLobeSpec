"""Offline numerical acceptance for animation_probe receipts; never imports unreal."""
from __future__ import annotations
import argparse
import hashlib
import json
import math
from pathlib import Path
import numpy as np


def wrapped_error(a, b):
    return np.abs((np.asarray(a) - np.asarray(b) + .5) % 1.0 - .5)


def expected_phases(sample):
    frequencies = sample['mid']['WorldFrequencies'][:3]
    center = sample['transform']['center']
    controls = sample['controls']
    enabled = controls['animate_density']
    t = sample['sample_time'] if enabled else 0.0
    wind = np.asarray(controls['density_wind_velocity'], dtype=np.float64)
    detail = wind + controls['density_detail_velocity']
    velocities = (wind, detail, detail + controls['density_evolution_velocity'])
    result = []
    for f, velocity in zip(frequencies, velocities):
        cycles = np.mod(np.asarray(center) * f, 1.0) - np.mod(velocity * t * f, 1.0)
        p = np.mod(cycles, 1.0).astype(np.float32)
        p[p == 1.0] = 0.0
        result.append(p.astype(float))
    return np.asarray(result)


def phases(sample):
    return np.asarray([sample['mid']['WorldPhase' + str(k)][:3] for k in range(3)])


def relative_rms(a, b):
    return float(np.linalg.norm((a - b).ravel()) / max(np.linalg.norm(b.ravel()), 1.e-20))


def load_coefficients(record):
    prefix = Path(record['prefix'])
    meta = json.loads(Path(str(prefix) + '.json').read_text(encoding='utf-8-sig'))
    if meta != record['metadata']:
        raise ValueError('Dump metadata changed after capture')
    payload = Path(str(prefix) + '.rgba32f').read_bytes()
    if hashlib.sha256(payload).hexdigest() != record['rgba_sha256']:
        raise ValueError('Dump payload changed after capture')
    n = int(meta['grid'])
    field = np.frombuffer(payload, dtype='<f4').reshape(4, n, n, n, 4)
    if not np.isfinite(field).all():
        raise ValueError('Nonfinite atlas value')
    if np.any(field[2] < 0):
        raise ValueError('Negative medium coefficients')
    return field[2].astype(np.float64)


def analyze(receipt):
    checks, limits = [], []

    def check(name, passed, **evidence):
        checks.append(dict(name=name, passed=bool(passed), **evidence))

    check('probe_completed_and_restored', receipt.get('status') == 'COMPLETED' and receipt.get('restoration_ok') is True and not receipt.get('errors'))
    cases = {case['name']: case for case in receipt['cases']}
    planned = ('static', 'manual-zero', 'manual-shift', 'translated-tracking', 'resized-world-lock',
               'detail-evolution', 'live-a', 'live-b', 'freeze', 'frozen-later', 'resume')
    check('all_cases_present', set(cases) == set(planned))
    if set(cases) != set(planned):
        return {'status': 'FAIL', 'checks': checks, 'limits': limits}
    fields, phase_errors, packet_errors = {}, [], []
    for name, case in cases.items():
        for sample in [case['before'], case['after']] + [case[role][when] for role in ('barrier', 'measurement') for when in ('before_request', 'after_receive')]:
            phase_errors.append(float(wrapped_error(phases(sample), expected_phases(sample)).max()))
        record = case['measurement']
        meta, sample = record['metadata'], record['before_request']
        fields[name] = load_coefficients(record)
        active = name != 'static'
        check(name + '_animation_marker', meta['animationActive'] is active)
        check(name + '_settled_history_reset', meta['historyReset'] is False)
        check(name + '_no_continuous_global_revision', meta['revision'] == case['barrier']['metadata']['revision'])
        if sample['controls']['use_manual_animation_time'] or not active:
            packet = np.asarray([meta['densityPhase' + str(k)] for k in range(3)])
            packet_errors.append(float(wrapped_error(packet, phases(sample)).max()))
    check('cpu_phases_match_mid', max(phase_errors) < 2.e-7, max_wrapped_error=max(phase_errors))
    check('frozen_mid_matches_producer_packet', max(packet_errors) < 2.e-7, max_wrapped_error=max(packet_errors))
    check('density_nontrivial', fields['static'].max() > 1.e-8 and float(fields['static'].std()) > 1.e-9,
          peak=float(fields['static'].max()), std=float(fields['static'].std()))
    comparisons = [('disabled_vs_manual_zero', 'static', 'manual-zero', 1.e-7),
                   ('positive_wind_follows_translated_box', 'static', 'translated-tracking', 3.e-5),
                   ('frozen_coefficients_stable', 'freeze', 'frozen-later', 1.e-7)]
    for title, a, b, tolerance in comparisons:
        error = relative_rms(fields[a], fields[b])
        check(title, error < tolerance, relative_rms=error, tolerance=tolerance)
    for title, a, b in [('manual_time_moves_density', 'manual-zero', 'manual-shift'),
                        ('live_time_moves_density', 'live-a', 'live-b'),
                        ('resume_moves_density', 'frozen-later', 'resume')]:
        difference = relative_rms(fields[a], fields[b])
        check(title, difference > 1.e-6, relative_rms=difference)
    size_a, size_b = cases['manual-shift']['after'], cases['resized-world-lock']['after']
    resize_error = float(wrapped_error(phases(size_a), phases(size_b)).max())
    check('resize_does_not_stretch_or_carry_world_mapping', resize_error < 2.e-7
          and size_a['mid']['WorldFrequencies'] == size_b['mid']['WorldFrequencies'], max_phase_error=resize_error)
    # Different source fields from motion are intentional; no GI/lighting equality is required.
    live_a = cases['live-a']['measurement']
    live_b = cases['live-b']['measurement']
    check('live_revision_stable_across_frames', live_a['metadata']['revision'] == live_b['metadata']['revision'])
    check('live_clock_advances', live_b['before_request']['sample_time'] > live_a['before_request']['sample_time'])
    packet_motion = float(wrapped_error(live_a['metadata']['densityPhase0'], live_b['metadata']['densityPhase0']).max())
    check('live_producer_phase_advances', packet_motion > 1.e-7, max_wrapped_delta=packet_motion)
    for name in ('freeze', 'resume'):
        before, after = cases[name]['before'], cases[name]['after']
        error = float(wrapped_error(phases(before), phases(after)).max())
        check(name + '_same_call_phase_continuity', error < 2.e-7, max_wrapped_error=error)
    check('freeze_status_and_clock', 'Frozen' in cases['freeze']['after']['status']
          and cases['freeze']['after']['controls']['use_manual_animation_time']
          and cases['freeze']['after']['sample_time'] == cases['frozen-later']['measurement']['after_receive']['sample_time'])
    check('resume_status', 'Active' in cases['resume']['after']['status'] and not cases['resume']['after']['controls']['use_manual_animation_time'])
    for a, b in [('static', 'manual-zero'), ('manual-zero', 'manual-shift'), ('live-b', 'freeze'), ('frozen-later', 'resume')]:
        check(a + '_to_' + b + '_discontinuity_revision', cases[a]['measurement']['metadata']['revision'] != cases[b]['measurement']['metadata']['revision'])
    if float(receipt['original']['authored']['properties']['detail_strength']) > 0:
        difference = relative_rms(fields['manual-shift'], fields['detail-evolution'])
        check('relative_detail_evolves_shape', difference > 1.e-6, relative_rms=difference)
    else:
        limits.append('Detail density influence NOT_RUN: authored Detail Strength is zero; MID phase evolution was still checked.')
    limits += ['Rendered motion trails, flicker and local reactive-history quality require separate viewport acceptance.',
               'Exact live MID/producer time alignment is NOT_RUN: producer metadata does not carry its CPU time snapshot.',
               'No performance conclusion: synchronous GPU readback deliberately stalls rendering.']
    return {'status': 'PASS' if all(c['passed'] for c in checks) else 'FAIL', 'checks': checks, 'limits': limits}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('receipt', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    try:
        result = analyze(json.loads(args.receipt.read_text(encoding='utf-8-sig')))
    except Exception as error:
        result = {'status': 'ERROR', 'error': type(error).__name__ + ': ' + str(error)}
    output = args.output or args.receipt.with_name('analysis.json')
    output.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'status': result['status'], 'analysis': str(output),
                      'failed': [x['name'] for x in result.get('checks', []) if not x['passed']]}))
    return 0 if result['status'] == 'PASS' else 1


if __name__ == '__main__':
    raise SystemExit(main())
