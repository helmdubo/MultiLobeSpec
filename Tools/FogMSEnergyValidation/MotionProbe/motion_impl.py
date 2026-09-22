"""Temporary native viewport motion probe. Never saves a map or changes rendering CVars.

Screenshot timing is an interval: Python request -> file observed, NOT exact GPU frames.
Each delay repeats the motion so asynchronous screenshots cannot replace each other.
"""
from __future__ import annotations
import hashlib
import json
import math
import pathlib
import re
import sys
import time
import traceback

ROOT = pathlib.Path(__file__).resolve().parent
CONTROLLER = None
ANIMATION = ('animate_density', 'use_manual_animation_time', 'manual_animation_time', 'animation_time_offset')
PROTECTED = ('enabled', 'density_enabled', 'density', 'density_albedo', 'density_texture', 'density_channel',
             'world_texture_size', 'threshold', 'softness', 'detail_strength', 'detail_scale', 'detail_second_octave',
             'density_wind_velocity', 'density_detail_velocity', 'density_evolution_velocity', 'wind_speed', 'edge_flow_speed',
             'scattering_mode', 'angular_quality', 'transport_iterations')


def atomic_json(path, value):
    temporary = path.with_suffix(path.suffix + '.tmp')
    temporary.write_text(json.dumps(value, indent=2, ensure_ascii=False, allow_nan=False), encoding='utf-8')
    temporary.replace(path)


def close_values(a, b, tolerance=0.002):
    if isinstance(a, dict) and isinstance(b, dict):
        return a.keys() == b.keys() and all(close_values(a[k], b[k], tolerance) for k in a)
    if isinstance(a, list) and isinstance(b, list):
        return len(a) == len(b) and all(close_values(x, y, tolerance) for x, y in zip(a, b))
    if isinstance(a, (int, float)) and isinstance(b, (int, float)):
        return abs(a-b) <= tolerance
    return a == b


