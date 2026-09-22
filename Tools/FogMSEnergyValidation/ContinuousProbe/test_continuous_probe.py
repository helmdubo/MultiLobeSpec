"""Offline state-machine/ownership tests. No UE process, shader or real image claim."""
import copy
import importlib.util
import json
import pathlib
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location('continuous_test_impl', pathlib.Path(__file__).with_name('continuous_impl.py'))
P = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(P)
FAKE_PNG = b'\x89PNG\r\n\x1a\n' + bytes(8) + (64).to_bytes(4, 'big') * 2 + bytes(8) + b'\x00\x00\x00\x00IEND\xaeB\x60\x82'


class Fake:
    def __init__(self):
        self.cam = {'position': [200., -300., 450.], 'rotation': [-15., 35., 0.], 'fov': 55.}
        self.game = False
        self.cv = {**{key: 1 for key in P.CVAR_OFF}, P.CVAR_MODE: 1}
        self.prefs = {'throttle': True, 'autosave': True}
        self.motion_value = {'animation': {'animate_density': True, 'use_manual_animation_time': False,
                                           'manual_animation_time': 0., 'animation_time_offset': 7.},
                             'reference': {'initialized': True, 'time': 1., 'displacement0': [1., 2., 3.]}}
        self.scene = {'box': {'density': .15}, 'transform': [5., 6., 7.]}
        self.exposure_value = {'fixed': True}
        self.count = 10
        self.context = True
        self.commands = []
        self.callback = None
        self.fail_freeze = False

    def context_ok(self): return self.context
    def camera(self): return copy.deepcopy(self.cam)
    def set_camera(self, value):
        self.cam = copy.deepcopy(value)
        self.commands.append((self.count, self.camera()))
    def game_view(self): return self.game
    def set_game_view(self, value): self.game = value
    def get_cvar(self, name): return self.cv[name]
    def set_cvar(self, name, value): self.cv[name] = value
    def preferences(self): return self.prefs.copy()
    def set_preference(self, name, value): self.prefs[name] = value
    def motion(self): return copy.deepcopy(self.motion_value)
    def restore_motion(self, value): self.motion_value = copy.deepcopy(value)
    def phases(self): return [[.1, .2, .3], [.4, .5, .6], [.7, .8, .9]]
    def freeze(self):
        self.motion_value['animation']['use_manual_animation_time'] = True
        self.motion_value['animation']['manual_animation_time'] = 123.
        if self.fail_freeze:
            raise RuntimeError('injected partial freeze failure')
    def authored(self): return copy.deepcopy(self.scene)
    def exposure(self): return copy.deepcopy(self.exposure_value)
    def map_sha(self): return 'unchanged-map-hash'
    def frame(self): return self.count
    def register(self, callback):
        self.callback = callback
        return 'callback-handle'
    def unregister(self, handle): self.callback = None


