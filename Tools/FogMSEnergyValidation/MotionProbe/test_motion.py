"""Offline orchestration/restore tests. These do NOT validate UE capture or rendering."""
import importlib.util
import json
import pathlib
import sys
import tempfile
import types
import unittest
from unittest.mock import patch
from PIL import Image

HERE = pathlib.Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('motion_test_target', HERE/'motion_impl.py')
impl = importlib.util.module_from_spec(spec); spec.loader.exec_module(impl)
spec = importlib.util.spec_from_file_location('motion_analysis_target', HERE/'analyze_motion.py')
analysis = importlib.util.module_from_spec(spec); spec.loader.exec_module(analysis)
spec = importlib.util.spec_from_file_location('motion_compare_target', HERE/'compare_motion.py')
comparison = importlib.util.module_from_spec(spec); spec.loader.exec_module(comparison)


def vector(*v):
    return types.SimpleNamespace(**dict(zip(('x', 'y', 'z'), v)))


def rotator(*v):
    return types.SimpleNamespace(**dict(zip(('pitch', 'yaw', 'roll'), v)))


def unreal_rotator(*, pitch, yaw, roll):
    """Reject positional calls so mocks cannot conceal UE's different field order."""
    return rotator(pitch, yaw, roll)


class Object:
    def get_path_name(self):
        return self.path


class Properties:
    def __init__(self, values):
        self.values = values.copy()
    def get_editor_property(self, name):
        return self.values[name]
    def set_editor_property(self, name, value):
        self.values[name] = value


class Actor(Object, Properties):
    def __init__(self, name, label, values):
        Properties.__init__(self, values); self.name = name; self.label = label
        self.path = '/Game/FogMS_Test/FogMS_Box.FogMS_Box:PersistentLevel.'+name
        self.freeze_failure = False
    def get_class(self): return types.SimpleNamespace(get_name=lambda: self.name)
    def get_actor_label(self): return self.label
    def get_actor_location(self): return vector(0, 0, 0)
    def get_actor_rotation(self): return rotator(0, 0, 0)
    def get_actor_scale3d(self): return vector(1, 1, 1)
    def is_hidden_ed(self): return False
    def get_components_by_class(self, _): return []
    def update_density(self): pass
    def freeze_density_animation(self):
        self.values['manual_animation_time'] = self.values['density_animation_time']
        self.values['use_manual_animation_time'] = True
        if self.freeze_failure: raise RuntimeError('Injected freeze failure after mutation')


