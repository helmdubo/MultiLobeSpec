"""Continuous native editor W/S capture; no render-frame accuracy claim or map save.

Only this module owns its callback globals. The bridge may reuse runpy globals.
The camera moves even while a screenshot request is outstanding.
"""
from __future__ import annotations
import copy
import datetime
import hashlib
import json
import math
import pathlib
import re
import time
import traceback

CONTROLLER = None
ANIMATION = ('animate_density', 'use_manual_animation_time', 'manual_animation_time', 'animation_time_offset')
REFERENCE = ('initialized', 'time', 'displacement0', 'displacement1', 'displacement2', 'velocity0', 'velocity1', 'velocity2')
PROTECTED = ('enabled', 'density_enabled', 'density', 'density_albedo', 'density_texture', 'density_channel',
             'world_texture_size', 'threshold', 'softness', 'detail_strength', 'detail_scale', 'detail_second_octave',
             'density_wind_velocity', 'density_detail_velocity', 'density_evolution_velocity', 'wind_speed', 'edge_flow_speed',
             'density_motion_mode', 'texture_offset_world', 'scattering_mode', 'angular_quality', 'transport_iterations',
             'world_aligned_texture', 'tile_scale', 'density_edge_feather', 'feather_distance',
             'authored_sun_shadow', 'cast_sun_shadow', 'filtered_sun_shadow', 'shadow_filter_sigma',
             'filter_sun_inside_volume', 'indirect_shadowing', 'indirect_shadow_strength', 'indirect_shadow_steps')
CVAR_OFF = {'r.FogMS.SSFS': 0, 'r.Fog.ScreenSpaceScattering': 0,
            'r.Fog.SeparateComposition': 0, 'r.Fog.ScreenSpaceScattering.TAA': 0}
CVAR_MODE = 'r.FogMS.ViewIntegration'
EXPOSURE = ('auto_exposure_method', 'auto_exposure_min_brightness', 'auto_exposure_max_brightness',
            'override_auto_exposure_method', 'override_auto_exposure_min_brightness', 'override_auto_exposure_max_brightness',
            'auto_exposure_bias', 'override_auto_exposure_bias')


def utc():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


def plain(value):
    """Never retain UE struct wrappers: they can alias live UPROPERTY memory."""
    if value is None or isinstance(value, (str, bool, int, float)):
        return value
    if isinstance(value, (list, tuple)):
        return [plain(x) for x in value]
    if hasattr(value, 'get_path_name'):
        return value.get_path_name()
    for fields in (('pitch', 'yaw', 'roll'), ('r', 'g', 'b', 'a'), ('x', 'y', 'z')):
        if all(hasattr(value, key) for key in fields):
            return [float(getattr(value, key)) for key in fields]
    return str(value)


def close(a, b, tolerance=0.002):
    if isinstance(a, dict) and isinstance(b, dict):
        return a.keys() == b.keys() and all(close(a[k], b[k], tolerance) for k in a)
    if isinstance(a, (list, tuple)) and isinstance(b, (list, tuple)):
        return len(a) == len(b) and all(close(x, y, tolerance) for x, y in zip(a, b))
    if isinstance(a, bool) or isinstance(b, bool):
        return type(a) is type(b) and a == b
    if isinstance(a, (int, float)) and isinstance(b, (int, float)):
        return math.isfinite(a) and math.isfinite(b) and abs(a-b) <= tolerance
    return a == b


def publish(path, value):
    """Publish once, atomically. No Windows reader ever blocks replacement of a live file."""
    path = pathlib.Path(path)
    if path.exists():
        raise RuntimeError('Immutable evidence already exists: '+str(path))
    temporary = path.with_name(path.name+'.tmp')
    with temporary.open('x', encoding='utf-8') as output:
        json.dump(value, output, indent=2, ensure_ascii=False, allow_nan=False)
    # Windows rename fails if destination exists; do not overwrite evidence.
    temporary.rename(path)


