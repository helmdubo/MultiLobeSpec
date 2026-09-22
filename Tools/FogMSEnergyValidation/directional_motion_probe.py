"""Explicit UE Python probe. Runs only as a script; does not save the map."""
def run():
    import unreal, json, pathlib, datetime, traceback
    root=pathlib.Path('E:/GITHUB/MultiLobeSpec/.codex-build/FogMS_Motion_20260921')
    out=root/('directional-'+datetime.datetime.now().strftime('%H%M%S-%f')+'.json')
    actors=unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    box=next(a for a in actors.get_all_level_actors() if a.get_class().get_name()=='FogMSBoxVolume')
    if not callable(getattr(box,'restore_density_motion_reference',None)):
        raise RuntimeError('Install the package with the explicit restore_density_motion_reference API before probing.')
    names=['animate_density','use_manual_animation_time','manual_animation_time','animation_time_offset',
           'density_wind_velocity','density_detail_velocity','density_evolution_velocity','wind_speed',
           'texture_offset_world','density_motion_mode','density_motion_reference']
    saved={k:box.get_editor_property(k) for k in names}
    wind=box.get_editor_property('wind_direction_component')
    rotation=wind.get_world_rotation()
    reference_fields=['initialized','time','displacement0','displacement1','displacement2','velocity0','velocity1','velocity2']
    def encode(v):
        if isinstance(v,(bool,float,int,str)):return v
        for fields in [('pitch','yaw','roll'),('x','y','z')]:
            if all(hasattr(v,f) for f in fields):return [float(getattr(v,f)) for f in fields]
        if isinstance(v,unreal.FogMSDensityMotionReference):
            return {k:encode(v.get_editor_property(k)) for k in reference_fields}
        return str(v)
    # get_editor_property returns reflected structs by reference (PyUtil.cpp).
    # Persist plain values before mutation; never restore live Unreal wrappers.
    original={k:encode(v) for k,v in saved.items()}
    def arrow_state():
        component=box.get_editor_property('wind_direction_component')
        return {'path':component.get_path_name(),'class':component.get_class().get_name(),
                'rotation':encode(component.get_world_rotation()),
                'absolute_location':bool(component.get_editor_property('absolute_location')),
                'absolute_rotation':bool(component.get_editor_property('absolute_rotation')),
                'absolute_scale':bool(component.get_editor_property('absolute_scale'))}
    original_arrow=arrow_state()
    data={'status':'RUNNING','original':original,'wind_rotation':encode(rotation),
          'original_arrow':original_arrow,'checks':{},'samples':{},'error':None}
    out.write_text(json.dumps(data,indent=2),encoding='utf-8')
    def setp(**values):
        for k,v in values.items():box.set_editor_property(k,v)
        box.update_density()
    def phases():
        mid=box.get_editor_property('density_component').get_material(0)
        return [tuple(getattr(mid.get_vector_parameter_value('FogMS_WorldPhase'+str(i)),v) for v in ('r','g','b')) for i in range(3)]
    def delta(a,b):return max(abs((x-y+.5)%1-.5) for av,bv in zip(a,b) for x,y in zip(av,bv))
    def sample(name):
        data['samples'][name]={'phases':phases(),'reference':encode(box.get_editor_property('density_motion_reference')),
                              'time':float(box.get_editor_property('density_animation_time')),
                              'status':str(box.get_editor_property('density_animation_status')),
                              'mode':str(box.get_editor_property('density_motion_mode')),
                              'wind_speed':float(box.get_editor_property('wind_speed')),
                              'arrow':arrow_state()}
        out.write_text(json.dumps(data,indent=2),encoding='utf-8')
    def check(name,ok):
        data['checks'][name]=bool(ok)
        if not ok:raise AssertionError(name)
    try:
        if 'LEGACY_VECTORS' in str(saved['density_motion_mode']):
            check('legacy_asset_migration',str(saved['density_motion_mode']).endswith('LEGACY_VECTORS: 0>'))
        else:
            check('existing_directional_mode',str(saved['density_motion_mode']).endswith('DIRECTIONAL: 1>'))
        static=phases()
        box.use_directional_motion();box.update_density()
        check('static_conversion_parity',delta(static,phases())==0)
        setp(animate_density=True,use_manual_animation_time=True,manual_animation_time=10000.,animation_time_offset=0.,
             density_detail_velocity=unreal.Vector(),density_evolution_velocity=unreal.Vector(),wind_speed=125.)
        box.reset_motion_origin();box.update_density()
        a=phases()
        sample('before_speed_edit')
        setp(wind_speed=750.)
        sample('after_speed_edit')
        check('speed_edit_no_jump_after_long_time',delta(a,phases())==0)
        wind=box.get_editor_property('wind_direction_component')
        wind.set_world_rotation(unreal.Rotator(pitch=15.,yaw=65.,roll=0.),False,False);box.update_density()
        sample('after_arrow_edit')
        check('arrow_edit_no_jump',delta(a,phases())==0)
        edited_arrow=arrow_state()
        check('arrow_uses_independent_world_rotation_scale',
              not edited_arrow['absolute_location'] and edited_arrow['absolute_rotation'] and edited_arrow['absolute_scale'])
        # An actor property edit reruns construction after the component was
        # rotated. Reacquire the component to inspect the surviving instance.
        setp(wind_speed=375.)
        sample('after_reconstruction_with_rotated_arrow')
        check('arrow_settings_survive_actor_reconstruction',arrow_state()==edited_arrow)
        check('second_speed_edit_preserves_phase',delta(a,phases())==0)
        # A snapshot of an already authored directional reference is restored
        # through the public API and must still support continuous later edits.
        checkpoint=encode(box.get_editor_property('density_motion_reference'))
        replay=unreal.FogMSDensityMotionReference()
        for k,v in checkpoint.items():replay.set_editor_property(k,unreal.Vector(*v) if isinstance(v,list) else v)
        check('serialized_directional_reference_restore_accepted',box.restore_density_motion_reference(replay))
        check('serialized_directional_reference_replay_phase',delta(a,phases())==0)
        setp(wind_speed=0.)
        setp(manual_animation_time=10010.)
        check('zero_speed_holds_phase',delta(a,phases())==0)
        setp(texture_offset_world=unreal.Vector(100.,-50.,20.))
        check('world_offset_moves_static_pattern',delta(a,phases())>1.e-5)
        before=phases()
        box.freeze_density_animation();box.update_density()
        check('freeze_no_jump',delta(before,phases())<1.e-7)
        box.resume_density_animation();box.update_density()
        check('resume_no_jump',delta(before,phases())<1.e-7)
        data['status']='PASS'
    except BaseException:
        data['status']='FAIL';data['error']=traceback.format_exc()
    finally:
        try:
            data['before_restore']={k:encode(box.get_editor_property(k)) for k in names}
            data['saved_wrappers_before_restore']={k:encode(v) for k,v in saved.items()}
            if 'LEGACY_VECTORS' in original['density_motion_mode']:box.use_legacy_motion()
            else:box.use_directional_motion()
            for k,v in original.items():
                if k not in ('density_motion_mode','density_motion_reference'):
                    box.set_editor_property(k,unreal.Vector(*v) if isinstance(v,list) else v)
            wind=box.get_editor_property('wind_direction_component')
            pitch,yaw,roll=original_arrow['rotation']
            wind.set_world_rotation(unreal.Rotator(pitch=pitch,yaw=yaw,roll=roll),False,False)
            reference=unreal.FogMSDensityMotionReference()
            for k,v in original['density_motion_reference'].items():
                reference.set_editor_property(k,unreal.Vector(*v) if isinstance(v,list) else v)
            if not box.restore_density_motion_reference(reference):raise RuntimeError('Motion reference restoration rejected')
            box.update_density()
            actual={k:encode(box.get_editor_property(k)) for k in names}
            actual_arrow=arrow_state()
            data['actual_after_restore']=actual
            data['arrow_after_restore']=actual_arrow
            data['restore_checks']={k:actual[k]==original[k] for k in names}
            data['restore_checks']['arrow']=actual_arrow==original_arrow
            data['restore_differences']={k:{'expected':original[k],'actual':actual[k]} for k in names if actual[k]!=original[k]}
            if actual_arrow!=original_arrow:
                data['restore_differences']['arrow']={'expected':original_arrow,'actual':actual_arrow}
            data['restored']=all(data['restore_checks'].values())
            if not data['restored']:data['status']='RESTORE_FAILED'
        except BaseException:
            data['status']='RESTORE_FAILED';data['restore_error']=traceback.format_exc()
        out.write_text(json.dumps(data,indent=2),encoding='utf-8')
        unreal.log('FOGMS_DIRECTIONAL_PROBE '+str(out)+' '+data['status'])

if __name__=='__main__':run()