def launch():
    global CONTROLLER
    import unreal
    config = json.loads((ROOT/'motion_config.json').read_text(encoding='utf-8-sig'))
    required_game_view = config.get('force_game_view', True)
    if not isinstance(required_game_view, bool):
        raise ValueError('force_game_view must be a boolean')
    action = config.get('action', 'run')
    if action == 'stop':
        if CONTROLLER is None or CONTROLLER.done:
            raise RuntimeError('No active probe; use action=restore with the original id after a restart.')
        CONTROLLER.finish('STOPPED', 'Operator requested stop')
        return
    if action not in ('run', 'restore'):
        raise ValueError('action must be run, stop or restore')
    if CONTROLLER is not None and not CONTROLLER.done:
        raise RuntimeError('A motion probe is already active')
    if not re.fullmatch(r'[A-Za-z0-9_-]+', config['id']):
        raise ValueError('Unsafe run id')
    evidence = pathlib.Path(config['evidence_root'])
    if not evidence.is_absolute():
        raise ValueError('evidence_root must be absolute')
    run = evidence/config['id']
    if action == 'run' and run.exists():
        raise RuntimeError('Immutable evidence directory exists; choose a fresh id')

    levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    actor_api = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    if not world or world.get_path_name() != config['world'] or levels.is_in_play_in_editor():
        raise RuntimeError('Wrong map or PIE; use the existing editor FogMS_Box map')
    viewport = levels.get_active_viewport_config_key()
    if levels.get_pilot_level_actor(viewport):
        raise RuntimeError('Unpilot the viewport before this probe; no camera actor is modified')
    all_actors = actor_api.get_all_level_actors()
    boxes = [a for a in all_actors if a.get_class().get_name() == 'FogMSBoxVolume']
    if len(boxes) != 1 or boxes[0].get_actor_label() != config['box_label']:
        raise RuntimeError('Expected the single authored FogMS Box')
    box = boxes[0]
    perf = unreal.get_default_object(unreal.load_class(None, '/Script/UnrealEd.EditorPerformanceSettings'))
    autosave = unreal.get_default_object(unreal.load_class(None, '/Script/UnrealEd.EditorLoadingSavingSettings'))

    def value(v):
        if v is None or isinstance(v, (str, bool, int, float)):
            return v
        if isinstance(v, (list, tuple)):
            return [value(x) for x in v]
        if isinstance(v, unreal.Object):
            return v.get_path_name()
        for fields in (('pitch', 'yaw', 'roll'), ('r', 'g', 'b', 'a'), ('x', 'y', 'z')):
            if all(hasattr(v, k) for k in fields):
                return [float(getattr(v, k)) for k in fields]
        return str(v)

    def camera():
        c = levels.get_level_viewport_camera_info(viewport)
        if len(c) == 3:
            if not c[0]:
                raise RuntimeError('Camera API returned false')
            c = c[1:]
        f = levels.get_level_viewport_fov(viewport)
        if isinstance(f, (tuple, list)):
            if len(f) > 1 and not f[0]:
                raise RuntimeError('FOV API returned false')
            f = f[-1]
        return {'position': value(c[0]), 'rotation': value(c[1]), 'fov': float(f)}

    def set_camera(c):
        # UE Python Rotator's positional order is not pitch/yaw/roll.
        p, y, r = c['rotation']
        levels.set_level_viewport_camera_info(unreal.Vector(*c['position']), unreal.Rotator(pitch=p, yaw=y, roll=r), viewport)
        levels.set_level_viewport_fov(c['fov'], viewport)
        actual = camera()
        if not close_values(actual, c):
            raise RuntimeError('CAMERA_API_ROUND_TRIP: requested='+str(c)+' actual='+str(actual))

    def game_view():
        result = levels.editor_get_game_view(viewport)
        if not isinstance(result, bool):
            raise RuntimeError('GAME_VIEW_API_CONTRACT: expected bool, got '+str(result))
        return result

    def set_game_view(enabled):
        levels.editor_set_game_view(enabled, viewport)
        actual = game_view()
        if actual != enabled:
            raise RuntimeError('GAME_VIEW_API_ROUND_TRIP: requested='+str(enabled)+' actual='+str(actual))

    def authored():
        result = []
        for a in actor_api.get_all_level_actors():
            row = {'path': a.get_path_name(), 'location': value(a.get_actor_location()),
                   'rotation': value(a.get_actor_rotation()), 'scale': value(a.get_actor_scale3d()), 'hidden': a.is_hidden_ed()}
            row['lights'] = [{'path': c.get_path_name(), 'intensity': value(c.get_editor_property('intensity')),
                              'color': value(c.get_editor_property('light_color'))} for c in a.get_components_by_class(unreal.LightComponent)]
            result.append(row)
        return {'actors': sorted(result, key=lambda x: x['path']),
                'box': {p: value(box.get_editor_property(p)) for p in PROTECTED}}

    def exposure():
        matches = [a for a in actor_api.get_all_level_actors() if a.get_actor_label() == config['exposure_label']]
        if len(matches) != 1:
            raise RuntimeError('Expected existing fixed-exposure actor; probe never changes exposure')
        a = matches[0]
        settings = a.get_editor_property('settings')
        fields = ('auto_exposure_method', 'auto_exposure_min_brightness', 'auto_exposure_max_brightness',
                  'override_auto_exposure_method', 'override_auto_exposure_min_brightness', 'override_auto_exposure_max_brightness')
        data = {p: value(settings.get_editor_property(p)) for p in fields}
        manual = data['override_auto_exposure_method'] and 'MANUAL' in str(data['auto_exposure_method']).upper()
        locked = data['override_auto_exposure_min_brightness'] and data['override_auto_exposure_max_brightness'] and data['auto_exposure_min_brightness'] == data['auto_exposure_max_brightness']
        if not (manual or locked):
            raise RuntimeError('Exposure actor is not demonstrably fixed')
        return {'path': a.get_path_name(), 'settings': data}

    map_file = pathlib.Path(unreal.Paths.project_dir())/'Content/FogMS_Test/FogMS_Box.umap'
    def map_sha():
        return hashlib.sha256(map_file.read_bytes()).hexdigest()

    original = json.loads((run/'original.json').read_text()) if action == 'restore' else {
        'world': world.get_path_name(), 'box': box.get_path_name(), 'viewport': str(viewport), 'camera': camera(),
        'game_view': game_view(),
        'animation': {p: value(box.get_editor_property(p)) for p in ANIMATION},
        'throttle': perf.get_editor_property('bThrottleCPUWhenNotForeground'),
        'autosave': autosave.get_editor_property('bAutoSaveEnable'), 'authored': authored(), 'exposure': exposure(),
        'map_sha256': map_sha()}
    if original['world'] != world.get_path_name() or original['box'] != box.get_path_name() or original['viewport'] != str(viewport):
        raise RuntimeError('Restore requires original world, Box and active viewport')
    if 'game_view' not in original or not isinstance(original['game_view'], bool):
        raise RuntimeError('GAME_VIEW_RESTORE_UNAVAILABLE: original snapshot has no boolean game_view; '
                           'exact show-mode restoration cannot be inferred')
    target_camera = original['camera']
    if config.get('reference_camera_file'):
        target_camera = json.loads(pathlib.Path(config['reference_camera_file']).read_text(encoding='utf-8-sig'))['camera']
    if (set(target_camera) != {'position', 'rotation', 'fov'} or len(target_camera['position']) != 3
            or len(target_camera['rotation']) != 3 or not 1 < target_camera['fov'] < 170
            or not all(math.isfinite(x) for x in [*target_camera['position'], *target_camera['rotation'], target_camera['fov']])):
        raise ValueError('Invalid reference camera')

    class Probe:
        def __init__(self):
            self.done = False; self.handle = None; self.errors = []; self.records = []; self.expected = camera()
            self.started = time.monotonic(); self.last_frame = -1; self.sequence = 0; self.index = -1
            self.phase = 'worker'; self.wait_started = self.started; self.pending = None
            self.during_game_view = None
            self.jobs = [(case, delay) for case in config['cases'] for delay in [*config['sample_delays'], 'reference']]
            if any(c not in ('yaw', 'dolly', 'pan', 'fov') for c in config['cases']):
                raise ValueError('Only yaw, dolly, pan, fov are implemented; no silently skipped cases')
            self.restoration_ok = False
            self.receipt_path = run/('restore-%d.json' % time.time_ns() if action == 'restore' else 'receipt.json')

        def write(self, status, restoration=None):
            atomic_json(self.receipt_path, {'schema': 1, 'status': status, 'config': config,
                'elapsed_seconds': time.monotonic()-self.started, 'phase': self.phase, 'job_index': self.index,
                'captures': self.records, 'errors': self.errors, 'restoration': restoration,
                'viewport_environment': {'original_game_view': original['game_view'],
                    'requested_game_view': required_game_view, 'during_game_view': self.during_game_view,
                    'show_flag_scope': 'Native editor game-view mode is guarded. Individual custom show flags are not enumerated by this API.'},
                'timing': 'Approximate: screenshot rendered after request and before file observation; no render-frame callback',
                'capture_api': 'UE_MCP_Bridge.capture_screenshot target=editor', 'exact_gpu_frame_claim': False})

        def finish(self, status, error=None):
            if self.done:
                return
            self.done = True
            if error:
                self.errors.append(error)
            checks = {}
            actions = [('callback', lambda: unreal.unregister_slate_post_tick_callback(self.handle) if self.handle else None),
                       *[(p, lambda p=p: box.set_editor_property(p, original['animation'][p])) for p in ANIMATION],
                       ('density_update', box.update_density), ('camera', lambda: set_camera(original['camera'])),
                       ('game_view', lambda: set_game_view(original['game_view'])),
                       ('throttle', lambda: perf.set_editor_property('bThrottleCPUWhenNotForeground', original['throttle'])),
                       ('autosave', lambda: autosave.set_editor_property('bAutoSaveEnable', original['autosave']))]
            for name, fn in actions:
                try:
                    fn(); checks[name] = True
                except BaseException:
                    checks[name] = False; self.errors.append(name+': '+traceback.format_exc())
            validations = {'camera_exact': lambda: close_values(camera(), original['camera']),
                'game_view_exact': lambda: game_view() == original['game_view'],
                'animation_exact': lambda: {p: value(box.get_editor_property(p)) for p in ANIMATION} == original['animation'],
                'authored_exact': lambda: authored() == original['authored'], 'exposure_exact': lambda: exposure() == original['exposure'],
                'map_unsaved': lambda: map_sha() == original['map_sha256'],
                'preferences_exact': lambda: perf.get_editor_property('bThrottleCPUWhenNotForeground') == original['throttle'] and autosave.get_editor_property('bAutoSaveEnable') == original['autosave']}
            for name, fn in validations.items():
                try:
                    checks[name] = bool(fn())
                    if not checks[name] and name == 'game_view_exact':
                        self.errors.append('GAME_VIEW_RESTORE_MISMATCH: expected='+str(original['game_view'])+' actual='+str(game_view()))
                except BaseException:
                    checks[name] = False; self.errors.append(name+': '+traceback.format_exc())
            self.restoration_ok = all(checks.values())
            if not self.restoration_ok:
                status = 'FAILED'
            try:
                levels.editor_invalidate_viewports()
                restored_camera = camera()
                restored_game_view = game_view()
            except BaseException:
                restored_camera = None
                restored_game_view = None
                self.restoration_ok = False; status = 'FAILED'
                self.errors.append('Final viewport read: '+traceback.format_exc())
            restoration = {'ok': self.restoration_ok, 'checks': checks, 'actual_camera': restored_camera,
                           'expected_camera': original['camera'], 'camera_tolerance': 0.002,
                           'actual_game_view': restored_game_view, 'expected_game_view': original['game_view']}
            self.write(status, restoration)
            # Immutable completion notice: worker never repeatedly reads receipt.json.
            finished = run/'finished.json'
            if not finished.exists():
                atomic_json(finished, {'status': status, 'restoration': restoration})
            unreal.log('FOGMS_MOTION_FINISHED '+status+' '+str(self.receipt_path))

        def start(self):
            run.mkdir(parents=True, exist_ok=False)
            (run/'capture_requests').mkdir()
            (run/'capture_acks').mkdir()
            atomic_json(run/'original.json', original)
            self.write('STARTING')
            try:
                perf.set_editor_property('bThrottleCPUWhenNotForeground', False)
                autosave.set_editor_property('bAutoSaveEnable', False)
                set_game_view(required_game_view)
                self.during_game_view = game_view()
                box.freeze_density_animation()
                self.handle = unreal.register_slate_post_tick_callback(self.tick)
            except BaseException:
                self.finish('FAILED', traceback.format_exc())

        def move(self, progress):
            target = target_camera; c = {k: list(v) if isinstance(v, list) else v for k, v in target.items()}
            case, delay = self.jobs[self.index]
            if case == 'yaw':
                c['rotation'][1] -= config['yaw_degrees']*(1-progress)
            elif case == 'dolly':
                pitch, yaw = [math.radians(x) for x in target['rotation'][:2]]
                direction = [math.cos(pitch)*math.cos(yaw), math.cos(pitch)*math.sin(yaw), math.sin(pitch)]
                c['position'] = [x-config['dolly_cm']*(1-progress)*d for x, d in zip(target['position'], direction)]
            elif case == 'pan':
                yaw = math.radians(target['rotation'][1])
                right = [-math.sin(yaw), math.cos(yaw), 0.]
                c['position'] = [x-config['pan_cm']*(1-progress)*d for x, d in zip(target['position'], right)]
            elif case == 'fov':
                c['fov'] += config['fov_degrees']*(1-progress)
            set_camera(c); self.expected = camera()

        def request(self, frame):
            case, delay = self.jobs[self.index]
            self.sequence += 1
            filename = run/(case+'-'+str(delay)+'.png')
            self.pending = {'sequence': self.sequence, 'case': case, 'target_delay_frames': delay, 'stop_engine_frame': self.stop_frame,
                'request_engine_frame': frame, 'request_offset': frame-self.stop_frame, 'filename': str(filename),
                'camera': camera(), 'animation_time': float(box.get_editor_property('density_animation_time')),
                'game_view': game_view()}
            atomic_json(run/'capture_requests'/('%06d.json' % self.sequence), self.pending)
            self.wait_started = time.monotonic(); self.phase = 'capture'; self.write('RUNNING')

        def tick(self, dt):
            try:
                frame = int(unreal.SystemLibrary.get_frame_count())
                if frame == self.last_frame:
                    return
                self.last_frame = frame
                if str(levels.get_active_viewport_config_key()) != original['viewport'] or levels.get_pilot_level_actor(viewport):
                    raise RuntimeError('USER_INTERVENTION: viewport/pilot changed')
                if game_view() != required_game_view:
                    raise RuntimeError('GAME_VIEW_STATE_DIVERGED: expected='+str(required_game_view)+' actual='+str(game_view())+
                                       '; editor/game show-mode changed during native capture')
                if not close_values(camera(), self.expected):
                    raise RuntimeError('CAMERA_STATE_DIVERGED: camera/FOV differs from last observed probe state; '
                                       'this alone does not prove user input. expected='+str(self.expected)+' actual='+str(camera()))
                active_world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
                if active_world is None or active_world.get_path_name() != config['world'] or levels.is_in_play_in_editor():
                    raise RuntimeError('World changed or PIE began')
                levels.editor_invalidate_viewports()
                if time.monotonic()-self.started > config['timeout_seconds']:
                    raise TimeoutError('Motion probe deadline')
                if self.phase == 'worker':
                    if (run/'capture_worker.json').exists():
                        self.phase = 'next'
                    elif time.monotonic()-self.wait_started > 20:
                        raise TimeoutError('Start capture_worker.mjs before running the probe')
                elif self.phase == 'next':
                    self.index += 1
                    if self.index == len(self.jobs):
                        self.finish('COMPLETED'); return
                    self.move(0); self.at = frame; self.phase = 'warm'; self.write('RUNNING')
                elif self.phase == 'warm' and frame-self.at >= config['warm_frames']:
                    self.at = frame; self.phase = 'motion'
                elif self.phase == 'motion':
                    progress = min((frame-self.at)/config['motion_frames'], 1)
                    self.move(progress)
                    if progress == 1:
                        self.stop_frame = frame; self.phase = 'settle'
                elif self.phase == 'settle':
                    delay = self.jobs[self.index][1]
                    wait = config['reference_frames'] if delay == 'reference' else delay
                    if frame-self.stop_frame >= wait:
                        self.request(frame)
                elif self.phase == 'capture':
                    file = pathlib.Path(self.pending['filename']); ack_file = run/'capture_acks'/('%06d.json' % self.sequence)
                    ack = json.loads(ack_file.read_text()) if ack_file.exists() else {}
                    if ack.get('sequence') == self.sequence and 'error' in ack['result']:
                        raise RuntimeError('Native capture RPC failed: '+str(ack))
                    if file.exists() and file.stat().st_size > 24 and ack.get('sequence') == self.sequence:
                        data = file.read_bytes()
                        if data[:8] != b'\x89PNG\r\n\x1a\n':
                            raise RuntimeError('Native screenshot is not PNG')
                        if data[-12:] != b'\x00\x00\x00\x00IEND\xaeB\x60\x82':
                            return  # Renderer is still writing the asynchronous PNG.
                        self.pending.update(file_observed_engine_frame=frame, observed_offset=frame-self.stop_frame,
                            capture_offset_interval=[self.pending['request_offset'], frame-self.stop_frame],
                            png_size=[int.from_bytes(data[16:20], 'big'), int.from_bytes(data[20:24], 'big')],
                            sha256=hashlib.sha256(data).hexdigest(), capture_ack=ack)
                        self.records.append(self.pending); self.pending = None
                        self.phase = 'next'; self.write('RUNNING')
                    elif time.monotonic()-self.wait_started > 15:
                        raise TimeoutError('Native screenshot/ack not received')
            except BaseException:
                self.finish('FAILED', traceback.format_exc())

    CONTROLLER = Probe()
    if action == 'restore':
        CONTROLLER.finish('RESTORED')
    else:
        CONTROLLER.start()