def configuration(source):
    c = copy.deepcopy(source) if isinstance(source, dict) else json.loads(pathlib.Path(source).read_text(encoding='utf-8-sig'))
    if c.get('action', 'run') == 'stop':
        return c
    if c.get('action', 'run') != 'run':
        raise ValueError('Only action=run or stop; recovery is intentionally not an unconditional stale restore')
    if not re.fullmatch(r'[A-Za-z0-9_-]+', c['id']) or not pathlib.Path(c['evidence_root']).is_absolute():
        raise ValueError('Fresh safe id and absolute evidence_root required')
    defaults = {'world': '/Game/FogMS_Test/FogMS_Box.FogMS_Box', 'box_label': 'FogMS - Live Box',
                'exposure_label': 'FogMS - Fixed Exposure', 'distance_cm': 20000., 'leg_frames': 180,
                'warmup_frames': 60, 'captures_per_leg': 16, 'timeout_seconds': 240,
                'worker_timeout_seconds': 20, 'capture_timeout_seconds': 20}
    for key, val in defaults.items():
        c.setdefault(key, val)
    for key, low, high in [('leg_frames', 60, 600), ('warmup_frames', 16, 240), ('captures_per_leg', 2, 16)]:
        if type(c[key]) is not int or not low <= c[key] <= high:
            raise ValueError(key+' outside bounded integer range')
    for key in ('distance_cm', 'timeout_seconds', 'worker_timeout_seconds', 'capture_timeout_seconds'):
        if not isinstance(c[key], (int, float)) or not math.isfinite(c[key]) or c[key] <= 0:
            raise ValueError('Positive finite '+key+' required')
    if c['distance_cm'] > 100000 or c['timeout_seconds'] > 600:
        raise ValueError('Distance/timeout exceeds bounded probe scope')
    return c


def endpoints(camera, distance):
    near = copy.deepcopy(camera)
    p, y = [math.radians(x) for x in camera['rotation'][:2]]
    forward = [math.cos(p)*math.cos(y), math.cos(p)*math.sin(y), math.sin(p)]
    far = copy.deepcopy(camera)
    far['position'] = [x-distance*d for x, d in zip(camera['position'], forward)]
    return far, near


def interpolate(a, b, progress):
    result = copy.deepcopy(a)
    t = min(1., max(0., progress))
    result['position'] = [x+(y-x)*t for x, y in zip(a['position'], b['position'])]
    return result


def capture_schedule(frames, count):
    # Margin for async readback; capture is NOT allowed to pause the camera.
    first, last = max(3, round(frames*.04)), max(4, round(frames*.88))
    return sorted(set(round(first+(last-first)*i/(count-1)) for i in range(count)))


