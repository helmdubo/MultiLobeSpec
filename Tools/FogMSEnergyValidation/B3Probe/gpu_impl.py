# On-demand only: explicit UE Python invocation; no startup registration.
"""Explicit B3 angular numeric probe; reuses one existing cube for its wall pair.

Freshly captures every StaticMeshActor pose/temporary visibility plus transport
controls and preferences. Restores on success, stop and errors. Never saves map.
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
MODULE = '_fogms_b3_gpu_probe'
LAYOUT = 'x,y+z*N; slabs: totalJ/residual, primaryJ/openFaces, sigma_s/sigma_t, diffuse input/output/absorption/directSource'
RECONSTRUCTION_LAYOUT = 'x,y+z*N; slabs: totalJ/residual, primaryJ/openFaces, sigma_s/sigma_t, maxReceiverRGB/valid'
RECONSTRUCTION_CVAR = 'r.FogMS.Transport.TestReconstruction'
CVARS = {
    'test': 'r.FogMS.Transport.Test', 'tau': 'r.FogMS.Transport.TestTau',
    'albedo': 'r.FogMS.Transport.TestAlbedo', 'boundary': 'r.FogMS.Transport.TestBoundary',
    'geometry': 'r.FogMS.Transport.TestGeometry'
}


def default_cases():
    def case(name, directions=48, test=1, tau=4, albedo=.9, boundary=0, geometry=0, shown=False, rgb=None):
        item = dict(name=name, directions=directions, test=test, tau=tau, albedo=albedo,
                    boundary=boundary, geometry=geometry, wall_position='center', wall_shown=shown)
        if rgb is not None:
            item['rgb_albedo'] = rgb
        return item
    return [case('furnace48-tau4', albedo=1),
            case('furnace96-tau16', directions=96, tau=16, albedo=1),
            case('uniform48-tau4'),
            case('vacuum48', tau=0, albedo=0),
            case('gap48-tau4', test=2, boundary=1),
            case('rgb48-tau4', albedo=-1, rgb=[.9,.65,.25,1.]),
            case('absorption96-tau4', directions=96, albedo=0, boundary=1),
            case('no-wall48-center', boundary=1, geometry=1),
            case('wall48-center', boundary=1, geometry=1, shown=True)]


def nearly_equal(a, b):
    if isinstance(a, dict) and isinstance(b, dict):
        return a.keys() == b.keys() and all(nearly_equal(a[k], b[k]) for k in a)
    if isinstance(a, list) and isinstance(b, list):
        return len(a) == len(b) and all(nearly_equal(x,y) for x,y in zip(a,b))
    if isinstance(a, float) or isinstance(b, float):
        return isinstance(a,(int,float)) and isinstance(b,(int,float)) and math.isclose(a,b,rel_tol=1.e-10,abs_tol=1.e-6)
    return a == b


def atomic_json(path, data):
    temp = path.with_suffix(path.suffix + '.tmp')
    temp.write_text(json.dumps(data, indent=2, ensure_ascii=False, allow_nan=False), encoding='utf-8')
    temp.replace(path)


def launch(config_name='gpu_config.json'):
    import unreal
    config = json.loads((ROOT / config_name).read_text(encoding='utf-8-sig'))
    output_root = pathlib.Path(config['output_root'])
    if not output_root.is_absolute():
        raise ValueError('output_root must be an absolute evidence directory')
    output_root = output_root.resolve()
    output_root.mkdir(parents=True, exist_ok=True)
    reconstruction_test = False
    if config.get('reconstruction_test'):
        raise ValueError('This B3 numeric suite requires the normal flux atlas; use a dedicated receiver probe for reconstruction')
    probe_cvars = dict(CVARS)
    reconstruction_value = unreal.SystemLibrary.get_console_variable_string_value(RECONSTRUCTION_CVAR)
    if reconstruction_value:
        if not math.isfinite(float(reconstruction_value)):
            raise RuntimeError('Invalid reconstruction diagnostic CVar')
        probe_cvars['reconstruction'] = RECONSTRUCTION_CVAR
    else:
        raise RuntimeError('B3 requires the reconstruction diagnostic CVar so it can be forced off and restored explicitly')
    expected_domain = 'transport_reconstruction' if reconstruction_test else 'transport'
    expected_layout = RECONSTRUCTION_LAYOUT if reconstruction_test else LAYOUT
    action = config.get('action', 'run')
    prior = sys.modules.get(MODULE)
    for other_name in ('_fogms_b2_numerical_probe', '_fogms_b2_wall_probe'):
        other = sys.modules.get(other_name)
        if other and getattr(other, 'controller', None) and not other.controller.restoration_ok:
            raise RuntimeError('Finish/restore the existing B2 probe before running B3.')
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
        result = {key: unreal.SystemLibrary.get_console_variable_string_value(name) for key, name in probe_cvars.items()}
        for item in result.values():
            if not item or not math.isfinite(float(item)):
                raise RuntimeError('Transport diagnostic CVars are missing or invalid; install/compile B3 first.')
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

    mesh_actors = {a.get_path_name():a for a in actor_api.get_all_level_actors()
                   if a.get_class().get_name() == 'StaticMeshActor'}
    candidates = [a for a in mesh_actors.values() if a.get_actor_label() == config.get('wall_actor_label','FogMS - Pillar 1')]
    if len(candidates) != 1:
        raise RuntimeError('Expected exactly one existing FogMS - Pillar 1 cube; no actor is created.')
    wall = candidates[0]
    wall_mesh = wall.get_component_by_class(unreal.StaticMeshComponent)
    asset = wall_mesh.get_editor_property('static_mesh')
    if asset is None or asset.get_path_name() != '/Engine/BasicShapes/Cube.Cube':
        raise RuntimeError('Wall actor must already use the native BasicShapes Cube.')
    if not wall_mesh.get_editor_property('visible') or not wall_mesh.get_editor_property('visible_in_ray_tracing'):
        raise RuntimeError('Existing wall component must already be visible and ray-trace-visible.')
    native_bounds = asset.get_bounding_box()
    cube_min, cube_max = val(native_bounds.min), val(native_bounds.max)
    if not nearly_equal(cube_min,[-50.]*3) or not nearly_equal(cube_max,[50.]*3):
        raise RuntimeError('Cube must have the known centered 100 cm local geometry.')
    if not nearly_equal(val(wall_mesh.get_world_location()),val(wall.get_actor_location())):
        raise RuntimeError('Wall cube component must be centered on the actor; offset components unsupported.')
    if any(x <= 0 or not math.isfinite(x) for x in geometry()['scale']):
        raise RuntimeError('Probe requires finite positive Box scale.')
    grid = int(config.get('grid',32))
    if grid != 32:
        raise RuntimeError('This probe targets the current fixed 32^3 Transport grid.')
    thickness = float(config.get('wall_thickness_cm',5.))
    half_extent = geometry()['half_extent_cm']
    spacing = [2*x/grid for x in half_extent]
    if thickness != 5. or thickness >= .25*spacing[0]:
        raise RuntimeError('Require the 5 cm wall to be thinner than one quarter of a cell.')
    padding = [max(5.,2*spacing[1]),max(5.,2*spacing[2])]
    basis = [val(component.get_forward_vector()),val(component.get_right_vector()),val(component.get_up_vector())]
    box_rotation = component.get_world_rotation()
    center = geometry()['center']
    live_transforms = {p:a.get_actor_transform() for p,a in mesh_actors.items()}

    def mesh_snapshot():
        return [{'path':p,'label':a.get_actor_label(),'hidden_editor':a.is_hidden_ed(),
                 'temporary_hidden':a.is_temporarily_hidden_in_editor(False),
                 'location':val(a.get_actor_location()),'rotation':val(a.get_actor_rotation()),
                 'scale':val(a.get_actor_scale3d())}
                for p,a in sorted(mesh_actors.items())]

    def place_wall(case):
        x = {'center':0., 'quarter_cell':.25*spacing[0],
             'boundary_half_cell':-half_extent[0]+.25*spacing[0]}[case['wall_position']]
        location = [center[i]+x*basis[0][i] for i in range(3)]
        scale = [thickness/100.,(2*half_extent[1]+2*padding[0])/100.,
                 (2*half_extent[2]+2*padding[1])/100.]
        # MakeTransform is not exported on UE 5.8 Python MathLibrary.
        wall.set_actor_scale3d(unreal.Vector(*scale))
        wall.set_actor_rotation(box_rotation,True)
        wall.set_actor_location(unreal.Vector(*location),False,True)
        for p,a in mesh_actors.items():
            a.set_is_temporarily_hidden_in_editor(a != wall or not case['wall_shown'])
        actual = next(s for s in mesh_snapshot() if s['path'] == wall.get_path_name())
        if not nearly_equal(actual['location'],location) or not nearly_equal(actual['rotation'],val(box_rotation)) or not nearly_equal(actual['scale'],scale):
            raise RuntimeError('Editor did not apply requested wall transform.')
        if actual['hidden_editor'] == bool(case['wall_shown']):
            raise RuntimeError('Wall is hidden by another editor visibility state; cannot run this probe.')
        if any(not a.is_hidden_ed() for a in mesh_actors.values() if a != wall):
            raise RuntimeError('Other StaticMeshActor geometry could not be hidden.')
        return {'actor_path':wall.get_path_name(),'asset':asset.get_path_name(),'grid':grid,
                'box_basis':basis,'local_center_x_cm':x,'thickness_cm':thickness,
                'cell_size_cm':spacing,'full_yz_cm':[100*scale[1],100*scale[2]],
                'location':location,'rotation':val(box_rotation),'scale':scale,
                'shown':case['wall_shown'],'static_meshes':mesh_snapshot()}

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
            box.set_editor_property('angular_quality', getattr(unreal.FogMSAngularQuality, original['quality_name']))
            box.set_editor_property('density_albedo', unreal.LinearColor(*original['density_albedo']))
        attempt('actor mode/iterations', actor_restore)
        for saved_mesh in original['static_meshes']:
            def restore_mesh(saved=saved_mesh):
                actor = mesh_actors.get(saved['path'])
                if actor is None:
                    raise RuntimeError('Original StaticMeshActor is unavailable: '+saved['path'])
                if action == 'restore':
                    actor.set_actor_scale3d(unreal.Vector(*saved['scale']))
                    actor.set_actor_rotation(unreal.Rotator(*saved['rotation']),True)
                    actor.set_actor_location(unreal.Vector(*saved['location']),False,True)
                else:
                    actor.set_actor_transform(live_transforms[saved['path']],False,True)
                actor.set_is_temporarily_hidden_in_editor(saved['temporary_hidden'])
            attempt('restore mesh '+saved_mesh['path'],restore_mesh)
        for key, old in original['cvars'].items():
            attempt(key, lambda k=key, v=old: command(probe_cvars[k], v))
        attempt('throttle', lambda: performance.set_editor_property('bThrottleCPUWhenNotForeground', original['throttle']))
        attempt('autosave', lambda: autosave.set_editor_property('bAutoSaveEnable', original['autosave']))
        def verify():
            checks['mode'] = str(box.get_editor_property('scattering_mode')) == original['mode_repr']
            checks['iterations'] = int(box.get_editor_property('transport_iterations')) == original['iterations']
            checks['quality'] = str(box.get_editor_property('angular_quality')) == original['quality_repr']
            checks['albedo'] = nearly_equal(val(box.get_editor_property('density_albedo')),original['density_albedo'])
            checks['cvars'] = cvars() == original['cvars']
            checks['throttle'] = bool(performance.get_editor_property('bThrottleCPUWhenNotForeground')) == original['throttle']
            checks['autosave'] = bool(autosave.get_editor_property('bAutoSaveEnable')) == original['autosave']
            checks['map_unchanged'] = hashlib.sha256(map_path.read_bytes()).hexdigest() == original['map_sha256']
            checks['authored_scene_unchanged'] = nearly_equal(scene_guard(),original['scene_guard'])
            checks['static_meshes_restored'] = nearly_equal(mesh_snapshot(),original['static_meshes'])
        attempt('verify restoration', verify)
        return {'ok': not errors and len(checks) == 10 and all(checks.values()), 'checks': checks, 'errors': errors}

    if action == 'restore':
        name = config.get('restore_receipt', '')
        if pathlib.Path(name).name != name or not name.startswith('gpu_b3-') or not name.endswith('.json'):
            raise ValueError('restore_receipt must name a local gpu_b3-*.json produced by this harness.')
        path = output_root / name
        saved = json.loads(path.read_text(encoding='utf-8-sig'))
        saved['restoration'] = restore(saved['original'])
        saved['restoration_ok'] = saved['restoration']['ok']
        saved['status'] = 'RESTORED' if saved['restoration_ok'] else 'RESTORATION_INCOMPLETE'
        atomic_json(path, saved)
        unreal.log('FOGMS_B3_GPU_RESTORED ' + str(path))
        return

    case_id = config.get('id', 'wall-' + datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%S'))
    if not re.fullmatch(r'[A-Za-z0-9_-]+', case_id):
        raise ValueError('Unsafe/invalid probe id')
    cases = default_cases()  # Fixed bounded matrix; no external arbitrary scene edits.
    selected = config.get('case_names')
    if selected:
        if len(set(selected)) != len(selected) or any(x not in {c['name'] for c in cases} for x in selected):
            raise ValueError('case_names must be a unique subset of the fixed suite')
        cases = [c for c in cases if c['name'] in selected]
    if 'reconstruction' in probe_cvars:
        for c in cases:
            c['reconstruction'] = int(reconstruction_test)
    if len({c['name'] for c in cases}) != len(cases):
        raise ValueError('Case names must be unique')
    for c in cases:
        if not re.fullmatch(r'[A-Za-z0-9_-]+', c['name']) or int(c['test']) not in (1, 2):
            raise ValueError('Only numerical Test1/Test2 cases are supported')
        if c['boundary'] not in (0, 1) or c['geometry'] not in (0, 1):
            raise ValueError('Invalid boundary/geometry')
        if not 0 <= float(c['tau']) <= 100 or not -1 <= float(c['albedo']) <= 1:
            raise ValueError('Invalid tau/albedo')
    iterations = int(config.get('iterations', 24))
    if not 4 <= iterations <= 64:
        raise ValueError('Iterations must be 4..64')
    warmup = max(4, int(config.get('warmup_frames', 24)))
    wait_frames = max(4, int(config.get('wait_frames', 4)))
    timeout = max(5., float(config.get('dump_timeout_seconds', 45)))
    duration = max(timeout, float(config.get('max_duration_seconds', 600)))
    receipt = output_root / ('gpu_b3-' + case_id + '.json')
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
            unreal.log('FOGMS_B3_BASELINE_OPTIONAL_MISSING ' + str(baseline))
    transport = getattr(unreal.FogMSScatteringMode, 'ANGULAR_TRANSPORT')
    qualities = {48:getattr(unreal.FogMSAngularQuality,'BALANCED48'),96:getattr(unreal.FogMSAngularQuality,'HIGH96')}
    original_quality = box.get_editor_property('angular_quality')
    quality_match = re.search(r'\.([A-Z][A-Z_0-9]*):', str(original_quality))
    if quality_match is None:
        raise RuntimeError('Cannot serialize angular quality for recovery')
    original_mode = box.get_editor_property('scattering_mode')
    match = re.search(r'\.([A-Z][A-Z_0-9]*):', str(original_mode))
    if match is None:
        raise RuntimeError('Cannot serialize the exact current enum for recovery.')
    original = {
        'world': world.get_path_name(), 'actor_path': box.get_path_name(),
        'mode_name': match.group(1), 'mode_repr': str(original_mode),
        'iterations': int(box.get_editor_property('transport_iterations')),
        'quality_name':quality_match.group(1), 'quality_repr':str(original_quality),
        'density_albedo':val(box.get_editor_property('density_albedo')),
        'cvars': cvars(), 'throttle': bool(performance.get_editor_property('bThrottleCPUWhenNotForeground')),
        'autosave': bool(autosave.get_editor_property('bAutoSaveEnable')),
        'map_sha256': hashlib.sha256(map_path.read_bytes()).hexdigest(), 'scene_guard': scene_guard(),
        'static_meshes':mesh_snapshot()
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
                'schema': 'fogms_b3_gpu_probe_v1', 'id': case_id, 'status': status,
                'output_root':str(output_root), 'complete_suite':cases == [dict(c, reconstruction=0) for c in default_cases()],
                'timestamp_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
                'engine_frames_elapsed': int(unreal.SystemLibrary.get_frame_count()) - self.engine_start,
                'slate_callbacks': self.frames, 'elapsed_seconds': time.monotonic() - self.started,
                'baseline_reference': str(baseline) if baseline else None, 'baseline_sha256': baseline_sha256,
                'baseline_status': baseline_status,
                'original': original, 'iterations': iterations, 'planned_cases': cases, 'cases': self.records,
                'reconstruction_test': reconstruction_test,
                'errors': self.errors, 'restoration': self.restoration, 'restoration_ok': self.restoration_ok,
                'note': 'Nine bounded angular cases, including one existing-cube wall pair; readback stalls are not GPU timing.'
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
            unreal.log('FOGMS_B3_GPU_ACCEPTED ' + str(receipt) + ' (asynchronous; wait for COMPLETED)')

        def guard(self):
            if editor.get_editor_world().get_path_name() != original['world'] or levels.is_in_play_in_editor():
                raise RuntimeError('Editor world/PIE changed during probe')
            if not nearly_equal(scene_guard(),self.expected_scene):
                raise RuntimeError('Scene changed externally during the wall probe')
            if box.get_editor_property('scattering_mode') != transport or int(box.get_editor_property('transport_iterations')) != iterations:
                raise RuntimeError('Transport mode/iteration controls changed externally')
            if box.get_editor_property('angular_quality') != qualities[cases[self.index]['directions']]:
                raise RuntimeError('Angular quality changed externally')
            values = cvars()
            for key, name in probe_cvars.items():
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
            box.set_editor_property('angular_quality', qualities[c['directions']])
            box.set_editor_property('density_albedo', unreal.LinearColor(*c.get('rgb_albedo',original['density_albedo'])))
            wall_geometry = place_wall(c) if c['geometry'] else None
            self.expected_scene = scene_guard()
            for key, name in probe_cvars.items():
                command(name, c[key])
            self.records.append({'index': self.index, 'case': c.copy(), 'box': geometry(), 'wall':wall_geometry,
                                 'applied_engine_frame': int(unreal.SystemLibrary.get_frame_count())})
            self.guard()
            self.phase, self.phase_frame, self.phase_slate = 'wait_barrier', int(unreal.SystemLibrary.get_frame_count()), self.frames
            self.write('RUNNING')

        def issue(self, role):
            values = self.guard()
            prefix = output_root / ('gpu_' + case_id + '-%02d-' % self.index + cases[self.index]['name'] + '-' + role)
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
            if meta.get('success') is not True or meta.get('domain') != expected_domain or meta.get('format') != 'RGBA32F_LE':
                raise RuntimeError('Missing current Transport dump: ' + str(meta))
            if reconstruction_test and (meta.get('reconstructionTest') is not True or meta.get('fluxDiagnosticsValid') is not False):
                raise RuntimeError('Reconstruction dump flags disagree with the configured diagnostic')
            n = int(meta['grid'])
            if n != grid:
                raise RuntimeError('Transport resolution changed; wall placement would use the wrong cell size.')
            payload = pathlib.Path(self.pending['prefix'] + '.rgba32f')
            if meta.get('layout') != expected_layout or meta.get('width') != n or meta.get('height') != 4*n*n or meta.get('bytes') != 4*n**3*16:
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
            if int(meta['iterations']) != iterations or int(meta['directions']) != c['directions'] or meta.get('transport_scheme') != 'upwind_half_gauss':
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
                unreal.log('FOGMS_B3_GPU_FINISHED ' + status + ' ' + str(receipt))

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
