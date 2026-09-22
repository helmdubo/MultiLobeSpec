"""CPU reference for mode-3 Box-entry-anchored coherent view integration.

Checks camera translation and log-Z repartition of the same physical ray against
constant-medium formulas and transfer-operator composition. Does not compile or
execute HLSL, import Unreal, or validate fp32/GPU tolerances. No temporal AA claim.
Run with --json <new-receipt.json> for an immutable evidence receipt.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass
from datetime import datetime, timezone
import json
import math
from pathlib import Path
import sys
import unittest


ZERO = (0.0, 0.0, 0.0)
EXTENT = (2.0, 3.0, 4.0)
METRICS = {}


def add(a, b):
    return tuple(x+y for x,y in zip(a,b))


def sub(a, b):
    return tuple(x-y for x,y in zip(a,b))


def scale(a, b):
    return tuple(x*b for x in a)


def dot(a, b):
    return sum(x*y for x,y in zip(a,b))


def length(a):
    return math.sqrt(dot(a, a))


def normalized(a):
    return scale(a, 1.0/length(a))


@dataclass(frozen=True)
class Transfer:
    l: tuple = ZERO
    t: float = 1.0

    def over(self, b):
        return Transfer(add(self.l, scale(b.l, self.t)), self.t*b.t)

    def faded(self, fade):
        return Transfer(scale(self.l, fade), 1.0-fade+fade*self.t)


def slab(sigma, source, distance):
    return Transfer(scale(source, -math.expm1(-sigma*distance)/max(sigma, 1e-5)),
                    math.exp(-sigma*distance))


def box_line(origin, direction, extent=EXTENT):
    entry, exit = -math.inf, math.inf
    for o,d,e in zip(origin,direction,extent):
        if d == 0.0:
            if abs(o) > e:
                return None
        else:
            lo,hi = sorted(((-e-o)/d, (e-o)/d))
            entry,exit = max(entry,lo),min(exit,hi)
    return (entry,exit) if exit > entry and math.isfinite(entry) and math.isfinite(exit) else None


def empty(point):
    return 0.0, ZERO  # sigma_t and unexposed sigma_s * J


@dataclass
class LastCellCache:
    """One ray in one dispatch: no native medium, length, fade or transfer."""
    index: int = -1
    sigma: float = 0.0
    pre_source: tuple = ZERO


def anchored_layer(origin, direction, a, b, native_sigma, native_pre_source, medium,
                   *, ratio=1.0, exposure=1.0, fade=1.0, steps=256, extent=EXTENT, cache=None):
    world_length = dot(sub(b,a), direction)
    native_length = world_length/ratio
    hit = box_line(origin,direction,extent)
    if hit is None:
        return slab(native_sigma,native_pre_source,native_length).faded(fade),0,False
    entry,exit = hit
    anchor = add(origin,scale(direction,entry))
    chord = exit-entry
    start,end = dot(sub(a,anchor),direction),dot(sub(b,anchor),direction)
    lo,hi = max(start,0.0),min(end,chord)
    if hi <= lo:
        return slab(native_sigma,native_pre_source,native_length).faded(fade),0,False
    result = slab(native_sigma,native_pre_source,(lo-start)/ratio)
    width = chord/steps
    first = min(steps-1,max(0,math.floor(lo/width)))
    last = min(steps-1,max(0,math.ceil(hi/width)-1))
    samples,has_box = 0,False
    for i in range(first,last+1):
        partial = min(hi,(i+1)*width)-max(lo,i*width)
        if partial <= 0.0:
            continue
        if cache is None or cache.index != i:
            point = add(anchor,scale(direction,(i+0.5)*width))
            sigma,q = medium(point)
            pre_source = scale(q,exposure)
            samples += 1
            if cache is not None:
                cache.index,cache.sigma,cache.pre_source = i,sigma,pre_source
        else:
            sigma,pre_source = cache.sigma,cache.pre_source
        has_box |= sigma > 0.0
        source = add(native_pre_source,scale(pre_source,ratio))
        result = result.over(slab(native_sigma+sigma*ratio,source,partial/ratio))
    result = result.over(slab(native_sigma,native_pre_source,(end-hi)/ratio))
    return result.faded(fade),samples,has_box


def column(origin, direction, points, native_sigma, native_pre_source, medium, *, cached=False, **kwargs):
    result = Transfer()
    samples,has_box = 0,False
    cache = LastCellCache() if cached else None
    for a,b in zip(points,points[1:]):
        segment,count,occupied = anchored_layer(origin,direction,a,b,native_sigma,native_pre_source,medium,cache=cache,**kwargs)
        result = result.over(segment)
        samples += count
        has_box |= occupied
    return result,samples,has_box


def zpoint(z,x=0.0,y=0.0):
    return (x,y,z)


def log_points(start,end,camera_z,count):
    assert start > camera_z
    lo,hi = math.log(start-camera_z),math.log(end-camera_z)
    values = [camera_z+math.exp(lo+(hi-lo)*i/count) for i in range(count+1)]
    values[0],values[-1] = start,end
    return [zpoint(z) for z in values]


def varied(point):
    x,y,z = point
    sigma = 0.08+(0.6 if math.sin(4.7*z+0.3*x-0.2*y) > 0.15 else 0.0)
    j = (0.7+0.2*math.sin(z),1.2+0.3*math.cos(0.7*z),0.9+0.2*math.sin(0.3*z))
    return sigma,tuple(sigma*a*v for a,v in zip((0.9,0.7,0.5),j))


class AnchoredViewIntegration(unittest.TestCase):
    def assert_transfer(self,a,b,tolerance=1e-11):
        for x,y in zip(a.l+(a.t,),b.l+(b.t,)):
            self.assertAlmostEqual(x,y,delta=tolerance*max(1.0,abs(y)))

    def test_world_sample_positions_stable_outside_and_inside(self):
        direction = normalized((0.2,0.1,1.0))
        points = []
        entries = []
        for shift in (-20.0,-6.0,-1.0,1.0):
            origin = scale(direction,shift)
            entry,exit = box_line(origin,direction)
            anchor = add(origin,scale(direction,entry))
            points.append([add(anchor,scale(direction,(i+0.5)*(exit-entry)/256)) for i in range(256)])
            entries.append(entry)
        for actual in points[1:]:
            self.assertLess(max(length(sub(a,b)) for a,b in zip(actual,points[0])),1e-13)
        self.assertGreater(entries[0],0.0)
        self.assertLess(entries[-1],0.0)
        METRICS['unclamped_entry'] = {'entry_distances':entries,'max_sample_shift':max(length(sub(a,b)) for a,b in zip(points[-1],points[0]))}

    def test_same_world_interval_survives_shifted_log_z_and_camera_translation(self):
        start,end = 0.2,5.0  # Common visible interval, including an outside tail.
        direction = (0.0,0.0,1.0)
        native_sigma,native_q = 0.03,(0.02,0.03,0.05)
        reference,_,_ = column(zpoint(-20.0),direction,[zpoint(start),zpoint(end)],native_sigma,native_q,varied)
        errors = []
        for camera in (-20.0,-6.0,-1.0,0.0):
            for count in (3,17,64,129):
                actual,_,_ = column(zpoint(camera),direction,log_points(start,end,camera,count),native_sigma,native_q,varied)
                self.assert_transfer(actual,reference)
                errors.append(max(abs(a-b) for a,b in zip(actual.l+(actual.t,),reference.l+(reference.t,))))
        METRICS['camera_log_z_invariance'] = {'cases':len(errors),'max_error':max(errors),'fade':1.0,'native_medium':'constant'}

    def test_partial_cell_camera_inside_keeps_original_midpoint(self):
        direction = (0.0,0.0,1.0)
        a,b = zpoint(0.113),zpoint(2.741)
        outside,_,_ = anchored_layer(zpoint(-20.0),direction,a,b,0.0,ZERO,varied)
        inside,_,_ = anchored_layer(zpoint(0.1),direction,a,b,0.0,ZERO,varied)
        self.assert_transfer(inside,outside)
        # Clamping the negative entry would move the grid origin to the camera.
        entry,_ = box_line(zpoint(0.1),direction)
        self.assertAlmostEqual(entry,-4.1)

    def test_constant_mixed_media_matches_piecewise_analytic_slabs(self):
        exposure,ratio = 3.0,1.4
        ns,nq,bs,bq = 0.2,(0.3,0.4,0.5),0.7,(0.5,0.2,0.8)
        medium = lambda p:(bs,bq)
        actual,_,_ = column(zpoint(-10.0),(0.0,0.0,1.0),[zpoint(z) for z in (-6.0,-3.7,-0.4,0.3,3.2,6.0)],
                            ns,nq,medium,ratio=ratio,exposure=exposure)
        outside = slab(ns,nq,2.0/ratio)
        inside = slab(ns+bs*ratio,add(nq,scale(bq,exposure*ratio)),8.0/ratio)
        self.assert_transfer(actual,outside.over(inside).over(outside))

    def test_zero_box_native_parity_including_denominator_floor(self):
        errors=[]
        for sigma in (0.0,1e-8,5e-6,0.3):
            native_q=(0.1,0.2,0.4)
            result,_,has_box = anchored_layer(zpoint(-10.0),(0.0,0.0,1.0),zpoint(-6.0),zpoint(6.0),sigma,native_q,empty,ratio=1.3,fade=0.37)
            expected=slab(sigma,native_q,12.0/1.3).faded(0.37)
            self.assert_transfer(result,expected)
            self.assertFalse(has_box)
            errors.append(max(abs(a-b) for a,b in zip(result.l+(result.t,),expected.l+(expected.t,))))
        METRICS['zero_box']={'max_error':max(errors),'native_denominator_floor':1e-5}

    def test_parallel_miss_and_outside_layers_are_native_only(self):
        native=slab(0.3,(0.2,0.4,0.8),2.0)
        result,count,occupied=anchored_layer((3.0,0.0,-10.0),(0.0,0.0,1.0),(3.0,0.0,-1.0),(3.0,0.0,1.0),0.3,(0.2,0.4,0.8),varied)
        self.assert_transfer(result,native)
        self.assertEqual(count,0)
        self.assertFalse(occupied)
        result,count,occupied=anchored_layer(zpoint(-10.0),(0.0,0.0,1.0),zpoint(-8.0),zpoint(-6.0),0.3,(0.2,0.4,0.8),varied)
        self.assert_transfer(result,native)
        self.assertEqual(count,0)

    def test_fade_once_after_all_anchored_cells(self):
        args=(zpoint(-10.0),(0.0,0.0,1.0),zpoint(-3.0),zpoint(3.0),0.1,(0.2,0.3,0.4),varied)
        unfaded,_,_=anchored_layer(*args)
        faded,_,_=anchored_layer(*args,fade=0.31)
        self.assert_transfer(faded,unfaded.faded(0.31))
        # Deliberately differs from applying native near-fade to 256 cells.
        self.assertAlmostEqual(faded.t,0.69+0.31*unfaded.t)

    def test_preexposure_only_rgb(self):
        ns,nq=0.1,(0.2,0.3,0.4)
        args=(zpoint(-10.0),(0.0,0.0,1.0),[zpoint(-6.0),zpoint(6.0)])
        baseline,_,_=column(*args,ns,nq,varied)
        for exposure in (0.001,0.5,128.0):
            actual,_,_=column(*args,ns,scale(nq,exposure),varied,exposure=exposure)
            self.assert_transfer(actual,Transfer(scale(baseline.l,exposure),baseline.t))

    def test_four_rays_preserve_jensen_without_double_extinction(self):
        medium=lambda p:(16.0 if p[0]>0 else 0.0,ZERO)
        rays=[]
        for x,y in ((-1.0,-1.0),(-1.0,1.0),(1.0,-1.0),(1.0,1.0)):
            ray,_,_=column((x,y,-10.0),(0.0,0.0,1.0),[(x,y,z) for z in (0.0,0.071,0.17,0.9,1.0)],0.0,ZERO,medium)
            rays.append(ray)
        mean=sum(r.t for r in rays)/4.0
        self.assertAlmostEqual(mean,0.5*(1.0+math.exp(-16.0)),places=14)
        self.assertGreater(mean-math.exp(-8.0),0.49)
        self.assertAlmostEqual(rays[2].t,math.exp(-16.0),places=14)
        METRICS['jensen']={'averaged_t':mean,'wrong_mean_density_t':math.exp(-8.0)}

    def test_partition_cost_is_steps_plus_boundaries_not_product(self):
        points=[zpoint(-4.0+8.0*i/127) for i in range(128)]
        original,samples,occupied=column(zpoint(-10.0),(0.0,0.0,1.0),points,0.0,ZERO,varied)
        cached,cached_samples,cached_occupied=column(zpoint(-10.0),(0.0,0.0,1.0),points,0.0,ZERO,varied,cached=True)
        self.assert_transfer(cached,original,0.0)
        self.assertEqual(cached_occupied,occupied)
        self.assertGreaterEqual(samples,256)
        self.assertLessEqual(samples,256+126)
        self.assertEqual(cached_samples,256)
        METRICS['cost']={'intervals':256,'layers':127,'density_samples_per_ray':samples,
                          'four_ray_density_samples':4*samples,'cached_samples_per_ray':cached_samples,
                          'cached_four_ray_samples':4*cached_samples,'sample_reduction':1.0-cached_samples/samples,
                          'no_early_transmittance_cutoff':True}

    def test_cache_keeps_partial_cell_points_for_outside_and_inside_cameras(self):
        cases=0
        for camera in (-20.0,0.1):
            for start,end in ((-6.0,6.0),(0.113,2.741),(-9.0,-7.0),(-4.0,-3.96),(3.96,4.02)):
                points=[zpoint(start+(end-start)*i/207) for i in range(208)]
                sampled=[]
                def recording_medium(p):
                    sampled.append(p)
                    return varied(p)
                args=(zpoint(camera),(0.0,0.0,1.0),points,0.021,(0.03,0.02,0.05),recording_medium)
                original,original_count,occupied=column(*args,ratio=1.4,exposure=3.0)
                uncached_points=list(sampled)
                sampled.clear()
                cached,count,cached_occupied=column(*args,ratio=1.4,exposure=3.0,cached=True)
                self.assert_transfer(cached,original,0.0)
                self.assertEqual(cached_occupied,occupied)
                self.assertLessEqual(count,original_count)
                # Removing consecutive repeated cell samples must be the only
                # change; neither partial-cell boundaries nor camera move them.
                unique_points=[p for i,p in enumerate(uncached_points) if i==0 or p!=uncached_points[i-1]]
                self.assertEqual(sampled,unique_points)
                cases+=1
        METRICS['cache_partial_intervals']={'cases':cases,'exact_transfer_and_sample_sequence':True}

    def test_cache_never_keeps_native_ratio_or_fade(self):
        points=[zpoint(-5.0)]+[zpoint(-3.987+7.974*i/193) for i in range(194)]+[zpoint(5.0)]
        cache=LastCellCache()
        original,cached=Transfer(),Transfer()
        original_count,cached_count=0,0
        for i,(a,b) in enumerate(zip(points,points[1:])):
            native_sigma=(0.0,1e-8,0.2,0.7)[i%4]
            native_q=(0.1+0.03*(i%5),0.7,0.03*(i%7))
            opts={'ratio':0.8+0.17*(i%7),'exposure':2.5,'fade':0.12+0.08*(i%11)}
            args=(zpoint(-10.0),(0.0,0.0,1.0),a,b,native_sigma,native_q,varied)
            segment,n,occupied=anchored_layer(*args,**opts)
            reused,nc,occupied_cached=anchored_layer(*args,cache=cache,**opts)
            self.assert_transfer(reused,segment,0.0)
            self.assertEqual(occupied_cached,occupied)
            original,cached=original.over(segment),cached.over(reused)
            original_count+=n
            cached_count+=nc
        self.assert_transfer(cached,original,0.0)
        self.assertEqual(cached_count,256)
        self.assertLess(cached_count,original_count)
        METRICS['cache_layer_dependent_inputs']={'layers':len(points)-1,'original_samples':original_count,
            'cached_samples':cached_count,'native_ratio_and_fade_change_each_layer':True,'exact_transfer':True}

    def test_cache_includes_empty_cells_and_preserves_native_floor(self):
        points=[zpoint(-4.0+8.0*i/127) for i in range(128)]
        for sigma in (0.0,1e-8,5e-6,0.3):
            args=(zpoint(-10.0),(0.0,0.0,1.0),points,sigma,(0.1,0.3,0.6),empty)
            original,_,occupied=column(*args,ratio=1.3,fade=0.37)
            cached,count,cached_occupied=column(*args,ratio=1.3,fade=0.37,cached=True)
            self.assert_transfer(cached,original,0.0)
            self.assertFalse(occupied or cached_occupied)
            self.assertEqual(count,256)

    def test_four_ray_caches_are_independent_and_preserve_jensen(self):
        def medium(p):
            return (0.0,ZERO) if p[0]<0.0 else (16.0,(8.0,3.0,1.0))
        xy=((-1.0,-1.0),(-1.0,1.0),(1.0,-1.0),(1.0,1.0))
        z=[i/91 for i in range(92)]
        caches=[LastCellCache() for _ in xy]
        totals=[Transfer() for _ in xy]
        sample_counts=[0 for _ in xy]
        for a,b in zip(z,z[1:]):
            # Interleave rays by native layer, as in one GPU column invocation.
            for r,(x,y) in enumerate(xy):
                seg,count,_=anchored_layer((x,y,-10.0),(0.0,0.0,1.0),(x,y,a),(x,y,b),
                    0.0,ZERO,medium,cache=caches[r])
                totals[r]=totals[r].over(seg)
                sample_counts[r]+=count
        for r,(x,y) in enumerate(xy):
            expected,_,_=column((x,y,-10.0),(0.0,0.0,1.0),[(x,y,p) for p in z],0.0,ZERO,medium)
            self.assert_transfer(totals[r],expected,0.0)
            self.assertEqual(sample_counts[r],32)
        self.assertAlmostEqual(sum(t.t for t in totals)/4,0.5*(1+math.exp(-16.0)),places=14)
        METRICS['independent_ray_caches']={'cached_samples_per_ray':sample_counts,'exact_transfer':True}

    def test_cache_lifetime_is_one_dispatch_not_across_preexposure_or_density_edits(self):
        points=[zpoint(-4.0+8.0*i/127) for i in range(128)]
        baseline=None
        for exposure in (0.001,1.0,128.0):
            args=(zpoint(-10.0),(0.0,0.0,1.0),points,0.1,(0.2*exposure,0.3*exposure,0.4*exposure),varied)
            cached,count,_=column(*args,exposure=exposure,cached=True)
            original,_,_=column(*args,exposure=exposure)
            self.assert_transfer(cached,original,0.0)
            self.assertEqual(count,256)
            normalized=Transfer(scale(cached.l,1.0/exposure),cached.t)
            if baseline is None:
                baseline=normalized
            self.assert_transfer(normalized,baseline)
        # A new dispatch must sample even the same first cell after density edit.
        args=(zpoint(-10.0),(0.0,0.0,1.0),[zpoint(0.0),zpoint(0.001)],0.0,ZERO)
        a,na,_=column(*args,lambda p:(1.0,(0.5,0.3,0.1)),cached=True)
        b,nb,_=column(*args,lambda p:(2.0,(1.0,0.6,0.2)),cached=True)
        self.assertEqual((na,nb),(1,1))
        self.assertNotEqual(a.t,b.t)

    def test_optical_depth_converges_with_fixed_world_partition_refinement(self):
        medium=lambda p:(0.2+0.03*p[2]*p[2],ZERO)
        expected=math.exp(-(0.2*8.0+0.03*128.0/3.0))
        errors=[]
        for steps in (32,64,128,256,512):
            actual,_,_=anchored_layer(zpoint(-10.0),(0.0,0.0,1.0),zpoint(-4.0),zpoint(4.0),0.0,ZERO,medium,steps=steps)
            errors.append(abs(actual.t-expected))
        self.assertTrue(all(b<a for a,b in zip(errors,errors[1:])))
        self.assertLess(errors[-1],errors[0]/200)
        METRICS['partition_convergence']={'steps':[32,64,128,256,512],'t_errors':errors,'analytic_t':expected}


class ReceiptResult(unittest.TextTestResult):
    def __init__(self,*args,**kwargs):
        super().__init__(*args,**kwargs)
        self.checks=[]

    def addSuccess(self,test):
        super().addSuccess(test)
        self.checks.append({'name':test.id(),'status':'PASS'})

    def addFailure(self,test,err):
        super().addFailure(test,err)
        self.checks.append({'name':test.id(),'status':'FAIL','detail':self._exc_info_to_string(err,test)})

    def addError(self,test,err):
        super().addError(test,err)
        self.checks.append({'name':test.id(),'status':'ERROR','detail':self._exc_info_to_string(err,test)})


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--json',type=Path,required=True)
    args=parser.parse_args()
    if args.json.exists():
        parser.error('Refusing to overwrite existing evidence: '+str(args.json))
    result=unittest.TextTestRunner(verbosity=2,resultclass=ReceiptResult).run(
        unittest.defaultTestLoader.loadTestsFromTestCase(AnchoredViewIntegration))
    receipt={'schema':'fogms-anchored-view-integration-cpu-v1','timestamp_utc':datetime.now(timezone.utc).isoformat(),
             'status':'PASS' if result.wasSuccessful() else 'FAIL','tests_run':result.testsRun,
             'checks':result.checks,'metrics':METRICS,
             'limitations':['Double-precision equation reference, not HLSL compile, fp32 validation, GPU timing or visual acceptance.',
                            'Invariance is for the same physical ray/interval with constant native medium and Fade=1; native near-fade and visibility distance remain camera dependent.',
                            'Fixed 256-cell ray partition is not temporal AA and does not resolve arbitrary thin features or the froxel XY pixel footprint.',
                            'Box transforms and ray-direction changes alter the physical chord and its quadrature; dynamic-density history is not added.',
                            'The coefficient cache is per ray and per dispatch. Its correctness requires fixed ray, Box field and pre-exposure throughout the column; no across-frame reuse is tested or implemented.',
                            'Mode3 preserves native center-ray measure, denominator floor, and one native Fade application per complete layer.']}
    args.json.parent.mkdir(parents=True,exist_ok=True)
    with args.json.open('x',encoding='utf-8') as out:
        json.dump(receipt,out,indent=2)
        out.write('\n')
    print('CPU receipt:',args.json.resolve())
    return 0 if result.wasSuccessful() else 1


if __name__=='__main__':
    sys.exit(main())
