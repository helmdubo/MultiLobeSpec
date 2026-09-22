"""Offline harness fault injection and analyzer checks; does not import unreal."""
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

import numpy as np

ROOT = Path(__file__).resolve().parent


def module(name):
    spec = importlib.util.spec_from_file_location('_test_' + name, ROOT / (name + '.py'))
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


impl = module('animation_impl')
analysis = module('analyze_animation')


def sample(t, *, active=True, manual=True, detail=False, center=(1000., 2000., 3000.)):
    result = {'sample_time': t if active else 0., 'world_time': 100. + t, 'engine_frame': int(t * 100),
              'controls': {'animate_density': active, 'use_manual_animation_time': manual,
                           'density_wind_velocity': [80., 0., 0.],
                           'density_detail_velocity': [17., -9., 3.] if detail else [0., 0., 0.],
                           'density_evolution_velocity': [2., 5., -7.] if detail else [0., 0., 0.]},
              'transform': {'center': list(center)},
              'mid': {'WorldFrequencies': [float(np.float32(1/8000)), float(np.float32(4/8000)), float(np.float32(8/8000)), 0.]},
              'status': 'Off' if not active else ('Frozen at manual time' if manual else 'Active: shared world-time density phases')}
    for k, p in enumerate(analysis.expected_phases(result)):
        result['mid']['WorldPhase' + str(k)] = list(p) + [0.]
    return result


def fixture(root):
    specs = [
        ('static', sample(0, active=False), 1, 1.),
        ('manual-zero', sample(0), 2, 1.),
        ('manual-shift', sample(20), 3, 1.2),
        ('translated-tracking', sample(20, center=(2600, 2000, 3000)), 4, 1.),
        ('resized-world-lock', sample(20), 5, 1.5),
        ('detail-evolution', sample(20, detail=True), 6, 1.3),
        ('live-a', sample(21, manual=False, detail=True), 7, 1.35),
        ('live-b', sample(23, manual=False, detail=True), 7, 1.4),
        ('freeze', sample(23.3, detail=True), 8, 1.42),
        ('frozen-later', sample(23.3, detail=True), 8, 1.42),
        ('resume', sample(24, manual=False, detail=True), 9, 1.5),
    ]
    receipt = {'status': 'COMPLETED', 'restoration_ok': True, 'errors': [],
               'original': {'authored': {'properties': {'detail_strength': .16}}}, 'cases': []}
    previous = sample(0, active=False)
    for index, (name, current, revision, coefficient) in enumerate(specs):
        before, after = copy.deepcopy(previous), copy.deepcopy(current)
        if name == 'freeze':
            before = sample(23.3, manual=False, detail=True)
        if name == 'resume':
            before = sample(23.3, detail=True)
            after = sample(23.3, manual=False, detail=True)
        case = {'name': name, 'before': before, 'after': after}
        for role in ('barrier', 'measurement'):
            prefix = root / (name + '-' + role)
            meta = {'grid': 2, 'animationActive': name != 'static', 'historyReset': False, 'revision': revision}
            for k in range(3):
                meta['densityPhase' + str(k)] = current['mid']['WorldPhase' + str(k)][:3]
            field = np.zeros((4, 2, 2, 2, 4), dtype='<f4')
            field[2] = coefficient * np.arange(1, 33, dtype=np.float32).reshape(2, 2, 2, 4)
            payload = field.tobytes()
            Path(str(prefix) + '.rgba32f').write_bytes(payload)
            Path(str(prefix) + '.json').write_text(json.dumps(meta), encoding='utf-8')
            case[role] = {'prefix': str(prefix), 'metadata': meta, 'rgba_sha256': hashlib.sha256(payload).hexdigest(),
                          'before_request': copy.deepcopy(current), 'after_receive': copy.deepcopy(current)}
        receipt['cases'].append(case)
        previous = current
    return receipt


class Recovery(unittest.TestCase):
    def test_cleanup_continues_after_each_action_failure(self):
        for failing in range(14):
            visited = []
            def action(i):
                visited.append(i)
                if i == failing:
                    raise RuntimeError('injected setter failure')
            result = impl.attempt_all([(str(i), lambda i=i: action(i)) for i in range(14)], [('check', lambda: True)])
            self.assertFalse(result['ok'])
            self.assertEqual(visited, list(range(14)))
            self.assertEqual(len(result['errors']), 1)

    def test_cleanup_checks_continue_after_each_verifier_failure(self):
        for failing in range(7):
            visited = []
            def check(i):
                visited.append(i)
                if i == failing:
                    raise RuntimeError('injected getter failure')
                return True
            result = impl.attempt_all([], [(str(i), lambda i=i: check(i)) for i in range(7)])
            self.assertFalse(result['ok'])
            self.assertEqual(visited, list(range(7)))

    def test_mismatch_is_not_success(self):
        self.assertFalse(impl.attempt_all([], [('restored', lambda: False)])['ok'])

    def test_success_and_atomic_receipt(self):
        result = impl.attempt_all([('restore', lambda: None)], [('restored', lambda: True)])
        self.assertTrue(result['ok'])
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'receipt.json'
            impl.atomic_json(path, result)
            self.assertEqual(json.loads(path.read_text()), result)
            self.assertFalse(path.with_suffix('.json.tmp').exists())


class Analyzer(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.receipt = fixture(Path(self.temporary.name))

    def tearDown(self):
        self.temporary.cleanup()

    def test_complete_valid_contract_passes(self):
        result = analysis.analyze(self.receipt)
        self.assertEqual(result['status'], 'PASS', [c for c in result['checks'] if not c['passed']])

    def test_wrong_phase_detected(self):
        self.receipt['cases'][2]['after']['mid']['WorldPhase0'][0] += .1
        result = analysis.analyze(self.receipt)
        self.assertEqual(result['status'], 'FAIL')
        self.assertFalse(next(c for c in result['checks'] if c['name'] == 'cpu_phases_match_mid')['passed'])

    def test_incomplete_restoration_is_not_acceptance(self):
        self.receipt['restoration_ok'] = False
        self.assertEqual(analysis.analyze(self.receipt)['status'], 'FAIL')

    def test_missing_case_is_not_acceptance(self):
        self.receipt['cases'].pop()
        self.assertEqual(analysis.analyze(self.receipt)['status'], 'FAIL')

    def test_dump_tampering_is_detected(self):
        prefix = self.receipt['cases'][0]['measurement']['prefix']
        path = Path(prefix + '.rgba32f')
        path.write_bytes(b'bad data')
        with self.assertRaisesRegex(ValueError, 'payload changed'):
            analysis.analyze(self.receipt)

    def test_history_reset_each_frame_detected(self):
        record = self.receipt['cases'][7]['measurement']
        record['metadata']['historyReset'] = True
        Path(record['prefix'] + '.json').write_text(json.dumps(record['metadata']))
        result = analysis.analyze(self.receipt)
        self.assertFalse(next(c for c in result['checks'] if c['name'] == 'live-b_settled_history_reset')['passed'])


if __name__ == '__main__':
    unittest.main(verbosity=2)
