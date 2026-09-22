"""Offline harness checks with SMALL SYNTHETIC CPU payloads; never launches UE.

These fixtures verify parsing/accounting/rejection paths, NOT independent GPU
correctness. Generated files remain inside this folder's ignored _verification.
"""
import argparse
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import types
import numpy as np

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parent


def load(name,path):
    spec=importlib.util.spec_from_file_location(name,path)
    module=importlib.util.module_from_spec(spec);sys.modules[name]=module;spec.loader.exec_module(module)
    return module


def run():
    out=ROOT/'_verification';out.mkdir(exist_ok=True)
    checks=[]
    def check(name,value):
        checks.append({'name':name,'pass':bool(value)})
        if not value:raise AssertionError(name)
    for path in ROOT.glob('*.py'):
        compile(path.read_text(encoding='utf-8'),str(path),'exec')
    check('all_python_compiles',True)
    capture=load('b3_capture_offline',ROOT/'gpu_impl.py')
    load('b3_entry_offline',ROOT/'gpu_probe.py')
    check('import_does_not_load_unreal','unreal' not in sys.modules)
    check('nine_unique_cases',len(capture.default_cases())==9 and len({c['name'] for c in capture.default_cases()})==9)
    check('both_quality_values',{c['directions'] for c in capture.default_cases()}=={48,96})
    check('rgb_sentinel_fixture',any(c['albedo']==-1 and len(c['rgb_albedo'])==4 for c in capture.default_cases()))
    base=load('b3_analysis_offline',ROOT/'analyze_gpu.py')
    oracle=base.load_oracle(base.DEFAULT_ORACLE)
    args=types.SimpleNamespace(oracle=base.DEFAULT_ORACLE,cpu_iterations=64,cpu_tolerance=1.e-8,
          error_threshold=.002,flux_threshold=.002,residual_threshold=.002,negative_tolerance=1.e-6,
          luminance_factors=[.2126390059,.7151686788,.0721923154],allow_partial=False)
    cases=[dict(c,reconstruction=0) for c in capture.default_cases()]
    points=[];n=4;extent=np.array([1000.,1000.,1000.]);spacing=2*extent/n
    for index,case in enumerate(cases):
        sigma=np.full((n,n,n),np.float32(case['tau']/2000),dtype=np.float32)
        if case['test']==2:sigma[:,:,n//4:3*n//4]=0
        albedo=np.array(case['rgb_albedo'][:3] if case['albedo']==-1 else [case['albedo']]*3,np.float32)
        scattering=(sigma[...,None]*albedo).astype(np.float32)
        actual_albedo=np.divide(scattering,sigma[...,None],out=np.zeros_like(scattering),where=sigma[...,None]>0)
        model=oracle.AngularTransport(sigma,actual_albedo,spacing,'gauss2x12' if case['directions']==48 else 'gauss3x16')
        if case['geometry'] and case['wall_shown']:model.close_face('x',n//2)
        if case['boundary']==0:model.set_isotropic_boundary(1)
        else:model.faces['x'].plus[:,:,0]=1
        solution,boundary=base.oracle_solution(model,args,case)
        flux=base.oracle_flux_cells(model,solution,args.luminance_factors)
        masks=np.zeros((n,n,n),np.int64)
        for di,(axis,array_axis) in enumerate(base.AXES.items()):
            for k in range(n):
                masks[base.sl(array_axis,k)] |= model.faces[axis].open[base.sl(array_axis,k)].astype(np.int64)<<(2*di)
                masks[base.sl(array_axis,k)] |= model.faces[axis].open[base.sl(array_axis,k+1)].astype(np.int64)<<(2*di+1)
        payload=np.zeros((4,n,n,n,4),np.float32)
        payload[0,...,:3]=solution['total'];payload[1,...,:3]=boundary;payload[1,...,3]=masks
        payload[2,...,:3]=scattering;payload[2,...,3]=sigma;payload[3]=flux
        values=payload.astype(np.float64);sums=values[3].sum(axis=(0,1,2))
        defect=(sums[1]+sums[2]-sums[0]-sums[3])/max(abs(sums[0]+sums[3]),1.e-20)
        point={'case':case,'box':{'half_extent_cm':extent.tolist(),'center':[0.,0.,0.],'rotation':[0.,0.,0.]},'wall':None}
        if case['geometry']:
            wall_actor={'path':'synthetic-wall','hidden_editor':not case['wall_shown'],'location':[0.,0.,0.],
                        'rotation':[0.,0.,0.],'scale':[.05,40.,40.]}
            point['wall']={'box_basis':np.eye(3).tolist(),'static_meshes':[wall_actor],
               'actor_path':'synthetic-wall','asset':'/Engine/BasicShapes/Cube.Cube',
               'local_center_x_cm':0.,'thickness_cm':5.}
        for role,frame in [('barrier',100+index*5),('measurement',103+index*5)]:
            prefix=out/(case['name']+'-'+role)
            path=Path(str(prefix)+'.rgba32f');payload.astype('<f4').tofile(path)
            meta={'success':True,'domain':'transport','format':'RGBA32F_LE','layout':base.LAYOUT,
                'grid':n,'width':n,'height':4*n*n,'bytes':payload.nbytes,'directions':case['directions'],
                'transport_scheme':'upwind_half_gauss','iterations':24,'test':case['test'],
                'testTau':case['tau'],'testAlbedo':case['albedo'],'testGeometry':case['geometry'],
                'testBoundary':case['boundary'],'cellSizeCm':spacing.tolist(),'viewKey':1,
                'dumpRenderFrame':frame,'sourceProducedRenderFrame':frame,'finite':True,
                'diffuseIncoming':float(sums[0]),'diffuseOutgoing':float(sums[1]),
                'diffuseAbsorbed':float(sums[2]),'directScatteringSource':float(sums[3]),
                'relativeFluxDefect':float(defect),'maxRelativeCellResidual':0.,
                'minRadiance':float(values[0,...,:3].min())}
            Path(str(prefix)+'.json').write_text(json.dumps(meta),encoding='utf-8')
            controls={k:str(case[k]) for k in ('test','tau','albedo','geometry','boundary','reconstruction')}
            point[role]={'prefix':str(prefix),'metadata':meta,'rgba_sha256':base.sha(path),
                         'cvars':controls,'cvars_after':controls.copy()}
        points.append(point)
    receipt=out/'synthetic-capture.json'
    saved={'schema':'fogms_b3_gpu_probe_v1','status':'COMPLETED','complete_suite':True,
           'planned_cases':cases,'cases':points,'restoration_ok':True,'restoration':{'ok':True},'errors':[],'iterations':24,
           'scope':'SMALL SYNTHETIC CPU-ORACLE FIXTURES; NOT A GPU RUN'}
    receipt.write_text(json.dumps(saved,indent=2),encoding='utf-8')
    report=base.analyze(receipt,args)
    (out/'synthetic-analysis.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
    check('synthetic_full_analyzer_passes',report['result']=='PASS')
    check('synthetic_wall_pair_passes',report['wall_pair']['status']=='PASS')
    record=points[0]['measurement'];meta_path=Path(record['prefix']+'.json');clean=copy.deepcopy(record)
    faults={'wrong_directions':{'directions':6},'wrong_scheme':{'transport_scheme':'six_axis_exponential'},
            'stale_source':{'sourceProducedRenderFrame':1},'wrong_albedo':{'testAlbedo':.2}}
    for name,updates in faults.items():
        bad=copy.deepcopy(clean);bad['metadata'].update(updates)
        meta_path.write_text(json.dumps(bad['metadata']),encoding='utf-8')
        rejected=False
        try:base.validate_dump(bad,cases[0],24)
        except ValueError:rejected=True
        finally:meta_path.write_text(json.dumps(clean['metadata']),encoding='utf-8')
        check('reject_'+name,rejected)
    source=(ROOT/'gpu_impl.py').read_text(encoding='utf-8')
    check('no_save_spawn_delete_commands',all(x not in source for x in ('save_current_level(', 'spawn_actor', 'destroy_actor(', 'save_all_dirty')))
    check('new_controls_restored',all(x in source for x in ("original['quality_name']","original['density_albedo']","checks['quality']","checks['albedo']")))
    # This is an explicit STATIC recovery check, not a mocked UE callback run.
    result={'scope':'offline synthetic analyzer and static capture checks only; UE not imported or operated',
            'result':'PASS','checks':checks,'count':len(checks)}
    (out/'offline-receipt.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
    print(json.dumps({'result':result['result'],'checks':len(checks),'receipt':str(out/'offline-receipt.json')}))


if __name__=='__main__':
    run()
