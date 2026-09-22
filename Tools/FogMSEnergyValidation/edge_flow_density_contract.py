"""Independent bounded-detail density contracts, without Unreal or GPU calls.

Uses an explicitly synthetic, periodic 32^3 texture. This is not the project's
Perlin asset, a rendered quality test, or a proof of the native C++ phase clock.
For fixed base noise N, |detail perturbation| <= D. Therefore evolving detail
cannot change the full-core mask at N >= T + S/2 + D or the empty mask at
N < T - S/2 - D. The intervening *value band* can include interior structure.
"""
from __future__ import annotations

import argparse
import hashlib
import itertools
import json
from pathlib import Path
import unittest

import numpy as np


def perturbation(detail0, detail1, strength, second_octave):
    weight = .5 * second_octave
    return strength * ((2*detail0-1) + weight*(2*detail1-1)) / (1+weight)


def density_mask(base, detail0, detail1, threshold, softness, strength, second_octave):
    noise = base + perturbation(detail0, detail1, strength, second_octave)
    if softness == 0:
        return (noise >= threshold).astype(float)
    t = np.clip((noise-(threshold-.5*softness))/softness, 0, 1)
    return t*t*(3-2*t)


def synthetic_texture(n=32):
    # Bounded smooth periodic data, not a claim to reproduce Perlin noise.
    x,y,z = np.meshgrid(*[(np.arange(n)+.5)/n]*3, indexing='ij')
    tau = 2*np.pi
    return (.5 + .22*np.sin(tau*x)*np.cos(tau*y)
            + .18*np.cos(tau*z+.7)*np.sin(tau*y)
            + .10*np.sin(tau*(x+y+z)))


def sample_wrap(texture, uvw):
    """Mip-zero periodic trilinear sampling, eight nonnegative convex weights."""
    size = np.array(texture.shape)
    position = np.mod(uvw, 1)*size-.5
    base = np.floor(position).astype(np.int64)
    alpha = position-base
    result = np.zeros(len(position))
    for bits in itertools.product((0,1), repeat=3):
        bits = np.array(bits)
        cell = np.mod(base+bits,size)
        weight = np.prod(np.where(bits,alpha,1-alpha),axis=1)
        result += texture[tuple(cell.T)]*weight
    return result


def world_uv(points, center, axes, frequency, displacement, offset):
    # Match the center-relative construction algebra. Extent is deliberately
    # absent from world-aligned coordinates; it controls bounds/feather only.
    local = (points-center) @ axes.T
    world_offset = local @ axes
    phase = np.mod((center-displacement-offset)*frequency,1)
    return world_offset*frequency+phase


METRICS = {}