class Safety(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.n = Fake()
        self.c = P.configuration({'id': 'offline', 'evidence_root': self.tmp.name, 'leg_frames': 60,
                                  'warmup_frames': 16, 'captures_per_leg': 4})
        self.probe = P.Probe(self.c, self.n)
        self.probe.start()
        P.publish(self.probe.root/'capture_worker.json', {'ready': True, 'target': 'editor'})

    def tearDown(self):
        if not self.probe.done:
            self.probe.finish('STOPPED')
        self.tmp.cleanup()

    def step(self, count=1, delay=2):
        for _ in range(count):
            if self.probe.done:
                return
            self.n.count += 1
            pending = self.probe.pending
            if pending and self.n.count-pending['request_frame'] >= delay:
                # Only protocol header/IEND fixture; not a rendered or decoded image.
                pathlib.Path(pending['filename']).write_bytes(FAKE_PNG)
                ack = self.probe.root/'capture_acks'/('%06d.json' % pending['sequence'])
                if not ack.exists():
                    P.publish(ack, {'sequence': pending['sequence'], 'result': {'id': pending['sequence'], 'result': {'success': True}}})
            self.probe.tick(.033)

    def receipt(self):
        return json.loads((self.probe.root/'receipt.json').read_text())

    def begin_motion(self):
        self.step(18)
        self.assertEqual(self.probe.state, 'moving')

    def test_complete_uses_real_engine_frames_and_same_path(self):
        self.step(400)
        r = self.receipt()
        self.assertEqual(r['status'], 'COMPLETED')
        self.assertEqual(r['valid_continuous_captures_per_leg'], [4, 4, 4, 4])
        self.assertTrue(r['restoration']['all_owned_values_restored'])
        self.assertEqual(r['visual_acceptance'], 'NOT_EVALUATED')
        self.assertFalse(r['exact_gpu_frame_claim'])
        self.assertEqual([x['observed_engine_duration'] for x in r['legs']], [60]*4)
        self.assertEqual(r['legs'][0]['start_camera'], r['legs'][2]['start_camera'])
        self.assertEqual(r['legs'][1]['end_camera'], r['legs'][3]['end_camera'])
        for capture in r['captures']:
            self.assertNotEqual(capture['request_camera']['position'], capture['observed_camera']['position'])
        self.assertEqual(self.n.camera(), self.probe.original['camera'])
        self.assertEqual(self.n.motion(), self.probe.original['motion'])

    def test_external_camera_preserved_but_preferences_restored(self):
        self.begin_motion()
        self.n.cam['position'][0] += 1500
        user_camera = self.n.camera()
        self.step()
        r = self.receipt()
        self.assertEqual(r['status'], 'ABORTED')
        self.assertEqual(r['restoration']['items']['camera']['status'], 'preserved_external')
        self.assertEqual(self.n.camera(), user_camera)
        self.assertEqual(self.n.preferences(), self.probe.original['preferences'])

    def test_external_density_is_not_overwritten(self):
        self.begin_motion()
        self.n.scene['box']['density'] = .9
        self.step()
        self.assertEqual(self.receipt()['status'], 'ABORTED')
        self.assertEqual(self.n.scene['box']['density'], .9)
        self.assertFalse(self.receipt()['restoration']['checks']['authored_unchanged'])
        self.assertEqual(self.n.camera(), self.probe.original['camera'])

    def test_external_motion_group_and_cvar_preserved(self):
        self.begin_motion()
        self.n.motion_value['animation']['manual_animation_time'] = 900
        self.n.cv[P.CVAR_MODE] = 2
        self.step()
        r = self.receipt()
        self.assertEqual(r['status'], 'ABORTED')
        self.assertEqual(self.n.motion_value['animation']['manual_animation_time'], 900)
        self.assertEqual(r['restoration']['items']['motion']['status'], 'preserved_external')
        self.assertEqual(r['restoration']['items']['cvar:'+P.CVAR_MODE]['status'], 'preserved_external')
        self.assertEqual(self.n.cv[P.CVAR_MODE], 2)

    def test_pending_capture_does_not_pause_and_late_capture_is_invalid(self):
        self.step(700, delay=70)
        r = self.receipt()
        self.assertEqual(r['status'], 'INSUFFICIENT_CONTINUOUS_CAPTURES')
        self.assertEqual([x['observed_engine_duration'] for x in r['legs']], [60]*4)
        self.assertTrue(r['skipped_slots'])
        self.assertTrue(all(not c['interval_entirely_within_moving_leg'] for c in r['captures']))
        self.assertTrue(r['restoration']['all_owned_values_restored'])

    def test_repeated_slate_callback_does_not_count_as_engine_frame(self):
        self.step()
        before = self.probe.stage_start
        for _ in range(100):
            self.probe.tick(.033)
        self.assertEqual(self.probe.state, 'warmup')
        self.assertEqual(before, self.probe.stage_start)
        self.assertEqual(self.probe.poses, [])

    def test_partial_freeze_exception_restores_group(self):
        self.n.fail_freeze = True
        self.step()
        self.assertEqual(self.receipt()['status'], 'ABORTED')
        self.assertEqual(self.n.motion(), self.probe.original['motion'])
        self.assertEqual(self.n.preferences(), self.probe.original['preferences'])

    def test_context_change_never_writes_old_camera(self):
        self.begin_motion()
        self.n.context = False
        new_camera = self.n.camera()
        new_camera['position'] = [9999., 0., 0.]
        self.n.cam = new_camera
        self.step()
        self.assertEqual(self.receipt()['status'], 'ABORTED')
        self.assertEqual(self.n.camera(), new_camera)
        self.assertEqual(self.n.preferences(), self.probe.original['preferences'])

    def test_capture_timeout_restores_and_retains_request(self):
        self.begin_motion()
        self.step(4, delay=100)
        self.assertIsNotNone(self.probe.pending)
        self.probe.pending['request_monotonic'] -= 50
        self.step(delay=100)
        self.assertEqual(self.receipt()['status'], 'ABORTED')
        self.assertIn('CAPTURE_TIMEOUT', ''.join(self.receipt()['errors']))
        self.assertTrue(list((self.probe.root/'capture_requests').glob('*.json')))
        self.assertEqual(self.n.preferences(), self.probe.original['preferences'])

    def test_queued_ack_waits_for_complete_png_while_camera_moves(self):
        self.begin_motion()
        self.step(4, delay=100)
        request = self.probe.pending
        P.publish(self.probe.root/'capture_acks'/('%06d.json' % request['sequence']),
                  {'sequence': request['sequence'], 'result': {'result': {'success': True, 'note': 'queued'}}})
        before = self.n.camera()
        self.step(delay=100)
        self.assertFalse(self.probe.done)
        self.assertIsNotNone(self.probe.pending)
        self.assertNotEqual(self.n.camera(), before)
        pathlib.Path(request['filename']).write_bytes(FAKE_PNG[:-12])
        self.step(delay=100)
        self.assertFalse(self.probe.done)
        self.assertIsNotNone(self.probe.pending)
        pathlib.Path(request['filename']).write_bytes(FAKE_PNG)
        self.step(delay=100)
        self.assertIsNone(self.probe.pending)
        self.assertEqual(self.probe.records[-1]['png_size'], [64, 64])

    def test_immutable_evidence_rejects_overwrite(self):
        original = (self.probe.root/'original.json').read_bytes()
        with self.assertRaises(RuntimeError):
            P.publish(self.probe.root/'original.json', {'bad': True})
        self.assertEqual((self.probe.root/'original.json').read_bytes(), original)
        with self.assertRaises(RuntimeError):
            P.Probe(self.c, self.n)


if __name__ == '__main__':
    unittest.main()
