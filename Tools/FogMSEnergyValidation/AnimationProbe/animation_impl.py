"""On-demand UE animation probe. Fresh durable restoration state precedes mutation.

Only changes Box mode/animation/transform, two diagnostic CVars and editor prefs.
No save, new actors, camera changes, engine edits or automatic startup registration.
"""
from __future__ import annotations
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
MODULE = '_fogms_animation_probe_controller'
WORLD = '/Game/FogMS_Test/FogMS_Box.FogMS_Box'
LAYOUT = 'x,y+z*N; slabs: totalJ/residual, primaryJ/openFaces, sigma_s/sigma_t, diffuse input/output/absorption/directSource'
ANIMATION = ('animate_density', 'density_wind_velocity', 'density_detail_velocity', 'density_evolution_velocity',
             'use_manual_animation_time', 'manual_animation_time', 'animation_time_offset')
VECTORS = ('density_wind_velocity', 'density_detail_velocity', 'density_evolution_velocity')
PROTECTED = ('enabled', 'density_enabled', 'density_texture', 'density_channel', 'world_aligned_texture',
             'world_texture_size', 'tile_scale', 'threshold', 'softness', 'density', 'density_albedo',
             'density_edge_feather', 'detail_strength', 'detail_scale', 'detail_second_octave',
             'spatial_strength', 'spatial_distance', 'indirect_shadow_strength', 'transport_iterations',
             'angular_quality', 'authored_sun_shadow', 'cast_sun_shadow', 'surface_shadow_strength',
             'surface_shadow_steps', 'filtered_sun_shadow', 'shadow_filter_sigma', 'filter_sun_inside_volume')
CVARS = ('r.FogMS.Transport.Test', 'r.FogMS.Transport.TestReconstruction')
CASES = ('static', 'manual-zero', 'manual-shift', 'translated-tracking', 'resized-world-lock',
         'detail-evolution', 'live-a', 'live-b', 'freeze', 'frozen-later', 'resume')


def atomic_json(path, data):
    temporary = path.with_suffix(path.suffix + '.tmp')
    temporary.write_text(json.dumps(data, indent=2, ensure_ascii=False, allow_nan=False), encoding='utf-8')
    temporary.replace(path)


def attempt_all(actions, checks):
    """Shared cleanup path: one failing restoration must not skip later restores."""
    errors, results = [], {}
    for name, action in actions:
        try:
            action()
        except BaseException:
            errors.append(name + ': ' + traceback.format_exc())
    for name, check in checks:
        try:
            results[name] = bool(check())
        except BaseException:
            results[name] = False
            errors.append(name + ': ' + traceback.format_exc())
    return {'ok': not errors and all(results.values()) and bool(results), 'checks': results, 'errors': errors}


def enum_name(value):
    match = re.search(r'\.([A-Z][A-Z_0-9]*):', str(value))
    if match is None:
        raise ValueError('Cannot serialize enum: ' + str(value))
    return match.group(1)