class EdgeFlowDensityContract(unittest.TestCase):
    def test_general_detail_is_bounded(self):
        rng = np.random.default_rng(290921)
        count = 10000
        strength = rng.uniform(0,1,count)
        second = rng.uniform(0,1,count)
        detail0, detail1 = rng.uniform(0,1,(2,count))
        delta = perturbation(detail0,detail1,strength,second)
        self.assertTrue(np.all(np.abs(delta) <= strength+1.e-15))
        for d0,d1 in itertools.product((0.,1.),repeat=2):
            self.assertTrue(np.all(np.abs(perturbation(d0,d1,strength,second)) <= strength+1.e-15))
        # Explicit float32 rounding bound, rather than exact-real arithmetic claims.
        v = perturbation(detail0.astype(np.float32),detail1.astype(np.float32),
                         strength.astype(np.float32),second.astype(np.float32))
        self.assertTrue(np.all(np.abs(v) <= strength+4*np.finfo(np.float32).eps))
        METRICS['random_detail_bound_samples'] = count

    def test_general_full_core_and_empty_plateaus(self):
        tested = 0
        for threshold,softness,strength,second in itertools.product(
                np.linspace(0,1,9), (0.,.1,.5,1.), (0.,.1,.5,1.), (0.,.5,1.)):
            lower = threshold-.5*softness-strength
            upper = threshold+.5*softness+strength
            base = np.r_[np.linspace(0,1,1025),np.clip([lower,upper],0,1)]
            # A strict empty inequality is essential for HLSL step at softness=0.
            empty = base < lower-1.e-14
            core = base >= upper+1.e-14
            for d0,d1 in itertools.product((0.,1.),repeat=2):
                mask = density_mask(base,d0,d1,threshold,softness,strength,second)
                self.assertTrue(np.all(mask[empty] == 0))
                self.assertTrue(np.all(mask[core] == 1))
                self.assertTrue(np.all((mask >= 0)&(mask <= 1)&np.isfinite(mask)))
            tested += 1
        # Exactly representable boundaries separately verify step's inclusive core.
        self.assertEqual(float(density_mask(np.array([.75]),0.,0.,.5,0.,.25,.5)[0]),1.)
        self.assertEqual(float(density_mask(np.array([.249]),1.,1.,.5,0.,.25,.5)[0]),0.)
        METRICS['general_parameter_combinations'] = tested
        METRICS['boundary_policy'] = 'real-arithmetic bounds; 1e-14 margin for sampled floating boundary predicates; float32 perturbation tolerance 4 epsilon'

    def test_authored_evolving_synthetic_texture_only_changes_band(self):
        rng = np.random.default_rng(210926)
        texture = synthetic_texture()
        points = rng.uniform(-20000,20000,(8192,3))
        frequency = 1/10000
        base = sample_wrap(texture,points*frequency)
        empty,core = base < .35-1.e-12,base >= .65+1.e-12
        band = ~(empty|core)
        self.assertGreater(int(empty.sum()),0)
        self.assertGreater(int(core.sum()),0)
        self.assertGreater(int(band.sum()),0)
        snapshots = []
        for time in (0.,.125,1.,3.,10.,30.,100.,300.):
            # Common wind is zero to isolate relative detail flow; base N stays
            # fixed. Octave two has half speed and twice the spatial frequency.
            d0=sample_wrap(texture,(points-np.array([0.,20*time,0.]))*(frequency*8)
                           +np.array([.173,.379,.613]))
            d1=sample_wrap(texture,(points-np.array([0.,0.,-10*time]))*(frequency*16)
                           +np.array([.731,.217,.419]))
            mask=density_mask(base,d0,d1,.5,.1,.1,.5)
            self.assertTrue(np.all(mask[empty] == 0))
            self.assertTrue(np.all(mask[core] == 1))
            # Existing feather multiplies the mask; it is not a detail-flow term.
            fade=np.clip(np.linspace(-.1,1.1,len(base)),0,1)
            extinction=.2*.01*mask*fade
            self.assertTrue(np.all(np.isfinite(extinction)&(extinction>=0)&(extinction<=.002)))
            snapshots.append(mask)
        variation=np.ptp(np.array(snapshots),axis=0)
        changed=variation>1.e-12
        self.assertTrue(np.all(~changed | band))
        self.assertGreater(int(changed.sum()),0)
        self.assertGreater(float(variation.max()),.05)
        METRICS['authored_synthetic']={
            'threshold':.5,'softness':.1,'detail_strength':.1,'second_octave':.5,
            'points':len(base),'times':8,'empty_points':int(empty.sum()),
            'core_points':int(core.sum()),'band_points':int(band.sum()),
            'changed_points':int(changed.sum()),'changed_outside_band':int((changed&~band).sum()),
            'maximum_mask_change':float(variation.max()),
            'base_noise_guaranteed_empty_below':.35,'base_noise_guaranteed_core_at_or_above':.65}

    def test_texture_offsets_wrap_and_interpolation_stays_bounded(self):
        rng=np.random.default_rng(71)
        texture=synthetic_texture()
        uv=rng.uniform(-4,4,(4096,3))
        a=sample_wrap(texture,uv)
        b=sample_wrap(texture,uv+rng.integers(-32,33,(4096,3)))
        error=float(np.max(np.abs(a-b)))
        self.assertLess(error,1.e-12)
        self.assertTrue(np.all((a>=0)&(a<=1)))
        seam=np.array([[-1.e-8,.2,.3],[1-1.e-8,.2,.3],[1+1.e-8,.2,.3],[1.e-8,.2,.3]])
        values=sample_wrap(texture,seam)
        self.assertAlmostEqual(values[0],values[1],places=14)
        self.assertAlmostEqual(values[2],values[3],places=14)
        METRICS['maximum_integer_wrap_error']=error

    def test_world_mapping_independent_of_box_resize(self):
        rng=np.random.default_rng(72)
        center=np.array([3100.,-1500.,2100.])
        points=center+rng.uniform(-2000,2000,(1024,3))
        offset=np.array([500.,-200.,800.])
        displacement=np.array([100.,300.,-20.])
        theta=.731
        axes=np.array([[np.cos(theta),np.sin(theta),0],[-np.sin(theta),np.cos(theta),0],[0,0,1]])
        texture=synthetic_texture()
        max_error=0.
        for frequency in (1/10000,8/10000,16/10000):
            reference=sample_wrap(texture,(points-displacement-offset)*frequency)
            baseline=None
            for extent in (np.array([5000,5000,5000]),np.array([12000,6000,9000]),np.array([50000]*3)):
                local=(points-center)@axes.T
                inside=np.min(extent-np.abs(local),axis=1)
                self.assertTrue(np.all(inside>250))  # Full feather in every compared box.
                value=sample_wrap(texture,world_uv(points,center,axes,frequency,displacement,offset))
                max_error=max(max_error,float(np.max(np.abs(reference-value))))
                if baseline is not None:
                    self.assertTrue(np.array_equal(value,baseline))
                baseline=value
        self.assertLess(max_error,1.e-12)
        METRICS['maximum_world_mapping_error']=max_error
        METRICS['resize_scope']='same world points in the common full-feather interior; bounds/feather changes outside that region remain intentional'


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--output',type=Path)
    args=parser.parse_args()
    result=unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(EdgeFlowDensityContract))
    root=Path(__file__).resolve().parents[2]
    shader=root/'Shaders/Private/FogMS_Indirect.ush'
    text=' '.join(shader.read_text().split())
    formula='Noise += Data.DetailStrength * ((2.0f * Detail0 - 1.0f) + Weight * (2.0f * Detail1 - 1.0f)) / (1.0f + Weight);'
    formula_matches=formula in text and 'float Weight = 0.5f * Data.DetailSecondOctave;' in text
    success=result.wasSuccessful() and formula_matches
    receipt={'status':'PASS' if success else 'FAIL','tests':result.testsRun,
             'failures':len(result.failures),'errors':len(result.errors),
             'actual_shader_formula_matches':formula_matches,
             'shader_sha256':hashlib.sha256(shader.read_bytes()).hexdigest(),
             'data_provenance':'synthetic periodic smooth 32^3 texture in [0,1]; not the project Perlin asset',
             'scope':'independent CPU density bounds and mapping; no Unreal, GPU or rendered acceptance',
             'limits':['Invariant is a base-noise value band, including possible interior features, not a geometric edge-only mask.',
                       'Large detail strength can leave no guaranteed full core or empty plateau in base noise [0,1].',
                       'Common wind changes base noise at fixed world points; the plateau guarantee isolates relative detail evolution for a fixed base sample.',
                       'Core means density mask one; Box feather can still reduce extinction.',
                       'No fluid mass conservation, phase-clock correctness, temporal-history quality or GPU parity is established.'],
             'metrics':METRICS}
    if args.output:
        args.output.parent.mkdir(parents=True,exist_ok=True)
        args.output.write_text(json.dumps(receipt,indent=2)+'\n',encoding='utf-8')
    print(json.dumps({'status':receipt['status'],'tests':receipt['tests'],'formula_matches':formula_matches}))
    return 0 if success else 1


if __name__=='__main__':
    raise SystemExit(main())
