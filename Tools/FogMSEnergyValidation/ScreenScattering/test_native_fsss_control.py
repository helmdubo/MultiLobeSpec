"""Offline transaction tests using a fake Unreal adapter; never opens Unreal."""
import importlib.util
import json
import pathlib
import sys
import tempfile
import types
import unittest
from unittest.mock import patch


SPEC = importlib.util.spec_from_file_location('native_fsss_control', pathlib.Path(__file__).with_name('native_fsss_control.py'))
CONTROL = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CONTROL)


class HarnessSafety(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tmp.name)
        self.session = self.root / 'evidence'
        content = self.root / 'Content' / 'FogMS_Test'
        content.mkdir(parents=True)
        (content / 'FogMS_Box.umap').write_bytes(b'FAKE map bytes; not an Unreal asset')
        self.values = {
            'bEnableFSSS': False, 'FSSSSceneColorScatteringAmountScale': .3,
            'FSSSSceneColorScatteringAmountPower': .8, 'FSSSSpreadScale': .05,
            'FSSSBlurControl': .2, 'visible': True,
        }
        self.cvars = {'r.Fog.ScreenSpaceScattering': 0, 'r.Fog.SeparateComposition': 0,
                      'r.Fog.ScreenSpaceScattering.TAA': 1, 'r.ForwardShading': 0, 'r.Fog': 1}
        self.initial_values = self.values.copy()
        self.initial_cvars = self.cvars.copy()
        self.fail_key = None
        self.use_aliases = False
        self.actor_count = 1
        self.world_name = CONTROL.DEFAULT_WORLD
        self.commands = []

        def getprop(name):
            if self.use_aliases:
                mapping = {v[1]: v[0] for v in CONTROL.PROPERTY_CANDIDATES.values()}
                if name in mapping:
                    return self.values[mapping[name]]
                if name != 'visible':
                    raise KeyError(name)
            return self.values[name]

        def setprop(name, value):
            name = {v[1]: v[0] for v in CONTROL.PROPERTY_CANDIDATES.values()}.get(name, name)
            if self.fail_key == name:
                self.fail_key = None
                raise RuntimeError('Injected one-time setter failure')
            self.values[name] = value

        component = types.SimpleNamespace(get_editor_property=getprop, set_editor_property=setprop,
                                          get_path_name=lambda: CONTROL.DEFAULT_WORLD + ':Fog.Component')
        actor = types.SimpleNamespace(get_components_by_class=lambda cls: [component],
                                      get_path_name=lambda: CONTROL.DEFAULT_WORLD + ':Fog',
                                      get_actor_location=lambda: [0., 0., 0.],
                                      get_actor_rotation=lambda: [0., 0., 0.],
                                      get_actor_scale3d=lambda: [1., 1., 1.], is_hidden_ed=lambda: False)
        levels = types.SimpleNamespace(is_in_play_in_editor=lambda: False,
                                       get_active_viewport_config_key=lambda: 'Viewport1',
                                       get_level_viewport_camera_info=lambda key: [[1., 2., 3.], [4., 5., 6.]],
                                       get_level_viewport_fov=lambda key: 55., editor_get_game_view=lambda key: True)
        world = types.SimpleNamespace(get_path_name=lambda: self.world_name)
        subsystems = {'level': levels,
                      'world': types.SimpleNamespace(get_editor_world=lambda: world),
                      'actor': types.SimpleNamespace(get_all_level_actors=lambda: [actor] * self.actor_count)}

        def command(world, text):
            self.commands.append(text)
            key, value = text.split()
            self.cvars[key] = int(value)

        self.fake = types.SimpleNamespace(LevelEditorSubsystem='level', UnrealEditorSubsystem='world',
            EditorActorSubsystem='actor', ExponentialHeightFogComponent=object,
            get_editor_subsystem=lambda name: subsystems[name],
            Paths=types.SimpleNamespace(project_dir=lambda: str(self.root)), log=lambda text: None,
            SystemLibrary=types.SimpleNamespace(get_console_variable_int_value=lambda name: self.cvars[name],
                get_console_variable_float_value=lambda name: 10., get_frame_count=lambda: 100,
                execute_console_command=command))
        self.patcher = patch.dict(sys.modules, unreal=self.fake)
        self.patcher.start()

    def tearDown(self):
        self.patcher.stop()
        self.tmp.cleanup()

    def call(self, action, **kwargs):
        path = CONTROL.run(action, str(self.session), **kwargs)
        return json.loads(pathlib.Path(path).read_text())

    def latest(self):
        return json.loads(sorted(self.session.glob('receipt-*.json'))[-1].read_text())

    def test_enable_disable_restore_keeps_immutable_original(self):
        observed = self.call('inspect')
        self.assertEqual(observed['status'], 'OBSERVED')
        self.assertFalse((self.session / 'original.json').exists())
        enabled = self.call('enable', mode='fog_only')
        self.assertEqual(enabled['status'], 'PASS')
        self.assertEqual(self.values['FSSSSceneColorScatteringAmountScale'], 0)
        self.assertTrue(self.values['bEnableFSSS'])
        self.assertEqual(self.cvars['r.Fog.ScreenSpaceScattering.TAA'], 0)
        original_bytes = (self.session / 'original.json').read_bytes()
        self.call('enable', mode='fog_and_scene')
        self.call('disable')
        self.assertFalse(self.values['bEnableFSSS'])
        restored = self.call('restore')
        self.assertEqual(self.values, self.initial_values)
        self.assertEqual(self.cvars, self.initial_cvars)
        self.assertEqual((self.session / 'original.json').read_bytes(), original_bytes)
        self.assertTrue(restored['scene_unchanged'] and restored['map_unsaved'])
        self.assertFalse(restored['cvar_priority_flags_restored'])

    def test_external_edit_is_not_overwritten(self):
        self.call('enable')
        self.values['FSSSSpreadScale'] = .9
        with self.assertRaises(RuntimeError):
            self.call('disable')
        self.assertEqual(self.values['FSSSSpreadScale'], .9)
        self.assertIn('CONTROL_STATE_DIVERGED', self.latest()['error'])
        self.call('restore', force_restore=True)
        self.assertEqual(self.values, self.initial_values)

    def test_partial_setter_failure_rolls_back(self):
        self.fail_key = 'FSSSSceneColorScatteringAmountScale'
        with self.assertRaises(RuntimeError):
            self.call('enable')
        self.assertEqual(self.values, self.initial_values)
        self.assertEqual(self.cvars, self.initial_cvars)
        failed = self.latest()
        self.assertEqual(failed['status'], 'FAIL')
        self.assertTrue(failed['rollback_controls_restored'])
        self.assertEqual(len(list(self.session.glob('intent-*.json'))), 1)

    def test_wrong_world_fails_before_snapshot_or_mutation(self):
        self.world_name = '/Game/Other.Other'
        with self.assertRaises(RuntimeError):
            self.call('enable')
        self.assertFalse((self.session / 'original.json').exists())
        self.assertEqual(self.values, self.initial_values)
        self.assertFalse(self.commands)

    def test_ambiguous_first_fog_fails_before_mutation(self):
        self.actor_count = 2
        with self.assertRaises(RuntimeError):
            self.call('enable')
        self.assertIn('AMBIGUOUS_FIRST_HEIGHT_FOG', self.latest()['error'])
        self.assertEqual(self.values, self.initial_values)

    def test_python_property_aliases_and_separate_only(self):
        self.use_aliases = True
        receipt = self.call('enable', mode='separate_only')
        self.assertEqual(receipt['resolved_property_names']['enabled'], 'enable_fsss')
        self.assertFalse(self.values['bEnableFSSS'])
        self.assertEqual(self.cvars['r.Fog.SeparateComposition'], 1)
        self.call('restore')
        self.assertEqual(self.values, self.initial_values)


if __name__ == '__main__':
    unittest.main()
