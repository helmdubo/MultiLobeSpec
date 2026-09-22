# On-demand offline analysis only; never operates Unreal Editor.
"""Offline B2 thin-wall acceptance: independent geometry plus six-axis CPU oracle.

Reuses the frozen numeric analyzer for its four-slab numerical/flux checks.
No Unreal import, scene access or mutation. Requires NumPy.
"""
from __future__ import annotations
import argparse
import importlib.util
import json
import math
import pathlib
import sys
import traceback
import numpy as np

sys.dont_write_bytecode = True
ROOT = pathlib.Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('_fogms_b2_numeric_analysis', ROOT/'analyze_gpu.py')
base = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = base
spec.loader.exec_module(base)
native_decode_faces = base.decode_faces


def wall_decode_faces(model, masks, case):
    count = native_decode_faces(model, masks, case)
    # External faces are closed in every dump, so their bit does not encode
    # visibility of the half-cell boundary. This independently specified wall
    # blocks all negative-X inflow, and must produce an exactly black domain.
    if case['wall_shown'] and case['wall_position'] == 'boundary_half_cell':
        model.faces['x'].plus[:, :, 0] = 0.
    return count


base.decode_faces = wall_decode_faces


def expected_topology(n, extent, wall_x, thickness, shown):
    masks = np.zeros((n, n, n), dtype=np.int64)
    centers = (np.arange(n)+.5)*(2*extent[0]/n)-extent[0]
    cut = np.zeros(n-1, dtype=bool)
    if shown:
        lo, hi = wall_x-.5*thickness, wall_x+.5*thickness
        if np.any((centers >= lo-.01) & (centers <= hi+.01)):
            raise ValueError('A cell center lies inside/touches the wall; this bounded test excludes that ambiguity')
        cut = (centers[:-1] < hi) & (centers[1:] > lo)
    for index, (name, axis) in enumerate(base.AXES.items()):
        for plane in range(1, n):
            opened = name != 'x' or not cut[plane-1]
            if opened:
                masks[base.sl(axis,plane-1)] |= 1 << (2*index+1)
                masks[base.sl(axis,plane)] |= 1 << (2*index)
    return masks, (np.flatnonzero(cut)+1).tolist()


def geometry_check(point, data):
    case, wall, box = point['case'], point['wall'], point['box']
    n = data.shape[1]
    extent = np.asarray(box['half_extent_cm'], dtype=float)
    dx = 2*extent[0]/n
    basis = np.asarray(wall['box_basis'], dtype=float)
    if basis.shape != (3,3) or not np.allclose(basis@basis.T,np.eye(3),rtol=0,atol=1.e-7):
        raise ValueError('Box basis is not orthonormal')
    meshes = wall['static_meshes']
    actual = next(a for a in meshes if a['path'] == wall['actor_path'])
    actual_local = basis@(np.asarray(actual['location'])-np.asarray(box['center']))
    requested = {'center':0.,'quarter_cell':.25*dx,'boundary_half_cell':-extent[0]+.25*dx}[case['wall_position']]
    if not np.allclose(actual_local,[requested,0,0],rtol=0,atol=1.e-4):
        raise ValueError('Actual wall pose disagrees with requested box-local placement')
    if not np.allclose(actual['rotation'],box['rotation'],rtol=0,atol=1.e-6):
        raise ValueError('Wall orientation differs from Box orientation')
    actual_size = 100*np.asarray(actual['scale'],dtype=float)
    if not math.isclose(actual_size[0],5.,abs_tol=1.e-6) or np.any(actual_size[1:] < 2*extent[1:]+.01):
        raise ValueError('Wall must be 5 cm thick and completely span the YZ domain')
    if wall['asset'] != '/Engine/BasicShapes/Cube.Cube':
        raise ValueError('Only the confirmed native cube geometry is supported')
    if bool(actual['hidden_editor']) == bool(case['wall_shown']):
        raise ValueError('Wall visibility disagrees with case')
    if any(not a['hidden_editor'] for a in meshes if a['path'] != wall['actor_path']):
        raise ValueError('Another StaticMeshActor remained visible')
    masks, cuts = expected_topology(n,extent,actual_local[0],actual_size[0],case['wall_shown'])
    actual_masks = np.rint(data[1,...,3]).astype(np.int64) & 63
    mismatch = int(np.count_nonzero(masks != actual_masks))
    return {'expected_closed_x_planes':cuts,'topology_mismatch_cells':mismatch,
            'wall_local_center_x_cm':float(actual_local[0]),'wall_thickness_cm':float(actual_size[0]),
            'expected_inflow_negative_x':0. if case['wall_shown'] and case['wall_position']=='boundary_half_cell' else 1.,
            'actual_masks_match_expected':mismatch==0}