class Fixture:
    def __init__(self, directory):
        self.root = pathlib.Path(directory)
        self.frame = 0; self.callback = None
        self.world = Object(); self.world.path = '/Game/FogMS_Test/FogMS_Box.FogMS_Box'
        boxprops = {p: 1.0 for p in impl.PROTECTED}
        boxprops.update(animate_density=True, use_manual_animation_time=False, manual_animation_time=0., animation_time_offset=0., density_animation_time=12.)
        self.box = Actor('FogMSBoxVolume', 'FogMS - Live Box', boxprops)
        settings = Properties({'auto_exposure_method': 'AEM_MANUAL', 'auto_exposure_min_brightness': 1.,
            'auto_exposure_max_brightness': 1., 'override_auto_exposure_method': True,
            'override_auto_exposure_min_brightness': False, 'override_auto_exposure_max_brightness': False})
        self.exposure = Actor('PostProcessVolume', 'FogMS - Fixed Exposure', {'settings': settings})
        self.perf = Properties({'bThrottleCPUWhenNotForeground': True})
        self.autosave = Properties({'bAutoSaveEnable': True})
        self.camera = {'position': [100., 200., 300.], 'rotation': [-16.2, 142., .01], 'fov': 55.}
        self.initial_camera = json.loads(json.dumps(self.camera))
        self.game_view = False; self.initial_game_view = self.game_view
        self.ignore_game_view_enable = False; self.ignore_game_view_restore = False
        self.initial_animation = {p: self.box.values[p] for p in impl.ANIMATION}
        self.levels = types.SimpleNamespace(get_active_viewport_config_key=lambda: 'Viewport1', is_in_play_in_editor=lambda: False,
            get_pilot_level_actor=lambda key: None,
            get_level_viewport_camera_info=lambda key: (True, vector(*self.camera['position']), rotator(*self.camera['rotation'])),
            get_level_viewport_fov=lambda key: (True, self.camera['fov']),
            set_level_viewport_camera_info=self.set_camera, set_level_viewport_fov=lambda f, key: self.camera.update(fov=f),
            editor_get_game_view=lambda key: self.game_view, editor_set_game_view=self.set_game_view,
            editor_invalidate_viewports=lambda: None)
        self.actors = types.SimpleNamespace(get_all_level_actors=lambda: [self.box, self.exposure])
        self.editor = types.SimpleNamespace(get_editor_world=lambda: self.world)
        self.unreal = types.SimpleNamespace(Object=Object, Vector=vector, Rotator=unreal_rotator,
            LevelEditorSubsystem='levels', EditorActorSubsystem='actors', UnrealEditorSubsystem='editor', LightComponent='light',
            get_editor_subsystem=lambda name: getattr(self, name),
            load_class=lambda _, name: name,
            get_default_object=lambda name: self.perf if name.endswith('EditorPerformanceSettings') else self.autosave,
            Paths=types.SimpleNamespace(project_dir=lambda: str(self.root/'project')),
            SystemLibrary=types.SimpleNamespace(get_frame_count=lambda: self.frame),
            register_slate_post_tick_callback=self.register, unregister_slate_post_tick_callback=self.unregister,
            log=lambda message: None)
        map_file = self.root/'project/Content/FogMS_Test/FogMS_Box.umap'
        map_file.parent.mkdir(parents=True); map_file.write_bytes(b'original-map')
        self.config = json.loads((HERE/'motion_config.json').read_text())
        self.config.update(id='mock', evidence_root=str(self.root/'evidence'), cases=['yaw', 'dolly', 'fov'],
                           sample_delays=[1, 2], warm_frames=1, motion_frames=2, reference_frames=3, force_game_view=True)
        (self.root/'motion_config.json').write_text(json.dumps(self.config))

    def set_camera(self, pos, rot, key):
        self.camera['position'] = [pos.x, pos.y, pos.z]
        self.camera['rotation'] = [rot.pitch, rot.yaw, rot.roll]
    def set_game_view(self, enabled, key):
        if enabled and self.ignore_game_view_enable: return
        if not enabled and self.ignore_game_view_restore: return
        self.game_view = enabled
    def register(self, callback): self.callback = callback; return 7
    def unregister(self, handle): self.callback = None
    @property
    def run(self): return self.root/'evidence/mock'
    def simulate_capture(self):
        requests = sorted((self.run/'capture_requests').glob('*.json'))
        if not requests: return
        p = requests[-1]
        request = json.loads(p.read_text()); image = pathlib.Path(request['filename'])
        if not image.exists():
            Image.new('RGB', (64, 48), (90, 110, 140)).save(image)
            impl.atomic_json(self.run/'capture_acks'/p.name, {'sequence': request['sequence'], 'result': {'result': {'target': 'editor'}}})
    def advance(self):
        self.frame += 1
        if self.callback: self.callback(1/30)
        self.simulate_capture()
    def restored(self):
        return self.camera == self.initial_camera and self.game_view == self.initial_game_view and {p: self.box.values[p] for p in impl.ANIMATION} == self.initial_animation and self.perf.values['bThrottleCPUWhenNotForeground'] and self.autosave.values['bAutoSaveEnable'] and self.callback is None