def launch():
    import unreal
    config = json.loads((ROOT / 'animation_config.json').read_text(encoding='utf-8-sig'))
    action = config.get('action', 'run')
    prior = sys.modules.get(MODULE)
    if action == 'stop':
        if not prior or not getattr(prior, 'controller', None):
            raise RuntimeError('No active animation probe; use action=restore with its id after a process restart.')
        prior.controller.finish('STOPPED', 'operator stop')
        return
    if action not in ('run', 'restore'):
        raise ValueError('action must be run, stop or restore')
    if prior and getattr(prior, 'controller', None) and not prior.controller.restoration_ok:
        raise RuntimeError('Previous animation probe needs stop/restoration before another run.')
    case_id = str(config['id'])
    if not re.fullmatch(r'[A-Za-z0-9_-]+', case_id):
        raise ValueError('Unsafe probe id')
    evidence = pathlib.Path(config['evidence_root'])
    if not evidence.is_absolute():
        raise ValueError('evidence_root must be an explicit absolute directory')
    run_root, receipt = evidence / case_id, evidence / case_id / 'receipt.json'
    if action == 'run' and run_root.exists():
        raise RuntimeError('Immutable id already exists; choose a new id.')

    editor = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem)
    levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    actor_api = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    world = editor.get_editor_world()
    if world is None or world.get_path_name() != WORLD or levels.is_in_play_in_editor():
        raise RuntimeError('Use the existing FogMS_Box editor map, outside PIE.')
    boxes = [a for a in actor_api.get_all_level_actors() if a.get_class().get_name() == 'FogMSBoxVolume']
    if len(boxes) != 1 or boxes[0].get_actor_label() != 'FogMS - Live Box':
        raise RuntimeError('Expected exactly the authored FogMS - Live Box actor.')
    box = boxes[0]
    component = box.get_editor_property('box_component')
    performance = unreal.get_default_object(unreal.load_class(None, '/Script/UnrealEd.EditorPerformanceSettings'))
    autosave = unreal.get_default_object(unreal.load_class(None, '/Script/UnrealEd.EditorLoadingSavingSettings'))
    map_path = pathlib.Path(unreal.Paths.project_dir()) / 'Content/FogMS_Test/FogMS_Box.umap'
    viewport = levels.get_active_viewport_config_key()

    def val(value):
        if value is None or isinstance(value, (bool, int, float, str)):
            return value
        if isinstance(value, (list, tuple)):
            return [val(x) for x in value]
        if isinstance(value, unreal.Object):
            return value.get_path_name()
        for names in (('pitch', 'yaw', 'roll'), ('r', 'g', 'b', 'a'), ('x', 'y', 'z')):
            if all(hasattr(value, n) for n in names):
                return [float(getattr(value, n)) for n in names]
        return str(value)

    def sha():
        return hashlib.sha256(map_path.read_bytes()).hexdigest()

    def cvars():
        result = {name: unreal.SystemLibrary.get_console_variable_string_value(name) for name in CVARS}
        if any(not x or not math.isfinite(float(x)) for x in result.values()):
            raise RuntimeError('Missing transport diagnostic CVars; install current package first.')
        return result

    def command(name, value):
        if name not in CVARS or not math.isfinite(float(value)):
            raise ValueError('Invalid controlled CVar')
        unreal.SystemLibrary.execute_console_command(world, name + ' ' + str(value))

    def transforms():
        return {'location': val(box.get_actor_location()), 'rotation': val(box.get_actor_rotation()),
                'scale': val(box.get_actor_scale3d()), 'center': val(component.get_world_location()),
                'extent': val(component.get_scaled_box_extent())}

    def authored():
        actors = []
        for actor in actor_api.get_all_level_actors():
            row = {'path': actor.get_path_name(), 'hidden': actor.is_hidden_ed()}
            if actor != box:
                row.update(location=val(actor.get_actor_location()), rotation=val(actor.get_actor_rotation()), scale=val(actor.get_actor_scale3d()))
            actors.append(row)
        return {'world': world.get_path_name(), 'actors': sorted(actors, key=lambda a: a['path']),
                'camera': val(levels.get_level_viewport_camera_info(viewport)),
                'fov': val(levels.get_level_viewport_fov(viewport)),
                'viewport_key': str(levels.get_active_viewport_config_key()),
                'properties': {p: val(box.get_editor_property(p)) for p in PROTECTED}}

    def controls():
        return {'scattering_mode': enum_name(box.get_editor_property('scattering_mode')),
                **{p: val(box.get_editor_property(p)) for p in ANIMATION}}

    def apply_controls(values):
        for name, value in values.items():
            if name == 'scattering_mode':
                value = getattr(unreal.FogMSScatteringMode, value)
            elif name in VECTORS:
                value = unreal.Vector(*value)
            box.set_editor_property(name, value)
        box.update_density()

    def set_transform(transform):
        box.set_actor_scale3d(unreal.Vector(*transform['scale']))
        box.set_actor_rotation(unreal.Rotator(*transform['rotation']), False)
        box.set_actor_location(unreal.Vector(*transform['location']), False, False)
        box.update_density()

    def sample():
        box.update_density()
        mid = box.get_editor_property('density_component').get_material(0)
        if mid is None or mid.get_class().get_name() != 'MaterialInstanceDynamic':
            raise RuntimeError('Expected native density MID.')
        # These getters are explicit ScriptName exports in UE MaterialInstanceDynamic.h.
        return {'engine_frame': int(unreal.SystemLibrary.get_frame_count()),
                'world_time': float(unreal.GameplayStatics.get_time_seconds(world)),
                'sample_time': float(box.get_editor_property('density_animation_time')),
                'status': str(box.get_editor_property('density_animation_status')),
                'controls': controls(), 'transform': transforms(),
                'mid': {p: val(mid.get_vector_parameter_value('FogMS_' + p))
                        for p in ('WorldFrequencies', 'WorldPhase0', 'WorldPhase1', 'WorldPhase2')}}

    def restore(original):
        def require_original():
            active = editor.get_editor_world()
            if active is None or active.get_path_name() != original['world'] or box.get_path_name() != original['actor_path']:
                raise RuntimeError('Restore requires the original editor world/actor.')
        def restore_prop(name, value):
            require_original()
            apply_controls({name: value})
        actions = [('disable temporary animation', lambda: restore_prop('animate_density', False))]
        # Every property is attempted separately so one failed setter cannot skip the rest.
        for name, value in original['controls'].items():
            if name != 'animate_density':
                actions.append((name, lambda n=name, v=value: restore_prop(n, v)))
        actions += [('transform', lambda: (require_original(), set_transform(original['transform']))),
                    ('original animation enabled', lambda: restore_prop('animate_density', original['controls']['animate_density']))]
        for name, value in original['cvars'].items():
            actions.append((name, lambda n=name, v=value: command(n, v)))
        actions += [('throttle', lambda: performance.set_editor_property('bThrottleCPUWhenNotForeground', original['throttle'])),
                    ('autosave', lambda: autosave.set_editor_property('bAutoSaveEnable', original['autosave']))]
        checks = [('controls', lambda: controls() == original['controls']),
                  ('transform', lambda: transforms() == original['transform']),
                  ('authored_scene', lambda: authored() == original['authored']),
                  ('map_file_unchanged', lambda: sha() == original['map_sha256']),
                  ('cvars', lambda: cvars() == original['cvars']),
                  ('throttle', lambda: bool(performance.get_editor_property('bThrottleCPUWhenNotForeground')) == original['throttle']),
                  ('autosave', lambda: bool(autosave.get_editor_property('bAutoSaveEnable')) == original['autosave'])]
        return attempt_all(actions, checks)

    if action == 'restore':
        saved = json.loads(receipt.read_text(encoding='utf-8-sig'))
        if saved.get('schema') != 'fogms_animation_probe_v1' or saved.get('id') != case_id:
            raise RuntimeError('Receipt schema/id mismatch')
        saved['restoration'] = restore(saved['original'])
        saved['restoration_ok'] = saved['restoration']['ok']
        saved['status'] = 'RESTORED' if saved['restoration_ok'] else 'RESTORATION_INCOMPLETE'
        atomic_json(receipt, saved)
        unreal.log('FOGMS_ANIMATION_RESTORED ' + str(receipt))
        return

    # CallInEditor alone does not export methods to Python. Validate BlueprintCallable
    # access before creating evidence or mutating the actor; recovery does not need it.
    for method in ('update_density', 'freeze_density_animation', 'resume_density_animation'):
        if not callable(getattr(box, method, None)):
            raise RuntimeError('Missing public Python actor method: ' + method
                               + '; install a package with BlueprintCallable animation methods.')

    mode = str(config.get('scattering_mode', 'TRANSPORT'))
    if mode not in ('TRANSPORT', 'ANGULAR_TRANSPORT') or not hasattr(unreal.FogMSScatteringMode, mode):
        raise ValueError('scattering_mode must be installed TRANSPORT or ANGULAR_TRANSPORT')
    wind = [float(x) for x in config.get('wind_cm_s', [80, 0, 0])]
    manual_time = float(config.get('manual_time_seconds', 20))
    if len(wind) != 3 or not all(math.isfinite(x) for x in wind) or not any(wind) or not math.isfinite(manual_time) or manual_time <= 0:
        raise ValueError('Need finite nonzero wind and positive manual time')
    for prop in ('enabled', 'density_enabled', 'world_aligned_texture'):
        if not box.get_editor_property(prop):
            raise RuntimeError('Existing scene must already have ' + prop + '; probe will not override authored density.')
    if float(box.get_editor_property('density')) <= 0 or box.is_hidden_ed():
        raise RuntimeError('Need a visible, nonzero authored density source.')
    original = {'world': world.get_path_name(), 'actor_path': box.get_path_name(), 'controls': controls(),
                'transform': transforms(), 'cvars': cvars(), 'authored': authored(), 'map_sha256': sha(),
                'throttle': bool(performance.get_editor_property('bThrottleCPUWhenNotForeground')),
                'autosave': bool(autosave.get_editor_property('bAutoSaveEnable'))}
    run_root.mkdir(parents=True, exist_ok=False)
    atomic_json(run_root / 'original.json', original)
    warmup = max(4, int(config.get('warmup_frames', 8)))
    wait_frames = max(4, int(config.get('wait_frames', 4)))
    live_wait = max(10, int(config.get('live_wait_frames', 30)))
    timeout = max(10., float(config.get('dump_timeout_seconds', 45)))
    duration = max(timeout, float(config.get('max_duration_seconds', 600)))

    class Probe:
        def __init__(self):
            self.handle, self.pending = None, None
            self.finished = self.restoration_ok = self.in_tick = False
            self.started, self.frames, self.index = time.monotonic(), 0, -1
            self.records, self.errors, self.restoration = [], [], None
            self.expected_controls = None
            self.expected_transform = None

        def write(self, status):
            atomic_json(receipt, {'schema': 'fogms_animation_probe_v1', 'id': case_id, 'status': status,
                        'timestamp_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
                        'config': config, 'original': original, 'planned_cases': CASES, 'cases': self.records,
                        'elapsed_seconds': time.monotonic() - self.started, 'slate_callbacks': self.frames,
                        'errors': self.errors, 'restoration': self.restoration, 'restoration_ok': self.restoration_ok,
                        'limits': ['No rendered trail/flicker acceptance; numerical density/history contract only.',
                                   'Live MID and producer are different asynchronous snapshots; exact parity is asserted only for frozen cases.',
                                   'Readback stalls invalidate GPU performance measurements.']})

        def start(self):
            self.write('PREPARED')
            autosave.set_editor_property('bAutoSaveEnable', False)
            performance.set_editor_property('bThrottleCPUWhenNotForeground', False)
            for name in CVARS:
                command(name, 0)
            apply_controls({'scattering_mode': mode, 'animate_density': False,
                            'density_wind_velocity': wind, 'density_detail_velocity': [0, 0, 0],
                            'density_evolution_velocity': [0, 0, 0], 'use_manual_animation_time': True,
                            'manual_animation_time': 0., 'animation_time_offset': 0.})
            self.begin_case()
            self.handle = unreal.register_slate_post_tick_callback(self.tick)
            self.write('RUNNING')
            unreal.log('FOGMS_ANIMATION_ACCEPTED ' + str(receipt) + ' (async; wait for completion then analyze)')

        def guard(self):
            active = editor.get_editor_world()
            if active is None or active.get_path_name() != original['world'] or levels.is_in_play_in_editor():
                raise RuntimeError('World/PIE changed')
            if authored() != original['authored'] or controls() != self.expected_controls or transforms() != self.expected_transform:
                raise RuntimeError('Camera or authored scene/Box controls changed externally during the probe')
            if any(float(x) != 0 for x in cvars().values()):
                raise RuntimeError('Transport diagnostic fixture became active')

        def begin_case(self):
            self.index += 1
            if self.index == len(CASES):
                self.finish('COMPLETED', 'all cases captured')
                return
            name = CASES[self.index]
            before = sample()
            if name == 'manual-zero':
                apply_controls({'animate_density': True})
            elif name == 'manual-shift':
                apply_controls({'manual_animation_time': manual_time})
            elif name == 'translated-tracking':
                moved = dict(original['transform'])
                moved['location'] = [x + v * manual_time for x, v in zip(moved['location'], wind)]
                set_transform(moved)
            elif name == 'resized-world-lock':
                resized = dict(original['transform'])
                resized['scale'] = [x * s for x, s in zip(resized['scale'], [1.25, .8, 1.1])]
                set_transform(resized)
            elif name == 'detail-evolution':
                set_transform(original['transform'])
                apply_controls({'density_detail_velocity': [17., -9., 3.], 'density_evolution_velocity': [2., 5., -7.]})
            elif name in ('live-a', 'resume'):
                box.resume_density_animation()
            elif name == 'freeze':
                box.freeze_density_animation()
            after = sample()
            self.records.append({'index': self.index, 'name': name, 'before': before, 'after': after})
            self.expected_controls, self.expected_transform = controls(), transforms()
            self.guard()
            self.phase, self.phase_frame, self.phase_slate = 'wait_barrier', int(unreal.SystemLibrary.get_frame_count()), self.frames
            self.write('RUNNING')

        def issue(self, role):
            self.guard()
            prefix = run_root / ('%02d-%s-%s' % (self.index, CASES[self.index], role))
            if any(pathlib.Path(str(prefix) + suffix).exists() for suffix in ('.json', '.rgba32f')):
                raise RuntimeError('Refusing an existing dump prefix')
            self.pending = {'role': role, 'prefix': str(prefix), 'before_request': sample()}
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
            if meta.get('success') is not True or meta.get('domain') != 'transport' or meta.get('format') != 'RGBA32F_LE' or meta.get('layout') != LAYOUT:
                raise RuntimeError('Missing current Transport atlas; inspect dump ' + str(path))
            n = int(meta['grid'])
            payload = pathlib.Path(self.pending['prefix'] + '.rgba32f')
            if meta.get('width') != n or meta.get('height') != 4*n*n or meta.get('bytes') != 64*n**3 or not payload.is_file() or payload.stat().st_size != 64*n**3:
                raise RuntimeError('Incomplete four-slab readback')
            for key in ('revision', 'densityPhase0', 'densityPhase1', 'densityPhase2', 'animationActive', 'historyReset'):
                if key not in meta:
                    raise RuntimeError('Required animation metadata missing: ' + key + '; install current package.')
            if int(meta.get('test', -1)) != 0 or meta.get('reconstructionTest', False):
                raise RuntimeError('A diagnostic fixture replaced authored density')
            stamp, source = int(meta['dumpRenderFrame']), int(meta['sourceProducedRenderFrame'])
            if not 0 <= ((stamp-source) & 0xffffffff) <= 2:
                raise RuntimeError('Stale producer atlas')
            result = dict(self.pending, metadata=meta, after_receive=sample(), rgba_sha256=hashlib.sha256(payload.read_bytes()).hexdigest())
            self.records[-1][result['role']] = result
            self.pending = None
            if result['role'] == 'barrier':
                self.phase, self.phase_frame, self.phase_slate = 'wait_measurement', int(unreal.SystemLibrary.get_frame_count()), self.frames
            else:
                barrier = self.records[-1]['barrier']['metadata']
                for key in ('dumpRenderFrame', 'sourceProducedRenderFrame'):
                    if not 2 <= ((int(meta[key]) - int(barrier[key])) & 0xffffffff) < 2**31:
                        raise RuntimeError('Need two fresh render/producer frames after barrier')
                self.begin_case()
            if not self.finished:
                self.write('RUNNING')

        def tick(self, dt):
            if self.finished or self.in_tick:
                return
            self.in_tick = True
            try:
                self.frames += 1
                if time.monotonic() - self.started > duration:
                    raise TimeoutError('Animation probe duration exceeded')
                self.guard()
                levels.editor_invalidate_viewports()
                if self.phase == 'readback':
                    self.receive()
                    if self.phase == 'readback' and time.monotonic() > self.deadline:
                        raise TimeoutError('Readback completion missing')
                elif self.phase in ('wait_barrier', 'wait_measurement'):
                    frames = warmup if self.phase == 'wait_barrier' else wait_frames
                    if CASES[self.index] in ('live-b', 'frozen-later') and self.phase == 'wait_barrier':
                        frames = live_wait
                    if int(unreal.SystemLibrary.get_frame_count()) - self.phase_frame >= frames and self.frames - self.phase_slate >= 2:
                        self.issue('barrier' if self.phase == 'wait_barrier' else 'measurement')
            except BaseException:
                self.errors.append(traceback.format_exc())
                self.finish('ERROR', 'callback failed')
            finally:
                self.in_tick = False

        def finish(self, status, reason):
            if self.finished and self.restoration_ok:
                return
            self.finished, self.phase = True, 'finished'
            try:
                if self.handle is not None:
                    unreal.unregister_slate_post_tick_callback(self.handle)
                    self.handle = None
            except BaseException:
                self.errors.append(traceback.format_exc())
            finally:
                self.restoration = restore(original)
                self.restoration_ok = self.restoration['ok'] and self.handle is None
                self.restoration['reason'] = reason
                self.write(status if self.restoration_ok else 'RESTORATION_INCOMPLETE')
                unreal.log('FOGMS_ANIMATION_FINISHED ' + str(receipt))

    module = types.ModuleType(MODULE)
    module.controller = Probe()
    sys.modules[MODULE] = module
    try:
        module.controller.start()
    except BaseException:
        module.controller.errors.append(traceback.format_exc())
        module.controller.finish('ERROR', 'startup failed')
        raise


if __name__ == '__main__':
    launch()
