# On-demand offline analysis only; never operates Unreal Editor.
"""Compare B3 four-slab GPU dumps with the independent paired-ordinate FV CPU oracle.

Offline only. Requires NumPy; never imports unreal or operates the editor.
A matching paired-ordinate FV oracle verifies implementation, not angular accuracy.
"""
from __future__ import annotations
import argparse
import hashlib
import importlib.util
import json
import math
import pathlib
import sys
import time
import traceback
import numpy as np
sys.dont_write_bytecode = True

LAYOUT = 'x,y+z*N; slabs: totalJ/residual, primaryJ/openFaces, sigma_s/sigma_t, diffuse input/output/absorption/directSource'
DEFAULT_ORACLE = pathlib.Path(__file__).resolve().parent.parent / 'angular_transport_reference.py'
AXES = {'x': 2, 'y': 1, 'z': 0}


def sha(path):
    return hashlib.sha256(pathlib.Path(path).read_bytes()).hexdigest()


def load_oracle(path):
    spec = importlib.util.spec_from_file_location('_fogms_b3_cpu_oracle', path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def metric(actual, expected, floor=1.e-12):
    a, b = np.asarray(actual, dtype=np.float64), np.asarray(expected, dtype=np.float64)
    delta = a-b
    peak = max(float(np.max(np.abs(b))), floor)
    return {'relative_rms': float(np.linalg.norm(delta.ravel()) / max(np.linalg.norm(b.ravel()), floor)),
            'max_absolute': float(np.max(np.abs(delta))),
            'max_relative_to_reference_peak': float(np.max(np.abs(delta))/peak),
            'max_cell_relative': float(np.max(np.abs(delta)/np.maximum(np.abs(b), peak*1.e-7))),
            'actual_min': float(a.min()), 'actual_max': float(a.max()), 'reference_peak': peak}


def sl(axis, start, stop=None):
    index = [slice(None)]*3
    index[axis] = start if stop is None else slice(start, stop)
    return tuple(index)


def validate_dump(record, case, iterations):
    prefix = pathlib.Path(record['prefix'])
    meta_path, data_path = pathlib.Path(str(prefix)+'.json'), pathlib.Path(str(prefix)+'.rgba32f')
    meta = json.loads(meta_path.read_text(encoding='utf-8-sig'))
    if meta != record['metadata']:
        raise ValueError('Completion metadata changed after harness capture')
    if meta.get('success') is not True or meta.get('domain') != 'transport' or meta.get('format') != 'RGBA32F_LE' or meta.get('layout') != LAYOUT:
        raise ValueError('Wrong four-slab Transport schema')
    n = int(meta['grid'])
    expected_bytes = 4*n**3*4*4
    if n < 1 or meta['width'] != n or meta['height'] != 4*n*n or meta['bytes'] != expected_bytes or data_path.stat().st_size != expected_bytes:
        raise ValueError('Atlas dimensions/payload size disagree')
    if record.get('rgba_sha256') and sha(data_path) != record['rgba_sha256']:
        raise ValueError('Raw dump bytes changed after harness capture')
    for key, field in [('test', 'test'), ('tau', 'testTau'), ('albedo', 'testAlbedo')]:
        if not math.isclose(float(meta[field]), float(case[key]), rel_tol=2.e-6, abs_tol=1.e-7):
            raise ValueError('Requested case differs from metadata: '+key)
    for key in ('geometry', 'boundary'):
        field = 'test' + key.title()
        if field in meta and int(meta[field]) != int(case[key]):
            raise ValueError('Requested case differs from metadata: '+key)
    for phase in ('cvars', 'cvars_after'):
        for key in ('test', 'tau', 'albedo', 'boundary', 'geometry'):
            if not math.isclose(float(record[phase][key]), float(case[key]), rel_tol=2.e-6, abs_tol=1.e-7):
                raise ValueError('Live CVar evidence disagrees: '+phase+'/'+key)
    if int(meta['iterations']) != iterations or int(meta['directions']) != int(case['directions']) or meta.get('transport_scheme') != 'upwind_half_gauss':
        raise ValueError('Wrong iteration/direction count')
    age = (int(meta['dumpRenderFrame'])-int(meta['sourceProducedRenderFrame'])) & 0xffffffff
    if age > 2:
        raise ValueError('Stale producer frame')
    field = np.fromfile(data_path, dtype='<f4').astype(np.float64).reshape(4,n,n,n,4)
    if not np.isfinite(field).all():
        raise ValueError('Nonfinite value in one or more atlas slabs')
    return meta, field


def decode_faces(oracle, masks, case):
    integers = np.rint(masks).astype(np.int64)
    if np.any(masks != integers) or np.any(integers < 0) or np.any(integers > 4095+(288<<12)):
        raise ValueError('Face diagnostics must be exact nonnegative packed integers (low12 faces, high missing count)')
    # Bits0..5 topology,6..11 missing diffuse face,12+ missing angular exterior samples.
    integers &= 63
    closed_internal = 0
    for direction, (name, axis) in enumerate(AXES.items()):
        n = masks.shape[axis]
        low = ((integers >> (2*direction)) & 1).astype(bool)
        high = ((integers >> (2*direction+1)) & 1).astype(bool)
        if np.any(low[sl(axis,0)]) or np.any(high[sl(axis,n-1)]):
            raise ValueError('External faces must be closed')
        left, right = high[sl(axis,0,n-1)], low[sl(axis,1,n)]
        if not np.array_equal(left, right):
            raise ValueError('Adjacent open flags are not reciprocal on '+name)
        oracle.faces[name].open[sl(axis,1,n)] = left
        closed_internal += int(np.count_nonzero(~left))
    if not int(case['geometry']) and closed_internal:
        raise ValueError('TestGeometry0 should have no internal barriers')
    if int(case['boundary']) == 0:
        oracle.set_isotropic_boundary(1.)
    elif int(case['boundary']) == 1:
        oracle.faces['x'].plus[:,:,0] = 1.
    else:
        raise ValueError('Unknown boundary')
    return closed_internal


def oracle_flux_cells(model, solution, luminance):
    """FV face powers with the shader's working-space Luminance factors."""
    result = np.zeros(model.shape+(4,), dtype=np.float64)
    factors = np.asarray(luminance,dtype=float)
    for di,(direction,weight,rates) in enumerate(zip(model.directions,model.weights,model.rates)):
        angular = solution['outgoing'][di]
        for a,(name,axis) in enumerate(AXES.items()):
            rate = rates[a]
            if rate == 0:
                continue
            sign = 1 if direction[a]>0 else -1
            faces = model.faces[name]
            supplied = faces.plus if sign==1 else faces.minus
            n = model.shape[axis]
            factor = 4*math.pi*weight*model.volume*rate
            for index in range(n):
                cell = sl(axis,index)
                enter = sl(axis,index if sign==1 else index+1)
                leave = sl(axis,index+1 if sign==1 else index)
                result[cell+(0,)] += factor*(supplied[enter]@factors)*(~faces.open[enter])
                result[cell+(1,)] += factor*(angular[cell]@factors)*(~faces.open[leave])
    result[...,2] = 4*math.pi*model.volume*((model.sigma_a*solution['diffuse'])@factors)
    return result


def oracle_solution(model,args,case):
    """Reuse boundary sweep; furnace uses exact known J=1, not iterative CPU work."""
    boundary = model.boundary_field()
    model.boundary_field = lambda: boundary
    if case['geometry']==0 and case['boundary']==0 and (case['albedo']==1 or case['tau']==0):
        total = np.ones(model.shape+(3,))
        formal = model.sweep(model.sigma_s,boundary=True,flux=True)
        defect = np.max(np.abs(formal['J']-total))
        if defect>1.e-10:
            raise ArithmeticError('Analytic furnace does not satisfy CPU formal operator')
        return {'total':total,'diffuse':total,'source':model.sigma_s,'outgoing':formal['outgoing'],
                'iterations':0,'converged':True,'linear_relative_residual':np.zeros(3),
                'equation_relative_residual':np.full(3,defect),
                'energy':{'relative_balance_defect':np.zeros(3)},'solver':'analytic J=1 verified by formal sweep'}, boundary
    result = model.solve(max_iterations=args.cpu_iterations,tolerance=args.cpu_tolerance)
    result['solver'] = 'cold-start paired-ordinate FV PCG'
    return result,boundary


def expected_topology(n, extent, wall_x, thickness, shown):
    masks = np.zeros((n, n, n), dtype=np.int64)
    centers = (np.arange(n)+.5)*(2*extent[0]/n)-extent[0]
    cut = np.zeros(n-1, dtype=bool)
    if shown:
        lo, hi = wall_x-.5*thickness, wall_x+.5*thickness
        if np.any((centers >= lo-.01) & (centers <= hi+.01)):
            raise ValueError('A cell center lies inside/touches the wall; this bounded test excludes that ambiguity')
        cut = (centers[:-1] < hi) & (centers[1:] > lo)
    for index, (name, axis) in enumerate(AXES.items()):
        for plane in range(1, n):
            opened = name != 'x' or not cut[plane-1]
            if opened:
                masks[sl(axis,plane-1)] |= 1 << (2*index+1)
                masks[sl(axis,plane)] |= 1 << (2*index)
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


def analyze_case(point, iterations, oracle_module, args):
    case = point['case']
    row = {'name': case['name'], 'case': case, 'errors': [], 'checks': {}}
    def check(name, condition):
        row['checks'][name] = bool(condition)
        if not condition:
            row['errors'].append(name)
    try:
        barrier_meta, barrier = validate_dump(point['barrier'],case,iterations)
        meta, data = validate_dump(point['measurement'],case,iterations)
        n = int(meta['grid'])
        frames = (int(meta['dumpRenderFrame'])-int(barrier_meta['dumpRenderFrame'])) & 0xffffffff
        produced = (int(meta['sourceProducedRenderFrame'])-int(barrier_meta['sourceProducedRenderFrame'])) & 0xffffffff
        check('fresh_render_interval', 2 <= frames < 2**31 and 2 <= produced < 2**31)
        check('same_view', meta['viewKey'] == barrier_meta['viewKey'])
        row['render_frames_after_barrier'], row['produced_frames_after_barrier'] = frames, produced
        extent = np.asarray(point['box']['half_extent_cm'],dtype=np.float32).astype(np.float64)
        if extent.shape != (3,) or np.any(extent <= 0) or not np.isfinite(extent).all():
            raise ValueError('Invalid live Box half extent')
        spacing = (extent*2/n).astype(np.float32).astype(np.float64)
        coverage = {name: name in meta for name in ('testGeometry','testBoundary','cellSizeCm')}
        row['statusmetadataCoverage'] = {'status': 'FULL' if all(coverage.values()) else 'PARTIAL_RECEIPT_INPUTS',
                                        'fields': coverage,
                                        'fallback': 'Live CVars before/after both dumps and Box extent; no guessed fixture geometry.'}
        if 'cellSizeCm' in meta:
            check('metadata_cell_size', np.allclose(np.asarray(meta['cellSizeCm']),spacing,rtol=2.e-6,atol=1.e-5))
        row['grid'], row['cell_size_cm'] = n, spacing.tolist()
        total, primary, coefficients, flux = data
        if case['geometry']:
            row['geometry'] = geometry_check(point,data)
            check('independent_wall_topology',row['geometry']['actual_masks_match_expected'])
        packed_faces = np.rint(primary[...,3]).astype(np.int64)
        missing_flags = (packed_faces >> 6) & 63
        row['missing_surface_boundary_flags'] = int(sum(np.count_nonzero((missing_flags >> bit) & 1) for bit in range(6)))
        row['missing_angular_samples'] = int(np.sum(packed_faces>>12))
        check('no_missing_diagnostic_sources',row['missing_surface_boundary_flags']==0 and row['missing_angular_samples']==0)
        sigma_t, sigma_s = coefficients[...,3], coefficients[...,:3]
        check('nonnegative_coefficients', np.all(coefficients >= 0))
        if np.any(sigma_t < 0) or np.any(sigma_s < 0) or np.any(sigma_s > sigma_t[...,None]+1.e-12):
            raise ValueError('Invalid absorption/scattering coefficients')
        expected_sigma = np.full((n,n,n),np.float32(np.float32(case['tau'])/np.float32(2*extent[0])),dtype=np.float64)
        if int(case['test']) == 2:
            expected_sigma[:,:,n//4:3*n//4] = 0.
        requested_albedo = np.asarray(case['rgb_albedo'][:3] if case['albedo']==-1 else [case['albedo']]*3,dtype=np.float32)
        expected_s = (expected_sigma.astype(np.float32)[...,None]*requested_albedo).astype(np.float64)
        check('requested_sigma_t', np.allclose(sigma_t,expected_sigma,rtol=3.e-6,atol=1.e-11))
        check('requested_sigma_s', np.allclose(sigma_s,expected_s,rtol=3.e-6,atol=1.e-11))
        check('no_coefficient_drift', np.array_equal(barrier[2],coefficients))
        check('no_face_drift', np.array_equal(barrier[1,...,3],primary[...,3]))
        albedo = np.divide(sigma_s,sigma_t[...,None],out=np.zeros_like(sigma_s),where=sigma_t[...,None]>0)
        quadrature = 'gauss2x12' if case['directions']==48 else 'gauss3x16'
        model = oracle_module.AngularTransport(sigma_t,albedo,spacing,quadrature)
        row['internal_closed_faces'] = decode_faces(model,primary[...,3],case)
        begun = time.monotonic()
        reference, boundary_reference = oracle_solution(model,args,case)
        row['cpu'] = {'solver':reference['solver'],'iterations':reference['iterations'],'converged':reference['converged'],
                      'linear_relative_residual':reference['linear_relative_residual'].tolist(),
                      'equation_relative_residual':reference['equation_relative_residual'].tolist(),
                      'seconds':time.monotonic()-begun}
        check('cpu_reference_converged',reference['converged'])
        ref_j = reference['total']
        row['total_radiance'] = metric(total[...,:3],ref_j)
        row['primary_radiance'] = metric(primary[...,:3],boundary_reference)
        check('total_max_error',row['total_radiance']['max_relative_to_reference_peak'] <= args.error_threshold)
        check('total_rms_error',row['total_radiance']['relative_rms'] <= args.error_threshold)
        check('primary_matches_oracle',row['primary_radiance']['max_relative_to_reference_peak'] <= 3.e-5)
        scale = max(float(np.max(np.abs(ref_j))),1.e-12)
        row['minimum_radiance'] = float(total[...,:3].min())
        check('positive_radiance',row['minimum_radiance'] >= -args.negative_tolerance*scale)
        check('positive_primary',float(primary[...,:3].min()) >= -args.negative_tolerance*scale)
        check('nonnegative_residual',np.all(total[...,3]>=0))
        row['max_cell_residual'] = float(total[...,3].max())
        check('gpu_reported_residual',row['max_cell_residual'] <= args.residual_threshold)
        equation_r = total[...,:3]-boundary_reference-model.apply(model.sigma_s*total[...,:3])
        row['recomputed_equation_residual'] = float(np.max(np.abs(equation_r))/scale)
        check('recomputed_equation_residual',row['recomputed_equation_residual'] <= args.residual_threshold)
        if case['albedo']!=-1:
            check('grayscale_diagnostic',np.max(np.ptp(total[...,:3],axis=-1)) <= 1.e-6*scale)
        else:
            check('rgb_response_nontrivial',float(np.max(np.ptp(total[...,:3],axis=-1)))>.01*scale)
        expected_flux = oracle_flux_cells(model,reference,args.luminance_factors)
        row['flux_fields'] = {}
        sums = np.sum(flux,axis=(0,1,2))
        expected_sums = np.sum(expected_flux,axis=(0,1,2))
        incident = max(abs(float(expected_sums[0]+expected_sums[3])),1.e-20)
        for c,name in enumerate(('incoming','outgoing','absorbed','direct_source')):
            m = metric(flux[...,c],expected_flux[...,c])
            m.update(actual_sum=float(sums[c]),cpu_sum=float(expected_sums[c]),
                     sum_error_relative_to_incoming=float(abs(sums[c]-expected_sums[c])/incident))
            row['flux_fields'][name] = m
            check('flux_'+name+'_sum',m['sum_error_relative_to_incoming'] <= args.flux_threshold)
            if np.any(expected_flux[...,c]):
                check('flux_'+name+'_cell_max',m['max_relative_to_reference_peak'] <= args.error_threshold)
            else:
                check('flux_'+name+'_zero',np.max(np.abs(flux[...,c])) <= 1.e-10)
        denominator = max(abs(float(sums[0]+sums[3])),1.e-20)
        defect = float((sums[1]+sums[2]-sums[0]-sums[3])/denominator)
        row['relative_flux_defect'] = defect
        check('flux_balance',abs(defect) <= args.flux_threshold)
        row['cpu_relative_flux_defect'] = reference['energy']['relative_balance_defect'].tolist()
        check('nonnegative_flux',np.all(flux>=-1.e-10))
        if meta.get('finite') is not True:
            check('native_finite_flag',False)
        for name,actual in [('diffuseIncoming',sums[0]),('diffuseOutgoing',sums[1]),('diffuseAbsorbed',sums[2]),
                            ('directScatteringSource',sums[3]),('relativeFluxDefect',defect),
                            ('maxRelativeCellResidual',row['max_cell_residual']),('minRadiance',row['minimum_radiance'])]:
            check('metadata_'+name,math.isclose(float(meta[name]),float(actual),rel_tol=2.e-6,abs_tol=1.e-9))
        if case['boundary']==0 and case['geometry']==0 and (case['albedo']==1 or case['tau']==0):
            row['white_furnace_max_abs_error'] = float(np.max(np.abs(total[...,:3]-1)))
            row['white_furnace_rms_error'] = float(np.sqrt(np.mean((total[...,:3]-1)**2)))
            check('analytic_white_furnace',row['white_furnace_max_abs_error'] <= args.error_threshold)
        if case['albedo']==0:
            row['pure_absorption_max_added_radiance'] = float(np.max(np.abs(total[...,:3]-primary[...,:3])))
            check('no_scattering_when_albedo_zero',row['pure_absorption_max_added_radiance'] <= 3.e-6*scale)
        if case['test']==2:
            gap=slice(n//4,3*n//4)
            row['vacuum_gap_cm'] = float((3*n//4-n//4)*spacing[0])
            row['gap_radiance'] = metric(total[:,:,gap,:3],ref_j[:,:,gap,:])
            check('gap_has_zero_extinction',np.all(sigma_t[:,:,gap]==0))
            check('gap_reference_error',row['gap_radiance']['max_relative_to_reference_peak'] <= args.error_threshold)
        row['rgba_sha256'] = sha(point['measurement']['prefix']+'.rgba32f')
    except Exception:
        row['errors'].append(traceback.format_exc())
    row['result'] = 'PASS' if not row['errors'] else 'FAIL'
    return row


def wall_pair(points,iterations):
    names = ('no-wall48-center','wall48-center')
    if not all(name in points for name in names):
        return {'status':'NOT_RUN','errors':[]}
    result = {'status':'FAIL','errors':[]}
    try:
        p0,p1 = [points[name] for name in names]
        _,d0 = validate_dump(p0['measurement'],p0['case'],iterations)
        _,d1 = validate_dump(p1['measurement'],p1['case'],iterations)
        n=d0.shape[1]; peak=float(np.max(d0[0,...,:3]))
        control=float(np.mean(d0[0,:,:,n//2:,:3])); leak=float(np.max(np.abs(d1[0,:,:,n//2:,:3])))
        result.update(no_wall_peak=peak,no_wall_behind_mean=control,behind_wall_max=leak,
                      relative_leakage=leak/max(peak,1.e-12))
        if control<=1.e-5:result['errors'].append('No-wall control is too dark')
        if result['relative_leakage']>1.e-6:result['errors'].append('Thin-wall light leakage')
        if not np.array_equal(d0[2],d1[2]):result['errors'].append('Wall pair coefficients differ')
        if not result['errors']:result['status']='PASS'
    except Exception:
        result['errors'].append(traceback.format_exc())
    return result


def analyze(receipt_path,args):
    saved=json.loads(receipt_path.read_text(encoding='utf-8-sig'))
    report={'schema':'fogms_b3_gpu_analysis_v1','receipt':str(receipt_path),'receipt_sha256':sha(receipt_path),
            'oracle':str(args.oracle),'oracle_sha256':sha(args.oracle),'errors':[],'cases':[],
            'thresholds':{'relative_max_and_rms':args.error_threshold,'flux_defect':args.flux_threshold,
                          'residual':args.residual_threshold,'relative_negative_tolerance':args.negative_tolerance},
            'limits':['Same paired-ordinate FV discrete operator only; this does not establish angular accuracy.',
                      'Capture elapsed time includes stalled readbacks, not GPU pass timing.']}
    if saved.get('schema')!='fogms_b3_gpu_probe_v1':
        report['errors'].append('Unknown probe receipt schema')
    if saved.get('complete_suite') is not True and not args.allow_partial:
        report['errors'].append('A full suite is required; use --allow-partial for an explicitly partial report')
    report['complete_suite'] = bool(saved.get('complete_suite'))
    report['luminance_factors'] = args.luminance_factors
    report['limits'].append('Scalar RGB flux comparison assumes these working-space luminance factors; source provenance must be checked against UE shader configuration.')
    if saved.get('status')!='COMPLETED':
        report['errors'].append('Probe is not COMPLETED: '+str(saved.get('status')))
    if not saved.get('restoration_ok') or not saved.get('restoration',{}).get('ok'):
        report['errors'].append('Fresh original state was not verifiably restored')
    if saved.get('errors'):
        report['errors'].append('Harness reported errors')
    if len(saved.get('cases',[]))!=len(saved.get('planned_cases',[])):
        report['errors'].append('Incomplete case coverage')
    oracle=load_oracle(args.oracle)
    for index,point in enumerate(saved.get('cases',[])):
        if index>=len(saved['planned_cases']) or point.get('case')!=saved['planned_cases'][index]:
            report['errors'].append('Captured cases differ from the declared plan')
        result=analyze_case(point,int(saved['iterations']),oracle,args)
        report['cases'].append(result)
        print(json.dumps({'case':result['name'],'result':result['result'],'errors':result['errors']},ensure_ascii=False),flush=True)
    if not report['cases']:
        report['errors'].append('No captured cases')
    points = {p['case']['name']:p for p in saved.get('cases',[])}
    report['wall_pair'] = wall_pair(points,int(saved['iterations']))
    if report['wall_pair']['status']=='FAIL':
        report['errors'].extend(report['wall_pair']['errors'])
    if report['complete_suite'] and report['wall_pair']['status']!='PASS':
        report['errors'].append('Full suite is missing the controlled thin-wall pair')
    report['result']='PASS' if not report['errors'] and all(c['result']=='PASS' for c in report['cases']) else 'FAIL'
    return report


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('receipt',type=pathlib.Path)
    parser.add_argument('--oracle',type=pathlib.Path,default=DEFAULT_ORACLE)
    parser.add_argument('--output',type=pathlib.Path)
    parser.add_argument('--cpu-iterations',type=int,default=64)
    parser.add_argument('--allow-partial',action='store_true')
    parser.add_argument('--luminance-factors',type=float,nargs=3,default=[.2126390059,.7151686788,.0721923154])
    parser.add_argument('--cpu-tolerance',type=float,default=1.e-8)
    parser.add_argument('--error-threshold',type=float,default=.002)
    parser.add_argument('--flux-threshold',type=float,default=.002)
    parser.add_argument('--residual-threshold',type=float,default=.002)
    parser.add_argument('--negative-tolerance',type=float,default=1.e-6)
    args=parser.parse_args()
    if any(not math.isfinite(v) or v<0 for v in args.luminance_factors) or not math.isclose(sum(args.luminance_factors),1,abs_tol=1.e-6):
        parser.error('Require finite nonnegative normalized luminance factors')
    for name in ('cpu_tolerance','error_threshold','flux_threshold','residual_threshold','negative_tolerance'):
        if not math.isfinite(getattr(args,name)) or getattr(args,name)<=0:
            parser.error('All thresholds must be finite and positive')
    if not 1<=args.cpu_iterations<=2048:
        parser.error('cpu-iterations must be 1..2048')
    output=args.output or args.receipt.with_name(args.receipt.stem+'-analysis.json')
    if output.resolve()==args.receipt.resolve():
        parser.error('Output must not overwrite the probe receipt')
    report=analyze(args.receipt,args)
    output.write_text(json.dumps(report,indent=2,ensure_ascii=False,allow_nan=False),encoding='utf-8')
    print(json.dumps({'result':report['result'],'cases':len(report['cases']),'output':str(output)},ensure_ascii=False))
    return 0 if report['result']=='PASS' else 2


if __name__=='__main__':
    sys.exit(main())
