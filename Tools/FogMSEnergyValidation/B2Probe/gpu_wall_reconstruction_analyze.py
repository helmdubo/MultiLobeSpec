"""On-demand offline receiver-reconstruction gate; no flux analysis or UE access.

The fourth atlas slab holds max receiver RGB across eight samples per cell,
not flux. Ordinary numerical acceptance still requires gpu_wall_analyze.py.
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
spec = importlib.util.spec_from_file_location('_fogms_reconstruction_geometry', ROOT/'gpu_wall_analyze.py')
wall = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = wall
spec.loader.exec_module(wall)
base = wall.base
LAYOUT = 'x,y+z*N; slabs: totalJ/residual, primaryJ/openFaces, sigma_s/sigma_t, maxReceiverRGB/valid'
EXPECTED = [(p,s) for p in ('center','quarter_cell','boundary_half_cell') for s in (False,True)]


def read_dump(record,case,iterations):
    prefix = pathlib.Path(record['prefix'])
    meta = json.loads(pathlib.Path(str(prefix)+'.json').read_text(encoding='utf-8-sig'))
    binary = pathlib.Path(str(prefix)+'.rgba32f')
    if meta != record['metadata']:
        raise ValueError('Metadata changed after capture')
    if meta.get('success') is not True or meta.get('domain') != 'transport_reconstruction' or meta.get('format') != 'RGBA32F_LE' or meta.get('layout') != LAYOUT:
        raise ValueError('Require the dedicated reconstruction domain/layout')
    if meta.get('reconstructionTest') is not True or meta.get('fluxDiagnosticsValid') is not False:
        raise ValueError('Reconstruction/flux flags disagree; cannot interpret this fourth slab')
    n = int(meta['grid'])
    if n < 1 or meta['width'] != n or meta['height'] != 4*n*n or meta['bytes'] != 4*n**3*16 or binary.stat().st_size != meta['bytes']:
        raise ValueError('Invalid reconstruction atlas dimensions/payload size')
    if base.sha(binary) != record.get('rgba_sha256'):
        raise ValueError('Payload hash does not match the completed capture')
    for key,field in [('test','test'),('tau','testTau'),('albedo','testAlbedo'),('geometry','testGeometry'),('boundary','testBoundary')]:
        if not math.isclose(float(meta[field]),float(case[key]),rel_tol=2.e-6,abs_tol=1.e-7):
            raise ValueError('Requested input differs from metadata: '+key)
    for phase in ('cvars','cvars_after'):
        for key in ('test','tau','albedo','geometry','boundary','reconstruction'):
            if not math.isclose(float(record[phase][key]),float(case[key]),rel_tol=2.e-6,abs_tol=1.e-7):
                raise ValueError('Live CVar differs from planned case: '+phase+'/'+key)
    if int(meta['iterations']) != iterations or int(meta['directions']) != 6:
        raise ValueError('Wrong iteration/direction count')
    if ((int(meta['dumpRenderFrame'])-int(meta['sourceProducedRenderFrame'])) & 0xffffffff) > 2:
        raise ValueError('Stale reconstruction producer')
    data = np.fromfile(binary,dtype='<f4').astype(np.float64).reshape(4,n,n,n,4)
    if not np.isfinite(data).all():
        raise ValueError('Nonfinite value in the reconstruction atlas')
    return meta,data


def analyze_case(point,iterations,args):
    case = point['case']
    row = {'name':case['name'],'case':case,'errors':[],'result':'FAIL'}
    data = None
    try:
        for key,value in {'test':1,'tau':4,'albedo':.9,'geometry':1,'boundary':1,'reconstruction':1}.items():
            if case[key] != value:
                raise ValueError('Unexpected reconstruction fixture input: '+key)
        bm,barrier = read_dump(point['barrier'],case,iterations)
        meta,data = read_dump(point['measurement'],case,iterations)
        n = int(meta['grid'])
        frames = (int(meta['dumpRenderFrame'])-int(bm['dumpRenderFrame'])) & 0xffffffff
        produced = (int(meta['sourceProducedRenderFrame'])-int(bm['sourceProducedRenderFrame'])) & 0xffffffff
        row.update(render_frames_after_barrier=frames,produced_frames_after_barrier=produced)
        if not (2 <= frames < 2**31 and 2 <= produced < 2**31) or meta['viewKey'] != bm['viewKey']:
            raise ValueError('Require two fresh frames from the same persistent view')
        if n != point['wall']['grid']:
            raise ValueError('Wall fixture used a different grid resolution')
        extent = np.asarray(point['box']['half_extent_cm'],dtype=np.float32)
        spacing = (2*extent/n).astype(np.float32)
        if not np.allclose(np.asarray(meta['cellSizeCm']),spacing,rtol=2.e-6,atol=1.e-5):
            raise ValueError('Metadata cell size differs from the live Box')
        if not np.array_equal(barrier[2],data[2]) or not np.array_equal(barrier[1,...,3],data[1,...,3]):
            raise ValueError('Coefficients/topology changed between barrier and measurement')
        sigma = np.float32(np.float32(4.)/np.float32(2*extent[0]))
        if not np.allclose(data[2,...,3],sigma,rtol=3.e-6,atol=1.e-11) or not np.allclose(data[2,...,:3],np.float32(sigma*np.float32(.9)),rtol=3.e-6,atol=1.e-11):
            raise ValueError('Dumped coefficients differ from the controlled uniform medium')
        if np.any(data[2] < 0) or np.min(data[0,...,:3]) < -1.e-6 or np.min(data[1,...,:3]) < -1.e-6:
            raise ValueError('Negative coefficients/source radiance')
        masks = data[1,...,3]
        packed = np.rint(masks).astype(np.int64)
        if np.any(masks != packed) or np.any(packed < 0) or np.any(packed > 4095):
            raise ValueError('Invalid packed face bits')
        row['missing_surface_flag_cells'] = int(np.count_nonzero((packed >> 6)&63))
        if row['missing_surface_flag_cells']:
            raise ValueError('Controlled black boundary unexpectedly needs missing surface-card data')
        row['geometry'] = wall.geometry_check(point,data)
        if not row['geometry']['actual_masks_match_expected']:
            raise ValueError('Native topology differs from independently expected wall geometry')
        for label,field in [('barrier',barrier),('measurement',data)]:
            if not np.all(field[3,...,3] == 1.):
                raise ValueError(label+': one or more groups of eight receiver samples were invalid')
            if np.min(field[3,...,:3]) < -1.e-6:
                raise ValueError(label+': negative reconstructed radiance')
        row.update(receiver_min=float(data[3,...,:3].min()),receiver_max=float(data[3,...,:3].max()),
                   receiver_valid_cells=int(np.count_nonzero(data[3,...,3] == 1.)),
                   source_max_relative_cell_residual=float(data[0,...,3].max()),
                   rgba_sha256=base.sha(point['measurement']['prefix']+'.rgba32f'))
        if np.any(data[0,...,3] < 0) or row['source_max_relative_cell_residual'] > args.residual_threshold:
            raise ValueError('Underlying cell solution residual is outside the diagnostic limit')
    except Exception:
        row['errors'].append(traceback.format_exc())
    row['result'] = 'PASS' if not row['errors'] else 'FAIL'
    return row,data


def analyze(path,args):
    saved = json.loads(path.read_text(encoding='utf-8-sig'))
    result = {'schema':'fogms_b2_reconstruction_analysis_v1','receipt':str(path),'receipt_sha256':base.sha(path),
              'scope':'GPU native receiver reconstruction isolation; not full numerical/flux acceptance',
              'flux_diagnostics':'UNAVAILABLE: fourth slab is maxReceiverRGB/valid; no flux checks claimed',
              'sample_offsets_per_axis':[.02,.98],'samples_per_cell':8,'errors':[],'cases':[],'pairs':[],
              'thresholds':{'leakage_relative_to_no_wall_peak':args.leakage_threshold,'source_residual':args.residual_threshold},
              'limits':['Six-direction discretized transport and three full-YZ black-wall placements only.',
                        'Max per-channel combines eight samples; it is not the radiance of one sample.',
                        'Raw native sampler is exercised only when capture used the reconstruction shader pass.',
                        'Readback timings are not GPU performance measurements.']}
    if saved.get('schema') != 'fogms_b2_wall_probe_v1' or saved.get('status') != 'COMPLETED' or saved.get('reconstruction_test') is not True:
        result['errors'].append('Require a completed explicitly configured reconstruction receipt')
    if not saved.get('restoration_ok') or not saved.get('restoration',{}).get('ok') or saved.get('errors'):
        result['errors'].append('Harness errors or fresh state restoration was not verified')
    planned,points = saved.get('planned_cases',[]),saved.get('cases',[])
    if len(points) != 6 or [(p.get('wall_position'),p.get('wall_shown')) for p in planned] != EXPECTED:
        result['errors'].append('Require all three matched no-wall/wall pairs')
    loaded = {}
    for index,point in enumerate(points):
        row,data = analyze_case(point,int(saved['iterations']),args)
        if index >= len(planned) or point['case'] != planned[index]:
            row['errors'].append('Captured case differs from plan');row['result']='FAIL'
        result['cases'].append(row)
        if data is not None:
            loaded[(point['case']['wall_position'],bool(point['case']['wall_shown']))]=(point,data)
        print(json.dumps({'case':row['name'],'result':row['result'],'errors':row['errors']}),flush=True)
    controls = []
    for position in ('center','quarter_cell','boundary_half_cell'):
        pair = {'position':position,'errors':[]}
        try:
            p0,no_wall = loaded[(position,False)]
            p1,blocked = loaded[(position,True)]
            n = no_wall.shape[1]
            start = 0 if position == 'boundary_half_cell' else n//2
            reference = no_wall[3,:,:,start:,:3]
            shadow = blocked[3,:,:,start:,:3]
            source = blocked[0,:,:,start:,:3]
            peak = max(float(np.max(no_wall[3,...,:3])),1.e-12)
            pair.update(shadow_x_index_begin=start,shadow_cell_count=int(n*n*(n-start)),
                        no_wall_receiver_peak=peak,no_wall_receiver_behind_mean=float(reference.mean()),
                        receiver_shadow_max_abs=float(np.max(np.abs(shadow))),
                        receiver_leakage_relative=float(np.max(np.abs(shadow))/peak),
                        source_cell_shadow_max_abs=float(np.max(np.abs(source))))
            if pair['no_wall_receiver_behind_mean'] <= 1.e-5:
                pair['errors'].append('Matched no-wall receiver control is dark')
            if pair['receiver_leakage_relative'] > args.leakage_threshold:
                pair['errors'].append('Native receiver reconstruction leaked across the closed wall')
            if pair['source_cell_shadow_max_abs'] > args.leakage_threshold*peak:
                pair['errors'].append('Cell field already leaks; cannot attribute this only to reconstruction')
            if not np.array_equal(no_wall[2],blocked[2]):
                pair['errors'].append('Matched media coefficients differ')
            controls.append(no_wall[3,...,:3])
        except Exception:
            pair['errors'].append(traceback.format_exc())
        pair['result']='PASS' if not pair['errors'] else 'FAIL'
        result['pairs'].append(pair)
    if len(controls)==3:
        result['no_wall_repeats']=[base.metric(x,controls[0]) for x in controls[1:]]
        if any(x['max_relative_to_reference_peak'] > 1.e-5 for x in result['no_wall_repeats']):
            result['errors'].append('Relocating the hidden cube changed the receiver control field')
    result['result']='PASS' if not result['errors'] and len(result['cases'])==6 and all(r['result']=='PASS' for r in result['cases']+result['pairs']) else 'FAIL'
    return result


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('receipt',type=pathlib.Path)
    p.add_argument('--output',type=pathlib.Path)
    p.add_argument('--leakage-threshold',type=float,default=1.e-6)
    p.add_argument('--residual-threshold',type=float,default=.01)
    args=p.parse_args()
    if any(not math.isfinite(getattr(args,k)) or getattr(args,k)<=0 for k in ('leakage_threshold','residual_threshold')):
        p.error('Thresholds must be finite and positive')
    output=args.output or args.receipt.with_name(args.receipt.stem+'-reconstruction-analysis.json')
    if output.resolve()==args.receipt.resolve():p.error('Do not overwrite the capture receipt')
    result=analyze(args.receipt,args)
    output.write_text(json.dumps(result,indent=2,allow_nan=False),encoding='utf-8')
    print(json.dumps({'result':result['result'],'output':str(output)}))
    return 0 if result['result']=='PASS' else 2


if __name__=='__main__':
    sys.exit(main())
