"""Opt-in native viewport check for relative detail flow; never saves the map.

Run the existing MotionProbe/capture_worker.mjs with edge-flow-visual-config.json
from the evidence directory first. All authoring, time reference and preferences
are restored from plain serialized values, including on capture failure.
"""
def run():
    import unreal, json, pathlib, time, traceback, types, sys, hashlib
    root = pathlib.Path('E:/GITHUB/MultiLobeSpec/.codex-build/FogMS_Motion_20260921')
    config = json.loads((root/'edge-flow-visual-config.json').read_text())
    out = pathlib.Path(config['evidence_root'])/config['id']
    out.mkdir(exist_ok=False)
    for d in ('capture_requests', 'capture_acks'): (out/d).mkdir()
    levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    api = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    assert world.get_path_name() == '/Game/FogMS_Test/FogMS_Box.FogMS_Box'
    assert not levels.is_in_play_in_editor()
    box = next(a for a in api.get_all_level_actors() if a.get_class().get_name() == 'FogMSBoxVolume')
    viewport = levels.get_active_viewport_config_key()
    assert not levels.get_pilot_level_actor(viewport)
    perf = unreal.get_default_object(unreal.load_class(None, '/Script/UnrealEd.EditorPerformanceSettings'))
    auto = unreal.get_default_object(unreal.load_class(None, '/Script/UnrealEd.EditorLoadingSavingSettings'))
    names = ('animate_density', 'use_manual_animation_time', 'manual_animation_time', 'animation_time_offset',
             'wind_speed', 'edge_flow_speed', 'density_detail_velocity', 'density_evolution_velocity')
    ref_fields = ('initialized', 'time', 'displacement0', 'displacement1', 'displacement2', 'velocity0', 'velocity1', 'velocity2')
    def encode(v):
        if v is None or isinstance(v, (bool, str, float, int)): return v
        if isinstance(v, (list, tuple)): return [encode(x) for x in v]
        if isinstance(v, unreal.Object): return v.get_path_name()
        for fields in (('pitch','yaw','roll'), ('r','g','b','a'), ('x','y','z')):
            if all(hasattr(v, k) for k in fields): return [float(getattr(v, k)) for k in fields]
        return str(v)
    def reference():
        r = box.get_editor_property('density_motion_reference')
        return {k: encode(r.get_editor_property(k)) for k in ref_fields}
    def restore_reference(values):
        r = unreal.FogMSDensityMotionReference()
        for k, v in values.items(): r.set_editor_property(k, unreal.Vector(*v) if isinstance(v,list) else v)
        assert box.restore_density_motion_reference(r)
    def camera():
        return [encode(levels.get_level_viewport_camera_info(viewport)), encode(levels.get_level_viewport_fov(viewport))]
    protected = ('density', 'density_albedo', 'density_texture', 'density_channel', 'threshold', 'softness',
                 'detail_strength', 'detail_scale', 'detail_second_octave', 'world_texture_size', 'texture_offset_world',
                 'scattering_mode', 'angular_quality', 'transport_iterations', 'density_motion_mode')
    def authored():
        rows = []
        for a in api.get_all_level_actors():
            rows.append([a.get_path_name(), encode(a.get_actor_location()), encode(a.get_actor_rotation()),
                         encode(a.get_actor_scale3d()), a.is_hidden_ed(),
                         [[c.get_path_name(), encode(c.get_editor_property('intensity')), encode(c.get_editor_property('light_color'))]
                          for c in a.get_components_by_class(unreal.LightComponent)]])
        return {'actors': sorted(rows), 'box': {k: encode(box.get_editor_property(k)) for k in protected}}
    map_file = pathlib.Path(unreal.Paths.project_dir())/'Content/FogMS_Test/FogMS_Box.umap'
    original = {'motion': {k: encode(box.get_editor_property(k)) for k in names}, 'reference': reference(),
                'camera': camera(), 'authored': authored(), 'map_sha256': hashlib.sha256(map_file.read_bytes()).hexdigest(),
                'game_view': levels.editor_get_game_view(viewport),
                'throttle': perf.get_editor_property('bThrottleCPUWhenNotForeground'),
                'autosave': auto.get_editor_property('bAutoSaveEnable')}
    assert 'DIRECTIONAL' in original['authored']['box']['density_motion_mode']
    (out/'original.json').write_text(json.dumps(original, indent=2))
    m = types.ModuleType('_fogms_edge_visual'); sys.modules[m.__name__] = m
    m.handle = None; m.done = False; m.start = time.monotonic(); m.index = -1; m.frame = 0; m.phase = 'worker'; m.records=[]
    jobs = [('off-t0',0.,0.), ('off-t10',0.,10.), ('on-t0',20.,0.), ('on-t5',20.,5.), ('on-t10',20.,10.)]
    def setp(k,v): box.set_editor_property(k, unreal.Vector(*v) if isinstance(v,list) else v)
    def phases():
        mid = box.get_editor_property('density_component').get_material(0)
        return [encode(mid.get_vector_parameter_value('FogMS_WorldPhase'+str(i))) for i in range(3)]
    def finish(error=None):
        if m.done: return
        m.done = True
        if m.handle: unreal.unregister_slate_post_tick_callback(m.handle)
        restoration = {}; failures=[]
        for k,v in original['motion'].items():
            try: setp(k,v)
            except BaseException: failures.append(traceback.format_exc())
        for name, fn in [('reference',lambda:restore_reference(original['reference'])),
                         ('game_view',lambda:levels.editor_set_game_view(original['game_view'],viewport)),
                         ('throttle',lambda:perf.set_editor_property('bThrottleCPUWhenNotForeground',original['throttle'])),
                         ('autosave',lambda:auto.set_editor_property('bAutoSaveEnable',original['autosave']))]:
            try: fn(); restoration[name]=True
            except BaseException: restoration[name]=False; failures.append(traceback.format_exc())
        restoration.update(motion={k:encode(box.get_editor_property(k)) for k in names}==original['motion'],
                           reference_exact=reference()==original['reference'], camera_unchanged=camera()==original['camera'],
                           authored_unchanged=authored()==original['authored'],
                           map_unsaved=hashlib.sha256(map_file.read_bytes()).hexdigest()==original['map_sha256'])
        status = 'PASS' if error is None and not failures and all(restoration.values()) else 'FAIL'
        result={'status':status,'error':error,'restore_errors':failures,'restoration':restoration,'captures':m.records,
                'seconds':time.monotonic()-m.start,'scope':'Native LDR viewport morphology check, not radiometric transport validation',
                'timing':'90 frames settle after explicit time seek, then native screenshot request/file observation interval'}
        (out/'receipt.json').write_text(json.dumps(result,indent=2))
        (out/'finished.json').write_text(json.dumps({'status':status}))
        unreal.log('FOGMS_EDGE_VISUAL '+status+' '+str(out))
    def next_job():
        m.index+=1
        if m.index==len(jobs): finish(); return
        name, speed, offset=jobs[m.index]
        setp('manual_animation_time',10000.)
        setp('edge_flow_speed',speed)
        box.reset_motion_origin(); box.update_density()
        setp('manual_animation_time',10000.+offset)
        box.update_density()
        m.frame=0; m.phase='warm'
    def tick(dt):
        try:
            assert time.monotonic()-m.start < config['timeout_seconds'],'Timeout'
            assert camera()==original['camera'],'Camera changed during comparison'
            assert authored()==original['authored'],'Authored bounds/light/parameters changed during comparison'
            assert levels.editor_get_game_view(viewport),'Game view changed'
            if m.phase=='worker':
                if (out/'capture_worker.json').exists(): next_job()
                return
            m.frame+=1
            if m.phase=='warm' and m.frame>=90:
                name,speed,offset=jobs[m.index]; sequence=m.index+1
                m.pending={'sequence':sequence,'name':name,'flow_cm_s':speed,'time_offset':offset,'phase':phases(),
                           'filename':str(out/(name+'.png')),'request_frame':int(unreal.SystemLibrary.get_frame_count())}
                (out/'capture_requests'/('%06d.json'%sequence)).write_text(json.dumps(m.pending))
                m.phase='capture'
            elif m.phase=='capture':
                ack=out/'capture_acks'/('%06d.json'%m.pending['sequence'])
                file=pathlib.Path(m.pending['filename'])
                if ack.exists() and file.exists():
                    result=json.loads(ack.read_text())['result']
                    assert 'error' not in result and result['result']['success'],result
                    m.pending['observed_frame']=int(unreal.SystemLibrary.get_frame_count())
                    m.records.append(m.pending); next_job()
        except BaseException: finish(traceback.format_exc())
    try:
        perf.set_editor_property('bThrottleCPUWhenNotForeground',False)
        auto.set_editor_property('bAutoSaveEnable',False)
        levels.editor_set_game_view(True,viewport)
        for k,v in {'animate_density':True,'use_manual_animation_time':True,'manual_animation_time':10000.,
                    'animation_time_offset':0.,'wind_speed':0.,'density_detail_velocity':[0.,0.,0.],
                    'density_evolution_velocity':[0.,0.,0.]}.items(): setp(k,v)
        m.handle=unreal.register_slate_post_tick_callback(tick)
        unreal.log('FOGMS_EDGE_VISUAL_STARTED '+str(out))
    except BaseException: finish(traceback.format_exc())

if __name__ == '__main__': run()