def analyze(path,args):
    saved=json.loads(path.read_text(encoding='utf-8-sig'))
    report={'schema':'fogms_b2_wall_analysis_v1','receipt':str(path),'receipt_sha256':base.sha(path),
            'oracle':str(args.oracle),'oracle_sha256':base.sha(args.oracle),
            'result':'FAIL','errors':[],'cases':[],'pairs':[],
            'limits':['Six-axis discrete transport verification only; not angular-accuracy acceptance.',
                      'Known black wall covers the whole YZ domain; no Lumen surface-card radiance is required.',
                      'Neither capture elapsed time nor stalled readback duration is a GPU performance benchmark.']}
    if saved.get('schema')!='fogms_b2_wall_probe_v1' or saved.get('status')!='COMPLETED':
        report['errors'].append('Wall capture is not a completed known-schema receipt')
    if not saved.get('restoration_ok') or not saved.get('restoration',{}).get('ok') or saved.get('errors'):
        report['errors'].append('Harness errors or original-state restoration was not verified')
    planned=saved.get('planned_cases',[])
    points=saved.get('cases',[])
    expected=[(position,shown) for position in ('center','quarter_cell','boundary_half_cell') for shown in (False,True)]
    if [(c.get('wall_position'),c.get('wall_shown')) for c in planned]!=expected or len(points)!=6:
        report['errors'].append('Require all three matched no-wall/wall pairs')
    oracle=base.load_oracle(args.oracle)
    loaded={}
    for index,point in enumerate(points):
        case=point['case']
        row=base.analyze_case(point,int(saved['iterations']),oracle,args)
        try:
            if index>=len(planned) or case!=planned[index]:
                raise ValueError('Captured case differs from plan')
            if any(case[k]!=v for k,v in {'test':1,'tau':4,'albedo':.9,'boundary':1,'geometry':1}.items()):
                raise ValueError('Wall contract requires Test1 tau4 albedo.9 boundary1 geometry1')
            meta,data=base.validate_dump(point['measurement'],case,int(saved['iterations']))
            row['geometry']=geometry_check(point,data)
            if not row['geometry']['actual_masks_match_expected']:
                row['errors'].append('Actual low6 face topology differs from independently expected wall geometry')
            if row.get('missing_surface_boundary_flags',0)!=0:
                row['errors'].append('Diagnostic black-wall boundary must not depend on missing surface cards')
            if 'missingSurfaceBoundarySamples' in meta and int(meta['missingSurfaceBoundarySamples'])!=0:
                row['errors'].append('Native missing-surface count is nonzero in a controlled diagnostic boundary')
            loaded[(case['wall_position'],bool(case['wall_shown']))]=(point,data)
        except Exception:
            row['errors'].append(traceback.format_exc())
        row['result']='PASS' if not row['errors'] else 'FAIL'
        report['cases'].append(row)
        print(json.dumps({'case':row['name'],'result':row['result'],'errors':row['errors']}),flush=True)
    no_wall_fields=[]
    for position in ('center','quarter_cell','boundary_half_cell'):
        pair={'position':position,'errors':[]}
        try:
            p0,d0=loaded[(position,False)]
            p1,d1=loaded[(position,True)]
            no_wall_fields.append(d0[0,...,:3])
            n=d0.shape[1]
            extent=np.asarray(p1['box']['half_extent_cm'],dtype=float)
            wall_x=float(p1['wall']['local_center_x_cm'])
            centers=(np.arange(n)+.5)*(2*extent[0]/n)-extent[0]
            behind=centers>wall_x+.5*p1['wall']['thickness_cm']
            no_wall=d0[0,:,:,behind,:3]
            blocked=d1[0,:,:,behind,:3]
            peak=max(float(np.max(d0[0,...,:3])),1.e-12)
            pair.update(no_wall_reference_peak=peak,no_wall_behind_mean=float(np.mean(no_wall)),
                        shadow_behind_max_abs=float(np.max(np.abs(blocked))),
                        leakage_relative_to_no_wall_peak=float(np.max(np.abs(blocked))/peak),
                        behind_cell_count=int(np.count_nonzero(behind)*n*n))
            if pair['no_wall_behind_mean']<=1.e-5:
                pair['errors'].append('Matched no-wall source is too dark to establish occlusion')
            if pair['leakage_relative_to_no_wall_peak']>args.leakage_threshold:
                pair['errors'].append('Radiance leaked behind the complete black wall')
            if position=='boundary_half_cell':
                pair['whole_domain_max_abs']=float(np.max(np.abs(d1[0,...,:3])))
                pair['negative_x_incoming_flux']=float(np.sum(d1[3,...,0]))
                if pair['whole_domain_max_abs']>args.leakage_threshold*peak or pair['negative_x_incoming_flux']!=0.:
                    pair['errors'].append('Boundary half-cell did not remove synthetic incoming illumination')
            if not np.array_equal(d0[2],d1[2]):
                pair['errors'].append('Matched wall/no-wall media coefficients differ')
        except Exception:
            pair['errors'].append(traceback.format_exc())
        pair['result']='PASS' if not pair['errors'] else 'FAIL'
        report['pairs'].append(pair)
    if len(no_wall_fields)==3:
        report['no_wall_repeats']=[base.metric(a,no_wall_fields[0]) for a in no_wall_fields[1:]]
        if any(m['max_relative_to_reference_peak']>1.e-5 for m in report['no_wall_repeats']):
            report['errors'].append('Hidden wall relocation changed the matched no-wall field')
    if not report['errors'] and len(report['cases'])==6 and all(r['result']=='PASS' for r in report['cases']+report['pairs']):
        report['result']='PASS'
    return report


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('receipt',type=pathlib.Path)
    p.add_argument('--oracle',type=pathlib.Path,default=base.DEFAULT_ORACLE)
    p.add_argument('--output',type=pathlib.Path)
    p.add_argument('--cpu-iterations',type=int,default=128)
    p.add_argument('--cpu-tolerance',type=float,default=1.e-10)
    p.add_argument('--error-threshold',type=float,default=.01)
    p.add_argument('--flux-threshold',type=float,default=.01)
    p.add_argument('--residual-threshold',type=float,default=.01)
    p.add_argument('--negative-tolerance',type=float,default=1.e-6)
    p.add_argument('--leakage-threshold',type=float,default=1.e-6)
    args=p.parse_args()
    if any(not math.isfinite(getattr(args,k)) or getattr(args,k)<=0 for k in
           ('cpu_tolerance','error_threshold','flux_threshold','residual_threshold','negative_tolerance','leakage_threshold')):
        p.error('Thresholds must be finite and positive')
    output=args.output or args.receipt.with_name(args.receipt.stem+'-analysis.json')
    if output.resolve()==args.receipt.resolve():
        p.error('Do not overwrite the capture receipt')
    result=analyze(args.receipt,args)
    output.write_text(json.dumps(result,indent=2,allow_nan=False),encoding='utf-8')
    print(json.dumps({'result':result['result'],'output':str(output)}))
    return 0 if result['result']=='PASS' else 2


if __name__=='__main__':
    sys.exit(main())
