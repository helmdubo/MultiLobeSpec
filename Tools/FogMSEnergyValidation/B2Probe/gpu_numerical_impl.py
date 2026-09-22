# On-demand only: explicit UE Python invocation; no startup registration.
"""Explicit asynchronous B2 numerical probe. Never auto-runs outside UE.

Reads adjacent gpu_probe_config.json. Only writes mode/iterations, five diagnostic
CVars, throttle and autosave; restores their freshly captured values. No map save.
"""
import datetime
import hashlib
import json
import math
import pathlib
import re
import sys
import time
import traceback
import types

ROOT = pathlib.Path(__file__).resolve().parent
MODULE = '_fogms_b2_numerical_probe'
LAYOUT = 'x,y+z*N; slabs: totalJ/residual, primaryJ/openFaces, sigma_s/sigma_t, diffuse input/output/absorption/directSource'
CVARS = {
    'test': 'r.FogMS.Transport.Test', 'tau': 'r.FogMS.Transport.TestTau',
    'albedo': 'r.FogMS.Transport.TestAlbedo', 'boundary': 'r.FogMS.Transport.TestBoundary',
    'geometry': 'r.FogMS.Transport.TestGeometry'
}


def default_cases():
    result = [dict(name='uniform-tau-' + str(t).replace('.', 'p'), test=1, tau=t, albedo=1, boundary=0, geometry=0)
              for t in (.1, 1, 4, 8, 16)]
    result += [dict(name='uniform-tau4-albedo-' + str(a).replace('.', 'p'), test=1, tau=4, albedo=a, boundary=0, geometry=0)
               for a in (0, .9)]
    result += [dict(name='vacuum', test=1, tau=0, albedo=0, boundary=0, geometry=0)]
    result += [dict(name='gap-tau4-albedo-' + str(a).replace('.', 'p'), test=2, tau=4, albedo=a, boundary=1, geometry=0)
               for a in (0, .9, 1)]
    return result


def atomic_json(path, data):
    temp = path.with_suffix(path.suffix + '.tmp')
    temp.write_text(json.dumps(data, indent=2, ensure_ascii=False, allow_nan=False), encoding='utf-8')
    temp.replace(path)