class Native:
    def __init__(self, config):
        import unreal
        self.u = unreal
        self.config = config
        self.levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
        self.actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
        self.editor = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem)
        self.world = self.editor.get_editor_world()
        if not self.world or self.world.get_path_name() != config['world'] or self.levels.is_in_play_in_editor():
            raise RuntimeError('Wrong existing editor map or PIE')
        self.viewport = self.levels.get_active_viewport_config_key()
        if self.levels.get_pilot_level_actor(self.viewport):
            raise RuntimeError('Unpilot viewport first; camera actors are never changed')
        boxes = [a for a in self.actors.get_all_level_actors() if a.get_class().get_name() == 'FogMSBoxVolume']
        if len(boxes) != 1 or boxes[0].get_actor_label() != config['box_label']:
            raise RuntimeError('Expected exactly one authored FogMS Box')
        self.box = boxes[0]
        self.perf = unreal.get_default_object(unreal.load_class(None, '/Script/UnrealEd.EditorPerformanceSettings'))
        self.autosave = unreal.get_default_object(unreal.load_class(None, '/Script/UnrealEd.EditorLoadingSavingSettings'))
        map_name = config['world'].split('.')[0]
        if not map_name.startswith('/Game/'):
            raise RuntimeError('Only a project /Game map is supported')
        self.map_file = pathlib.Path(unreal.Paths.project_dir())/'Content'/(map_name[6:]+'.umap')
        if not self.map_file.is_file():
            raise RuntimeError('Saved source map missing; no level will be created')

    def context_ok(self):
        world = self.editor.get_editor_world()
        return bool(world and world.get_path_name() == self.config['world'] and not self.levels.is_in_play_in_editor()
                    and self.levels.get_active_viewport_config_key() == self.viewport
                    and not self.levels.get_pilot_level_actor(self.viewport))

    def camera(self):
        result = self.levels.get_level_viewport_camera_info(self.viewport)
        if len(result) == 3:
            if not result[0]:
                raise RuntimeError('Camera API returned false')
            result = result[1:]
        fov = self.levels.get_level_viewport_fov(self.viewport)
        if isinstance(fov, (tuple, list)):
            if len(fov) > 1 and not fov[0]:
                raise RuntimeError('FOV API returned false')
            fov = fov[-1]
        return {'position': plain(result[0]), 'rotation': plain(result[1]), 'fov': float(fov)}

    def set_camera(self, camera):
        p, y, r = camera['rotation']
        self.levels.set_level_viewport_camera_info(self.u.Vector(*camera['position']), self.u.Rotator(pitch=p, yaw=y, roll=r), self.viewport)
        self.levels.set_level_viewport_fov(camera['fov'], self.viewport)
        if not close(self.camera(), camera):
            raise RuntimeError('CAMERA_API_ROUND_TRIP')
        self.levels.editor_invalidate_viewports()

    def game_view(self):
        result = self.levels.editor_get_game_view(self.viewport)
        if not isinstance(result, bool):
            raise RuntimeError('Game-view API returned non-bool')
        return result

    def set_game_view(self, value):
        self.levels.editor_set_game_view(value, self.viewport)
        if self.game_view() != value:
            raise RuntimeError('GAME_VIEW_API_ROUND_TRIP')

    def get_cvar(self, name):
        return self.u.SystemLibrary.get_console_variable_int_value(name)

    def set_cvar(self, name, value):
        self.u.SystemLibrary.execute_console_command(self.world, name+' '+str(int(value)))
        if self.get_cvar(name) != value:
            raise RuntimeError('CVAR_SET_FAILED '+name)

    def preferences(self):
        return {'throttle': bool(self.perf.get_editor_property('bThrottleCPUWhenNotForeground')),
                'autosave': bool(self.autosave.get_editor_property('bAutoSaveEnable'))}

    def set_preference(self, key, value):
        obj, prop = (self.perf, 'bThrottleCPUWhenNotForeground') if key == 'throttle' else (self.autosave, 'bAutoSaveEnable')
        obj.set_editor_property(prop, value)
        if self.preferences()[key] != value:
            raise RuntimeError('PREFERENCE_SET_FAILED '+key)

    def motion(self):
        ref = self.box.get_editor_property('density_motion_reference')
        return {'animation': {p: plain(self.box.get_editor_property(p)) for p in ANIMATION},
                'reference': {p: plain(ref.get_editor_property(p)) for p in REFERENCE}}

    def restore_motion(self, motion):
        for key, value in motion['animation'].items():
            self.box.set_editor_property(key, value)
        ref = self.u.FogMSDensityMotionReference()
        for key, value in motion['reference'].items():
            ref.set_editor_property(key, self.u.Vector(*value) if isinstance(value, list) else value)
        if not self.box.restore_density_motion_reference(ref):
            raise RuntimeError('RESTORE_MOTION_REFERENCE_FAILED')
        if not close(self.motion(), motion, 1e-5):
            raise RuntimeError('MOTION_RESTORE_MISMATCH')

    def phases(self):
        mid = self.box.get_editor_property('density_component').get_material(0)
        return [plain(mid.get_vector_parameter_value('FogMS_WorldPhase'+str(i)))[:3] for i in range(3)]

    def freeze(self):
        self.box.freeze_density_animation()

    def authored(self):
        rows = []
        for actor in self.actors.get_all_level_actors():
            rows.append({'path': actor.get_path_name(), 'location': plain(actor.get_actor_location()),
                         'rotation': plain(actor.get_actor_rotation()), 'scale': plain(actor.get_actor_scale3d()),
                         'hidden': actor.is_hidden_ed(),
                         'lights': [{'path': light.get_path_name(), 'intensity': plain(light.get_editor_property('intensity')),
                                     'color': plain(light.get_editor_property('light_color'))}
                                    for light in actor.get_components_by_class(self.u.LightComponent)]})
        components = {}
        for key in ('box_component', 'wind_direction_component'):
            component = self.box.get_editor_property(key)
            props = ('relative_location', 'relative_rotation', 'relative_scale3d')
            if key == 'box_component':
                props += ('box_extent',)
            components[key] = {p: plain(component.get_editor_property(p)) for p in props}
        return {'actors': sorted(rows, key=lambda a: a['path']), 'box_components': components,
                'box': {p: plain(self.box.get_editor_property(p)) for p in PROTECTED}}

    def exposure(self):
        actors = [a for a in self.actors.get_all_level_actors() if a.get_actor_label() == self.config['exposure_label']]
        if len(actors) != 1:
            raise RuntimeError('Expected existing fixed-exposure actor')
        actor = actors[0]
        settings = actor.get_editor_property('settings')
        fields = {p: plain(settings.get_editor_property(p)) for p in EXPOSURE}
        manual = fields['override_auto_exposure_method'] and 'MANUAL' in str(fields['auto_exposure_method']).upper()
        locked = (fields['override_auto_exposure_min_brightness'] and fields['override_auto_exposure_max_brightness']
                  and fields['auto_exposure_min_brightness'] == fields['auto_exposure_max_brightness'])
        if not (manual or locked):
            raise RuntimeError('Existing exposure is not demonstrably fixed; not modified')
        return {'path': actor.get_path_name(), 'settings': fields}

    def map_sha(self):
        return hashlib.sha256(self.map_file.read_bytes()).hexdigest()

    def frame(self):
        return int(self.u.SystemLibrary.get_frame_count())

    def register(self, callback):
        return self.u.register_slate_post_tick_callback(callback)

    def unregister(self, handle):
        self.u.unregister_slate_post_tick_callback(handle)