class Tests(unittest.TestCase):
    def execute_fixture(self, mode):
        with tempfile.TemporaryDirectory(prefix='fogms-motion-unit-') as directory:
            f = Fixture(directory)
            if mode == 'freeze_failure': f.box.freeze_failure = True
            if mode == 'game_view_set_failure': f.ignore_game_view_enable = True
            if mode == 'game_view_restore_failure': f.ignore_game_view_restore = True
            with patch.dict(sys.modules, {'unreal': f.unreal}), patch.object(impl, 'ROOT', f.root), patch.object(impl, 'CONTROLLER', None):
                impl.launch()
                if mode != 'freeze_failure':
                    impl.atomic_json(f.run/'capture_worker.json', {'ready': True})
                    if mode == 'intervention': f.camera['position'][0] += 10
                    if mode == 'game_view_intervention': f.game_view = False
                    for _ in range(200):
                        f.advance()
                        if not f.callback: break
                receipt = json.loads((f.run/'receipt.json').read_text())
                self.assertEqual(f.restored(), mode != 'game_view_restore_failure')
                self.assertEqual(receipt['restoration']['ok'], mode != 'game_view_restore_failure')
                self.assertEqual(receipt['status'], 'COMPLETED' if mode == 'success' else 'FAILED')
                if mode == 'success':
                    self.assertEqual(len(receipt['captures']), 9)
                    self.assertTrue(all(c['game_view'] for c in receipt['captures']))
                    self.assertFalse(receipt['viewport_environment']['original_game_view'])
                    self.assertTrue(receipt['viewport_environment']['during_game_view'])
                    self.assertTrue(receipt['restoration']['checks']['game_view_exact'])
                    measured = analysis.analyze(f.run/'receipt.json')
                    self.assertEqual(measured['status'], 'MEASURED')
                    self.assertFalse(measured['exact_gpu_frame_claim'])
                    legacy = json.loads(json.dumps(receipt)); legacy.pop('viewport_environment')
                    for capture in legacy['captures']: capture.pop('game_view')
                    legacy_path = f.run/'legacy.json'; legacy_path.write_text(json.dumps(legacy))
                    self.assertEqual(comparison.compare(legacy_path, f.run/'receipt.json')['status'], 'LIMITED')
                    wrong_mode = json.loads(json.dumps(receipt))
                    wrong_mode['viewport_environment'].update(requested_game_view=False, during_game_view=False)
                    for capture in wrong_mode['captures']: capture['game_view'] = False
                    wrong_path = f.run/'wrong_mode.json'; wrong_path.write_text(json.dumps(wrong_mode))
                    self.assertEqual(comparison.compare(wrong_path, f.run/'receipt.json')['status'], 'INVALID')
                    pathlib.Path(receipt['captures'][0]['filename']).write_bytes(b'corrupted')
                    self.assertEqual(analysis.analyze(f.run/'receipt.json')['status'], 'INVALID')
                if mode == 'intervention':
                    self.assertIn('CAMERA_STATE_DIVERGED', ''.join(receipt['errors']))
                if mode == 'game_view_intervention':
                    self.assertIn('GAME_VIEW_STATE_DIVERGED', ''.join(receipt['errors']))
                if mode == 'game_view_set_failure':
                    self.assertIn('GAME_VIEW_API_ROUND_TRIP', ''.join(receipt['errors']))
                if mode == 'game_view_restore_failure':
                    self.assertIn('GAME_VIEW_RESTORE_MISMATCH', ''.join(receipt['errors']))
                    self.assertFalse(receipt['restoration']['checks']['game_view_exact'])

    def test_complete_and_hash_invalidation(self): self.execute_fixture('success')
    def test_camera_intervention_restores(self): self.execute_fixture('intervention')
    def test_start_exception_restores_mutated_animation(self): self.execute_fixture('freeze_failure')
    def test_game_view_intervention_restores(self): self.execute_fixture('game_view_intervention')
    def test_game_view_set_failure_is_explicit(self): self.execute_fixture('game_view_set_failure')
    def test_game_view_restore_failure_is_explicit(self): self.execute_fixture('game_view_restore_failure')
    def test_close_values_nested_tolerance(self):
        self.assertTrue(impl.close_values({'x': [1., 2.]}, {'x': [1.001, 2.]}))
        self.assertFalse(impl.close_values({'x': [1.]}, {'x': [2.]}))
    def test_rotator_mock_requires_explicit_field_names(self):
        with self.assertRaises(TypeError): unreal_rotator(-16.2, 142., .01)
        r = unreal_rotator(pitch=-16.2, yaw=142., roll=.01)
        self.assertEqual([r.pitch, r.yaw, r.roll], [-16.2, 142., .01])
    def test_comparison_rejects_scene_changes_but_allows_serialization_roundoff(self):
        a = {'actors': [{'yaw': 25.865636955562486, 'hidden': True}], 'density': .2}
        b = {'actors': [{'yaw': 25.865636955562493, 'hidden': False}], 'density': .2}
        self.assertEqual(comparison.state_differences(a, b, 'authored'), ['authored/actors/0/hidden'])
        b['actors'][0]['hidden'] = True
        self.assertEqual(comparison.state_differences(a, b), [])
        b['density'] = .1
        self.assertEqual(comparison.state_differences(a, b), ['/density'])


if __name__ == '__main__':
    unittest.main()