def launch():
    import unreal
    config = json.loads((ROOT / 'gpu_probe_config.json').read_text(encoding='utf-8-sig'))
    action = config.get('action', 'run')
    prior = sys.modules.get(MODULE)
    if action == 'stop':
        if not prior or not getattr(prior, 'controller', None):
            raise RuntimeError('No active numerical probe. Use action=restore and its own receipt after a process restart.')
        prior.controller.finish('STOPPED', 'operator stop')
        return
    if action not in ('run', 'restore'):
        raise ValueError('action must be run, stop or restore')
    if prior and getattr(prior, 'controller', None) and not prior.controller.restoration_ok:
        raise RuntimeError('Previous probe is running or needs restoration; use action=stop first.')
    editor = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem)
    levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    actor_api = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    world = editor.get_editor_world()
    if world is None or world.get_path_name() != '/Game/FogMS_Test/FogMS_Box.FogMS_Box' or levels.is_in_play_in_editor():
        raise RuntimeError('Use the existing FogMS_Box editor world, outside PIE.')
    boxes = [a for a in actor_api.get_all_level_actors() if a.get_class().get_name() == 'FogMSBoxVolume']
    if len(boxes) != 1 or boxes[0].get_actor_label() != 'FogMS - Live Box':
        raise RuntimeError('Expected exactly the existing FogMS - Live Box actor.')
    box = boxes[0]
    component = box.get_component_by_class(unreal.BoxComponent)
    performance = unreal.get_default_object(unreal.load_class(None, '/Script/UnrealEd.EditorPerformanceSettings'))
    autosave = unreal.get_default_object(unreal.load_class(None, '/Script/UnrealEd.EditorLoadingSavingSettings'))
    map_path = pathlib.Path(unreal.Paths.project_dir()) / 'Content/FogMS_Test/FogMS_Box.umap'

    def val(v):
        if v is None or isinstance(v, (bool, int, float, str)):
            return v
        if isinstance(v, unreal.Object):
            return v.get_path_name()
        for names in (('pitch', 'yaw', 'roll'), ('r', 'g', 'b', 'a'), ('x', 'y', 'z')):
            if all(hasattr(v, n) for n in names):
                return [float(getattr(v, n)) for n in names]
        return str(v)

    def cvars():
        result = {key: unreal.SystemLibrary.get_console_variable_string_value(name) for key, name in CVARS.items()}
        for item in result.values():
            if not item or not math.isfinite(float(item)):
                raise RuntimeError('Transport diagnostic CVars are missing or invalid; install/compile B2 first.')
        return result

    def geometry():
        return {'center': val(component.get_world_location()), 'rotation': val(component.get_world_rotation()),
                'scale': val(component.get_world_scale()), 'half_extent_cm': val(component.get_scaled_box_extent())}

    def scene_guard():
        actors = [{'path': a.get_path_name(), 'hidden': a.is_hidden_ed(), 'location': val(a.get_actor_location()),
                   'rotation': val(a.get_actor_rotation()), 'scale': val(a.get_actor_scale3d())}
                  for a in actor_api.get_all_level_actors()]
        props = ['enabled', 'density_enabled', 'density_texture', 'density_channel', 'world_aligned_texture',
                 'world_texture_size', 'tile_scale', 'threshold', 'softness', 'density', 'density_albedo',
                 'density_edge_feather', 'detail_strength', 'detail_scale', 'detail_second_octave',
                 'spatial_strength', 'spatial_distance', 'indirect_shadow_strength']
        return {'actors': sorted(actors, key=lambda a: a['path']), 'box': geometry(),
                'properties': {p: val(box.get_editor_property(p)) for p in props}}

    def command(name, scalar):
        # All inputs are locally captured/generated numeric CVars, never arbitrary source text.
        value = float(scalar)
        if not math.isfinite(value):
            raise ValueError('Nonfinite CVar')
        unreal.SystemLibrary.execute_console_command(world, name + ' ' + str(scalar))

    def restore(original):
        errors, checks = [], {}
        def attempt(name, fn):
            try:
                fn()
            except BaseException:
                errors.append(name + ': ' + traceback.format_exc())
        def actor_restore():
            current = editor.get_editor_world()
            if current is None or current.get_path_name() != original['world'] or box.get_path_name() != original['actor_path']:
                raise RuntimeError('Original editor world/actor must be active before restoration.')
            box.set_editor_property('scattering_mode', getattr(unreal.FogMSScatteringMode, original['mode_name']))
            box.set_editor_property('transport_iterations', original['iterations'])
        attempt('actor mode/iterations', actor_restore)
        for key, old in original['cvars'].items():
            attempt(key, lambda k=key, v=old: command(CVARS[k], v))
        attempt('throttle', lambda: performance.set_editor_property('bThrottleCPUWhenNotForeground', original['throttle']))
        attempt('autosave', lambda: autosave.set_editor_property('bAutoSaveEnable', original['autosave']))
        def verify():
            checks['mode'] = str(box.get_editor_property('scattering_mode')) == original['mode_repr']
            checks['iterations'] = int(box.get_editor_property('transport_iterations')) == original['iterations']
            checks['cvars'] = cvars() == original['cvars']
            checks['throttle'] = bool(performance.get_editor_property('bThrottleCPUWhenNotForeground')) == original['throttle']
            checks['autosave'] = bool(autosave.get_editor_property('bAutoSaveEnable')) == original['autosave']
            checks['map_unchanged'] = hashlib.sha256(map_path.read_bytes()).hexdigest() == original['map_sha256']
            checks['authored_scene_unchanged'] = scene_guard() == original['scene_guard']
        attempt('verify restoration', verify)
        return {'ok': not errors and len(checks) == 7 and all(checks.values()), 'checks': checks, 'errors': errors}

    if action == 'restore':
        name = config.get('restore_receipt', '')
        if pathlib.Path(name).name != name or not name.startswith('gpu_numerical-') or not name.endswith('.json'):
            raise ValueError('restore_receipt must name a local gpu_numerical-*.json produced by this harness.')
        path = ROOT / name
        saved = json.loads(path.read_text(encoding='utf-8-sig'))
        saved['restoration'] = restore(saved['original'])
        saved['restoration_ok'] = saved['restoration']['ok']
        saved['status'] = 'RESTORED' if saved['restoration_ok'] else 'RESTORATION_INCOMPLETE'
        atomic_json(path, saved)
        unreal.log('FOGMS_B2_NUMERICAL_RESTORED ' + str(path))
        return

    case_id = config.get('id', 'numerical-' + datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%S'))
    if not re.fullmatch(r'[A-Za-z0-9_-]+', case_id):
        raise ValueError('Unsafe/invalid probe id')
    cases = config.get('cases') or default_cases()
    if len({c['name'] for c in cases}) != len(cases):
        raise ValueError('Case names must be unique')
    for c in cases:
        if not re.fullmatch(r'[A-Za-z0-9_-]+', c['name']) or int(c['test']) not in (1, 2):
            raise ValueError('Only numerical Test1/Test2 cases are supported')
        if c['boundary'] not in (0, 1) or c['geometry'] not in (0, 1):
            raise ValueError('Invalid boundary/geometry')
        if not 0 <= float(c['tau']) <= 100 or not 0 <= float(c['albedo']) <= 1:
            raise ValueError('Invalid tau/albedo')
    iterations = int(config.get('iterations', 24))
    if not 4 <= iterations <= 64:
        raise ValueError('Iterations must be 4..64')
    warmup = max(4, int(config.get('warmup_frames', 12)))
    wait_frames = max(4, int(config.get('wait_frames', 4)))
    timeout = max(5., float(config.get('dump_timeout_seconds', 45)))
    duration = max(timeout, float(config.get('max_duration_seconds', 900)))
    receipt = ROOT / ('gpu_numerical-' + case_id + '.json')
    if receipt.exists():
        raise RuntimeError('Use a new id; a probe receipt already exists.')
    # Optional provenance only; restoration always uses this run's fresh snapshot.
    baseline_name = config.get('baseline_reference')
    baseline, baseline_sha256, baseline_status = None, None, 'not_requested'
    if baseline_name:
        baseline = pathlib.Path(baseline_name)
        if not baseline.is_absolute():
            baseline = ROOT / baseline
        if baseline.is_file():
            baseline_bytes = baseline.read_bytes()
            baseline_json = json.loads(baseline_bytes.decode('utf-8-sig'))
            if baseline_json.get('world') != world.get_path_name():
                raise RuntimeError('Reference inspection is from a different map.')
            baseline_sha256 = hashlib.sha256(baseline_bytes).hexdigest()
            baseline_status = 'verified_world'
        else:
            baseline_status = 'missing_optional'
            unreal.log('FOGMS_B2_BASELINE_OPTIONAL_MISSING ' + str(baseline))
    transport = getattr(unreal.FogMSScatteringMode, 'TRANSPORT')
    original_mode = box.get_editor_property('scattering_mode')
    match = re.search(r'\.([A-Z][A-Z_0-9]*):', str(original_mode))
    if match is None:
        raise RuntimeError('Cannot serialize the exact current enum for recovery.')
    original = {
        'world': world.get_path_name(), 'actor_path': box.get_path_name(),
        'mode_name': match.group(1), 'mode_repr': str(original_mode),
        'iterations': int(box.get_editor_property('transport_iterations')),
        'cvars': cvars(), 'throttle': bool(performance.get_editor_property('bThrottleCPUWhenNotForeground')),
        'autosave': bool(autosave.get_editor_property('bAutoSaveEnable')),
        'map_sha256': hashlib.sha256(map_path.read_bytes()).hexdigest(), 'scene_guard': scene_guard()
    }

    class Probe:
        def __init__(self):
            self.handle, self.pending = None, None
            self.finished = self.restoration_ok = self.in_tick = False
            self.started, self.engine_start = time.monotonic(), int(unreal.SystemLibrary.get_frame_count())
            self.frames, self.index = 0, -1
            self.records, self.errors, self.restoration = [], [], None

        def write(self, status):
            atomic_json(receipt, {
                'schema': 'fogms_b2_numerical_probe_v1', 'id': case_id, 'status': status,
                'timestamp_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
                'engine_frames_elapsed': int(unreal.SystemLibrary.get_frame_count()) - self.engine_start,
                'slate_callbacks': self.frames, 'elapsed_seconds': time.monotonic() - self.started,
                'baseline_reference': str(baseline) if baseline else None, 'baseline_sha256': baseline_sha256,
                'baseline_status': baseline_status,
                'original': original, 'iterations': iterations, 'planned_cases': cases, 'cases': self.records,
                'errors': self.errors, 'restoration': self.restoration, 'restoration_ok': self.restoration_ok,
                'note': 'Readback stalls deliberately; timings are not a GPU performance benchmark.'
            })

        def start(self):
            self.write('PREPARED')  # Durable original state precedes the first mutation.
            autosave.set_editor_property('bAutoSaveEnable', False)
            performance.set_editor_property('bThrottleCPUWhenNotForeground', False)
            box.set_editor_property('transport_iterations', iterations)
            box.set_editor_property('scattering_mode', transport)
            self.begin_case()
            self.handle = unreal.register_slate_post_tick_callback(self.tick)
            self.write('RUNNING')
            unreal.log('FOGMS_B2_NUMERICAL_ACCEPTED ' + str(receipt) + ' (asynchronous; wait for COMPLETED)')

        def guard(self):
            if editor.get_editor_world().get_path_name() != original['world'] or levels.is_in_play_in_editor():
                raise RuntimeError('Editor world/PIE changed during probe')
            if scene_guard() != original['scene_guard']:
                raise RuntimeError('Authored scene changed externally during the numerical probe')
            if box.get_editor_property('scattering_mode') != transport or int(box.get_editor_property('transport_iterations')) != iterations:
                raise RuntimeError('Transport mode/iteration controls changed externally')
            values = cvars()
            for key, name in CVARS.items():
                expected = float(cases[self.index][key])
                if not math.isclose(float(values[key]), expected, rel_tol=2.e-6, abs_tol=1.e-7):
                    raise RuntimeError('Diagnostic CVar mismatch: ' + name)
            return values

        def begin_case(self):
            self.index += 1
            if self.index == len(cases):
                self.finish('COMPLETED', 'all cases captured')
                return
            c = cases[self.index]
            for key, name in CVARS.items():
                command(name, c[key])
            self.records.append({'index': self.index, 'case': c.copy(), 'box': geometry(),
                                 'applied_engine_frame': int(unreal.SystemLibrary.get_frame_count())})
            self.guard()
            self.phase, self.phase_frame, self.phase_slate = 'wait_barrier', int(unreal.SystemLibrary.get_frame_count()), self.frames
            self.write('RUNNING')

        def issue(self, role):
            values = self.guard()
            prefix = ROOT / ('gpu_' + case_id + '-%02d-' % self.index + cases[self.index]['name'] + '-' + role)
            if any(pathlib.Path(str(prefix) + s).exists() for s in ('.json', '.rgba32f')):
                raise RuntimeError('Refusing existing dump prefix')
            self.pending = {'role': role, 'prefix': str(prefix), 'cvars': values,
                            'requested_engine_frame': int(unreal.SystemLibrary.get_frame_count()),
                            'requested_slate_callback': self.frames}
            self.phase, self.deadline = 'readback', time.monotonic() + timeout
            unreal.SystemLibrary.execute_console_command(world, 'FogMS.DumpSpatial "' + prefix.as_posix() + '"')

        def receive(self):
            path = pathlib.Path(self.pending['prefix'] + '.json')
            if not path.exists():
                return
            try:
                meta = json.loads(path.read_text(encoding='utf-8-sig'))
            except (OSError, json.JSONDecodeError):
                return
            if meta.get('success') is not True or meta.get('domain') != 'transport' or meta.get('format') != 'RGBA32F_LE':
                raise RuntimeError('Missing current Transport dump: ' + str(meta))
            n = int(meta['grid'])
            payload = pathlib.Path(self.pending['prefix'] + '.rgba32f')
            if meta.get('layout') != LAYOUT or meta.get('width') != n or meta.get('height') != 4*n*n or meta.get('bytes') != 4*n**3*16:
                raise RuntimeError('Unexpected four-slab layout')
            if not payload.is_file() or payload.stat().st_size != meta['bytes']:
                raise RuntimeError('Readback payload is absent or incomplete')
            c = cases[self.index]
            for key, field in [('test', 'test'), ('tau', 'testTau'), ('albedo', 'testAlbedo')]:
                if not math.isclose(float(meta[field]), float(c[key]), rel_tol=2.e-6, abs_tol=1.e-7):
                    raise RuntimeError('Dump does not match requested ' + key)
            for key, field in [('geometry', 'testGeometry'), ('boundary', 'testBoundary')]:
                if field in meta and int(meta[field]) != c[key]:
                    raise RuntimeError('Dump does not match requested ' + key)
            if int(meta['iterations']) != iterations or int(meta['directions']) != 6:
                raise RuntimeError('Iteration/quadrature mismatch')
            stamp, source = int(meta['dumpRenderFrame']), int(meta['sourceProducedRenderFrame'])
            if not 0 <= ((stamp-source) & 0xffffffff) <= 2:
                raise RuntimeError('Dump contains stale source frames')
            record = dict(self.pending)
            record.update(metadata=meta, completed_engine_frame=int(unreal.SystemLibrary.get_frame_count()),
                          completed_slate_callback=self.frames, cvars_after=self.guard(),
                          rgba_sha256=hashlib.sha256(payload.read_bytes()).hexdigest())
            point = self.records[-1]
            point[record['role']] = record
            self.pending = None
            if record['role'] == 'barrier':
                self.phase, self.phase_frame, self.phase_slate = 'wait_measurement', int(unreal.SystemLibrary.get_frame_count()), self.frames
            else:
                before = point['barrier']['metadata']
                d = (stamp-int(before['dumpRenderFrame'])) & 0xffffffff
                s = (source-int(before['sourceProducedRenderFrame'])) & 0xffffffff
                point['render_frames_after_barrier'], point['produced_frames_after_barrier'] = d, s
                if not (2 <= d < 2**31 and 2 <= s < 2**31):
                    raise RuntimeError('Need at least two fresh render/producer frames after barrier')
                self.begin_case()
            if not self.finished:
                self.write('RUNNING')

        def tick(self, dt):
            if self.finished or self.in_tick:
                return
            self.in_tick = True
            try:
                self.frames += 1
                if time.monotonic()-self.started > duration:
                    raise TimeoutError('Probe duration exceeded')
                levels.editor_invalidate_viewports()
                if self.phase == 'readback':
                    self.receive()
                    if self.phase == 'readback' and time.monotonic() > self.deadline:
                        raise TimeoutError('Native dump completion JSON did not arrive')
                elif self.phase in ('wait_barrier', 'wait_measurement'):
                    wait = warmup if self.phase == 'wait_barrier' else wait_frames
                    if int(unreal.SystemLibrary.get_frame_count())-self.phase_frame >= wait and self.frames-self.phase_slate >= 2:
                        self.issue('barrier' if self.phase == 'wait_barrier' else 'measurement')
            except BaseException:
                self.errors.append(traceback.format_exc())
                self.finish('ERROR', 'callback failed')
            finally:
                self.in_tick = False

        def finish(self, status, reason):
            if self.finished and self.restoration_ok:
                return
            self.finished = True
            self.phase = 'finished'
            try:
                if self.handle is not None:
                    unreal.unregister_slate_post_tick_callback(self.handle)
                    self.handle = None
            except BaseException:
                self.errors.append(traceback.format_exc())
            finally:
                # Normal completion, callback errors and operator stop share the same cleanup.
                self.restoration = restore(original)
                self.restoration_ok = self.restoration['ok'] and self.handle is None
                self.restoration['reason'] = reason
                if not self.restoration_ok:
                    status = 'RESTORATION_INCOMPLETE'
                self.write(status)
                unreal.log('FOGMS_B2_NUMERICAL_FINISHED ' + status + ' ' + str(receipt))

    module = types.ModuleType(MODULE)
    module.controller = Probe()
    module.stop = lambda: module.controller.finish('STOPPED', 'operator stop')
    sys.modules[MODULE] = module
    try:
        module.controller.start()
    except BaseException:
        module.controller.errors.append(traceback.format_exc())
        module.controller.finish('ERROR', 'startup failed')
        raise


if __name__ == '__main__':
    launch()