class Probe:
    def __init__(self, config, native):
        self.c, self.n = config, native
        self.root = pathlib.Path(config['evidence_root'])/config['id']
        if self.root.exists():
            raise RuntimeError('Immutable evidence directory exists; use fresh id')
        self.done = False
        self.handle = None
        self.started = time.monotonic()
        self.errors, self.records, self.poses, self.legs, self.skipped = [], [], [], [], []
        self.owned = {'cvars': {}, 'preferences': {}}
        self.state, self.job, self.sequence = 'worker', -1, 0
        comparison = int(config.get('comparison_mode', 2))
        if comparison not in (2, 3):
            raise ValueError('comparison_mode must be 2 or 3')
        self.jobs = [(0, 'forward'), (0, 'backward'), (comparison, 'forward'), (comparison, 'backward')]
        self.pending = None
        self.expected_camera = self.n.camera()
        self.camera_intervened = False
        self.last_frame = self.n.frame()
        self.original = {'schema': 1, 'captured_utc': utc(), 'world': config['world'], 'camera': self.expected_camera,
                         'game_view': self.n.game_view(), 'motion': self.n.motion(), 'phases': self.n.phases(),
                         'preferences': self.n.preferences(), 'cvars': {p: self.n.get_cvar(p) for p in [*CVAR_OFF, CVAR_MODE]},
                         'authored': self.n.authored(), 'exposure': self.n.exposure(), 'map_sha256': self.n.map_sha()}
        self.far, self.near = endpoints(self.original['camera'], config['distance_cm'])
        self.schedule = capture_schedule(config['leg_frames'], config['captures_per_leg'])

    def start(self):
        self.root.mkdir(parents=True, exist_ok=False)
        (self.root/'capture_requests').mkdir()
        (self.root/'capture_acks').mkdir()
        (self.root/'events').mkdir()
        publish(self.root/'original.json', self.original)
        publish(self.root/'config.json', self.c)
        publish(self.root/'started.json', {'utc': utc(), 'status': 'WAITING_FOR_CAPTURE_WORKER', 'exact_gpu_frame_claim': False})
        try:
            self.handle = self.n.register(self.tick)
        except BaseException:
            self.finish('FAILED', traceback.format_exc())
        return {'id': self.c['id'], 'directory': str(self.root), 'status': self.state}

    def configure(self):
        # Snapshot is already durable before the first mutation.
        for key in self.original['preferences']:
            self.owned['preferences'][key] = False
            self.n.set_preference(key, False)
        self.owned['game_view'] = True
        self.n.set_game_view(True)
        for key, value in CVAR_OFF.items():
            self.owned['cvars'][key] = value
            self.n.set_cvar(key, value)
        before_freeze = self.n.phases()
        try:
            self.n.freeze()
        finally:
            # Even a partial native API failure must leave a restorable owned group.
            self.owned['motion'] = self.n.motion()
        self.frozen_phases = self.n.phases()
        publish(self.root/'frozen.json', {'motion': self.owned['motion'], 'phases': self.frozen_phases, 'phases_before_freeze': before_freeze,
                                        'frame': self.n.frame(), 'game_view': self.n.game_view(),
                                        'far': self.far, 'near': self.near,
                                        'phase_preservation': 'Freeze native API retains current phase; initial snapshot may precede freeze by worker startup frames.'})
        self.next_job(self.n.frame())

    def move(self, camera):
        # Track intent before call so failure after a successful native setter is recoverable.
        self.owned['camera'] = True
        self.expected_camera = copy.deepcopy(camera)
        self.n.set_camera(camera)

    def next_job(self, frame):
        self.job += 1
        if self.job >= len(self.jobs):
            return self.finish('COMPLETED')
        mode, direction = self.jobs[self.job]
        self.owned['cvars'][CVAR_MODE] = mode
        self.n.set_cvar(CVAR_MODE, mode)
        self.a, self.b = (self.far, self.near) if direction == 'forward' else (self.near, self.far)
        self.move(self.a)
        self.state, self.stage_start, self.schedule_index = 'warmup', frame, 0
        self.legs.append({'job': self.job, 'mode': mode, 'direction': direction, 'warmup_start_frame': frame,
                          'start_camera': copy.deepcopy(self.a), 'end_camera': copy.deepcopy(self.b), 'requested_frames': self.c['leg_frames']})

    def guard(self):
        if not self.n.context_ok():
            self.camera_intervened = True
            raise RuntimeError('CONTEXT_CHANGED: preserve camera; editor map/viewport/PIE/pilot changed')
        actual = self.n.camera()
        if not close(actual, self.expected_camera):
            self.camera_intervened = True
            raise RuntimeError('CAMERA_CHANGED_EXTERNALLY: preserve actual '+json.dumps(actual)+' expected '+json.dumps(self.expected_camera))
        if not close(self.n.authored(), self.original['authored'], 1e-5):
            raise RuntimeError('AUTHORING_CHANGED_EXTERNALLY: scene values/transforms are never restored')
        if not close(self.n.exposure(), self.original['exposure'], 1e-5):
            raise RuntimeError('EXPOSURE_CHANGED_EXTERNALLY')
        if 'motion' in self.owned:
            if not close(self.n.motion(), self.owned['motion'], 1e-5):
                raise RuntimeError('MOTION_CHANGED_EXTERNALLY: animation ownership lost')
            if not close(self.n.phases(), self.frozen_phases, 2e-5):
                raise RuntimeError('FROZEN_PHASE_DRIFT')
        else:
            # Live phases/reference advance legitimately while waiting for worker.
            if not close(self.n.motion()['animation'], self.original['motion']['animation'], 1e-5):
                raise RuntimeError('ANIMATION_CHANGED_BEFORE_FREEZE')
        if 'game_view' in self.owned and self.n.game_view() != self.owned['game_view']:
            raise RuntimeError('GAME_VIEW_CHANGED_EXTERNALLY')
        for key, val in self.owned['cvars'].items():
            if self.n.get_cvar(key) != val:
                raise RuntimeError('CVAR_CHANGED_EXTERNALLY '+key)
        for key, val in self.owned['preferences'].items():
            if self.n.preferences()[key] != val:
                raise RuntimeError('PREFERENCE_CHANGED_EXTERNALLY '+key)

    def request_capture(self, frame, elapsed, target):
        self.sequence += 1
        mode, direction = self.jobs[self.job]
        request = {'sequence': self.sequence, 'job': self.job, 'mode': mode, 'direction': direction,
                   'filename': str(self.root/('%02d-mode%d-%s-f%03d.png' % (self.sequence, mode, direction, elapsed))),
                   'requested_utc': utc(), 'request_frame': frame, 'leg_elapsed_frame': elapsed,
                   'scheduled_elapsed_frame': target, 'request_camera': copy.deepcopy(self.expected_camera),
                   'request_monotonic': time.monotonic(), 'capture_method': 'UE_MCP_Bridge.capture_screenshot', 'target': 'editor'}
        self.pending = request
        publish(self.root/'capture_requests'/('%06d.json' % self.sequence), request)

    def observe_capture(self, frame):
        if self.pending is None:
            return
        request = self.pending
        if time.monotonic()-request['request_monotonic'] > self.c['capture_timeout_seconds']:
            raise RuntimeError('CAPTURE_TIMEOUT sequence '+str(request['sequence']))
        path = self.root/'capture_acks'/('%06d.json' % request['sequence'])
        if not path.exists():
            return
        ack = json.loads(path.read_text(encoding='utf-8-sig'))
        rpc = ack.get('result', {})
        if ack.get('sequence') != request['sequence'] or 'error' in rpc or not rpc.get('result', {}).get('success'):
            raise RuntimeError('CAPTURE_RPC_FAILED '+json.dumps(ack))
        image = pathlib.Path(request['filename'])
        # The native success ack only means queued. PNG writing is asynchronous.
        if not image.is_file() or image.stat().st_size < 32:
            return
        try:
            data = image.read_bytes()
        except PermissionError:
            return  # Windows renderer may still own the file handle.
        if data[:8] != b'\x89PNG\r\n\x1a\n':
            raise RuntimeError('CAPTURE_NOT_PNG '+str(image))
        if data[-12:] != b'\x00\x00\x00\x00IEND\xaeB\x60\x82':
            return  # Partial PNG: continue camera travel, wait for complete IEND.
        interval_moving = (self.state == 'moving' and self.job == request['job']
                           and frame-self.stage_start < self.c['leg_frames'])
        record = {**request, 'observed_utc': utc(), 'observed_frame': frame,
                  'observed_camera': self.n.camera(), 'observed_state': self.state,
                  'frame_interval': [request['request_frame'], frame],
                  'interval_entirely_within_moving_leg': interval_moving,
                  'seconds_until_observed': time.monotonic()-request['request_monotonic'],
                  'bytes': len(data), 'png_size': [int.from_bytes(data[16:20], 'big'), int.from_bytes(data[20:24], 'big')],
                  'sha256': hashlib.sha256(data).hexdigest(), 'ack': ack,
                  'timing_limit': 'CPU request/observation interval, not exact rendered GPU frame or pose; renderer may lag.'}
        self.records.append(record)
        publish(self.root/'events'/('capture-%06d.json' % request['sequence']), record)
        self.pending = None

    def tick(self, delta):
        if self.done:
            return
        try:
            if time.monotonic()-self.started > self.c['timeout_seconds']:
                raise RuntimeError('PROBE_TIMEOUT')
            frame = self.n.frame()
            if frame == self.last_frame:
                return
            if frame < self.last_frame:
                raise RuntimeError('ENGINE_FRAME_COUNTER_REVERSED')
            self.last_frame = frame
            self.guard()
            # Observe before new camera command: records pose present during preceding rendering.
            self.observe_capture(frame)
            if self.state == 'worker':
                path = self.root/'capture_worker.json'
                if path.exists():
                    worker = json.loads(path.read_text(encoding='utf-8-sig'))
                    if not worker.get('ready') or worker.get('target') != 'editor':
                        raise RuntimeError('Unexpected capture worker contract')
                    self.configure()
                elif time.monotonic()-self.started > self.c['worker_timeout_seconds']:
                    raise RuntimeError('CAPTURE_WORKER_NOT_READY')
                return
            elapsed = frame-self.stage_start
            if self.state == 'warmup' and elapsed >= self.c['warmup_frames']:
                self.state, self.stage_start = 'moving', frame
                elapsed = 0
                self.legs[-1]['motion_start_frame'] = frame
            if self.state == 'moving':
                self.move(interpolate(self.a, self.b, elapsed/self.c['leg_frames']))
                self.poses.append({'frame': frame, 'job': self.job, 'elapsed': elapsed, 'camera': copy.deepcopy(self.expected_camera)})
                while self.schedule_index < len(self.schedule) and elapsed >= self.schedule[self.schedule_index]:
                    target = self.schedule[self.schedule_index]
                    self.schedule_index += 1
                    if self.pending is None and elapsed < self.c['leg_frames']:
                        self.request_capture(frame, elapsed, target)
                    else:
                        self.skipped.append({'job': self.job, 'target_frame': target, 'actual_frame': elapsed,
                                             'reason': 'capture_pending_or_leg_finished'})
                if elapsed >= self.c['leg_frames']:
                    self.legs[-1]['motion_stop_frame'] = frame
                    self.legs[-1]['observed_engine_duration'] = elapsed
                    self.state = 'drain'
                    publish(self.root/'events'/('leg-%02d.json' % self.job), self.legs[-1])
            if self.state == 'drain' and self.pending is None:
                self.next_job(frame)
        except BaseException:
            self.finish('ABORTED', traceback.format_exc())

    def finish(self, status, error=None):
        if self.done:
            return
        self.done = True
        if error:
            self.errors.append(error)
        results = {}

        def restore(key, current, expected, original, setter, valid=lambda: True):
            try:
                if not valid():
                    results[key] = {'status': 'preserved_external', 'reason': 'context or ownership changed'}
                elif not close(current(), expected, 1e-5 if key == 'motion' else .002):
                    results[key] = {'status': 'preserved_external', 'actual': current(), 'expected_owned': expected}
                else:
                    setter(original)
                    result = current()
                    results[key] = {'status': 'restored' if close(result, original, 1e-5 if key == 'motion' else .002) else 'failed',
                                    'actual': result}
            except BaseException:
                results[key] = {'status': 'failed', 'error': traceback.format_exc()}

        if self.handle:
            try:
                self.n.unregister(self.handle)
            except BaseException:
                self.errors.append('Callback unregister: '+traceback.format_exc())
        if self.owned.get('camera'):
            restore('camera', self.n.camera, self.expected_camera, self.original['camera'], self.n.set_camera,
                    lambda: not self.camera_intervened and self.n.context_ok())
        if 'motion' in self.owned:
            restore('motion', self.n.motion, self.owned['motion'], self.original['motion'], self.n.restore_motion, self.n.context_ok)
        if 'game_view' in self.owned:
            restore('game_view', self.n.game_view, self.owned['game_view'], self.original['game_view'], self.n.set_game_view, self.n.context_ok)
        for key, value in self.owned['cvars'].items():
            restore('cvar:'+key, lambda k=key: self.n.get_cvar(k), value, self.original['cvars'][key], lambda v, k=key: self.n.set_cvar(k, v))
        for key, value in self.owned['preferences'].items():
            restore('preference:'+key, lambda k=key: self.n.preferences()[k], value, self.original['preferences'][key], lambda v, k=key: self.n.set_preference(k, v))
        checks = {}
        for name, getter, expected in [('map_file_unchanged', self.n.map_sha, self.original['map_sha256']),
                                       ('authored_unchanged', self.n.authored, self.original['authored']),
                                       ('exposure_unchanged', self.n.exposure, self.original['exposure'])]:
            try:
                checks[name] = close(getter(), expected, 1e-5)
            except BaseException:
                checks[name] = False
                self.errors.append(name+': '+traceback.format_exc())
        restored = all(x['status'] == 'restored' for x in results.values())
        if any(x['status'] == 'failed' for x in results.values()):
            status = 'RESTORE_FAILED'
        elif status == 'COMPLETED' and (not restored or not all(checks.values())):
            status = 'ABORTED'
        valid_counts = [sum(r['job'] == j and r['interval_entirely_within_moving_leg'] for r in self.records) for j in range(4)]
        if status == 'COMPLETED' and min(valid_counts) < 2:
            status = 'INSUFFICIENT_CONTINUOUS_CAPTURES'
        receipt = {'schema': 1, 'status': status, 'finished_utc': utc(), 'elapsed_seconds': time.monotonic()-self.started,
                   'config': self.c, 'legs': self.legs, 'captures': self.records, 'valid_continuous_captures_per_leg': valid_counts,
                   'skipped_slots': self.skipped, 'pending_request': self.pending, 'errors': self.errors,
                   'restoration': {'all_owned_values_restored': restored, 'items': results, 'checks': checks,
                                   'camera_intervened': self.camera_intervened},
                   'visual_acceptance': 'NOT_EVALUATED', 'exact_gpu_frame_claim': False,
                   'timing': 'Engine-frame progression; each screenshot has CPU request/observation interval. Camera never waits for capture during a leg.',
                   'guard_scope': 'All actor transforms/editor hidden state, lights intensity/color, selected Box/exposure fields, owned controls. Individual custom show flags and every native component property are not enumerated.',
                   'restore_limit': 'Console values restored when still owned; console SetBy flags are not exposed by this Python API. No stale recovery after editor restart.',
                   'animation_restore': 'Original controls/reference restored if still owned. Originally live animation resumes its original timeline and may advance by probe duration. No map is saved.'}
        publish(self.root/'commanded-poses.json', self.poses)
        publish(self.root/'receipt.json', receipt)
        publish(self.root/'finished.json', {'status': status, 'restoration': receipt['restoration']})
        print('FOGMS_CONTINUOUS_FINISHED '+status+' '+str(self.root/'receipt.json'))


def launch(source):
    global CONTROLLER
    config = configuration(source)
    if config.get('action', 'run') == 'stop':
        if CONTROLLER is None or CONTROLLER.done:
            raise RuntimeError('No active continuous probe')
        CONTROLLER.finish('STOPPED', 'Operator requested stop; ownership-aware restoration')
        return {'status': 'STOPPED'}
    if CONTROLLER is not None and not CONTROLLER.done:
        raise RuntimeError('A continuous probe is already active')
    CONTROLLER = Probe(config, Native(config))
    return CONTROLLER.start()
