"""Analytic contracts for the Box froxel estimator, not a substitute for GPU QA."""
import itertools
import math
import unittest

# Positive tetrahedral rule: degree two exact, mixed cubic xyz deliberately not
# exact. Keep this order/sign pattern identical to FogMS_BoxQuadraturePosition.
SIGNS = ((1,1,1),(1,-1,-1),(-1,1,-1),(-1,-1,1))
POINTS = tuple(tuple(.5+s/math.sqrt(12) for s in signs) for signs in SIGNS)

def average(f):
    return sum(f(*p) for p in POINTS) / len(POINTS)

def alpha(current, history, motion, native=.9):
    density = abs(current[3]-history[3])/max(current[3],history[3],1e-6)
    light = max(abs(c-h) for c,h in zip(current[:3],history[:3]))/max(*current[:3],*history[:3],1e-6)
    return native * 2**(-2*motion-8*max(max(density,light)-.02,0))

class ReconstructionContract(unittest.TestCase):
    def test_degree_two_exactness_and_mixed_cubic_limit(self):
        self.assertEqual(len(POINTS),4)
        for a,b,c in itertools.product(range(3), repeat=3):
            if a+b+c <= 2:
                self.assertAlmostEqual(average(lambda x,y,z: x**a*y**b*z**c),1/((a+1)*(b+1)*(c+1)),places=14)
        # The eight-point tensor rule integrated this term exactly. Four points
        # sacrifice that property; record the bias instead of claiming parity.
        self.assertAlmostEqual(average(lambda x,y,z:x*y*z)-1/8,1/(12*math.sqrt(12)))

    def test_correlated_medium_and_light(self):
        # sigma=x, J=x: integral(sigma*J)=1/3, not product of averages=1/4.
        self.assertAlmostEqual(average(lambda x,y,z:x*x),1/3)
        self.assertGreater(average(lambda x,y,z:x*x)-average(lambda x,y,z:x)**2,.08)
        # Arbitrary linear sigma and J, including all mixed quadratic terms.
        sigma=(2.,3.,4.); light=(3.,5.,7.)
        expected=2.+sum(light)*.5+2.*sum(sigma)*.5
        expected+=sum(sigma[i]*light[j]*(1/3 if i==j else 1/4) for i in range(3) for j in range(3))
        self.assertAlmostEqual(average(lambda x,y,z:(1+2*x+3*y+4*z)*(2+3*x+5*y+7*z)),expected)

    def test_furnace_zero_absorption_no_gain(self):
        sigma=lambda x,y,z: .4+x+y*y+z*z*z
        j=7.5
        self.assertAlmostEqual(average(lambda x,y,z:sigma(x,y,z)*j)/average(sigma),j)

    def test_nonnegative_convex_source(self):
        for density in (0.,.001,1.,10.):
            actual=average(lambda x,y,z:density*x*max(3*y-z,0))
            self.assertGreaterEqual(actual,0)
            self.assertLessEqual(actual,3*average(lambda x,y,z:density*x))

    def test_history_unchanged_at_rest(self):
        self.assertEqual(alpha((1,2,3,.2),(1,2,3,.2),0),.9)

    def test_camera_response(self):
        self.assertAlmostEqual(alpha((1,2,3,.2),(1,2,3,.2),1),.225)
        self.assertLess(alpha((1,2,3,.2),(1,2,3,.2),2),.057)

    def test_light_change_without_density_change(self):
        self.assertLess(alpha((0,0,0,.2),(1,1,1,.2),0),.005)
        self.assertLess(alpha((1,0,0,.2),(0,1,0,.2),0),.005)

    def test_exposure_conversion_invariance(self):
        current=(1,2,3,.2); history=(.5,.3,.7,.3)
        expected=alpha(current,history,.4)
        for scale in (.01,1,100):
            self.assertAlmostEqual(alpha(tuple(v*scale for v in current[:3])+current[3:],
                                         tuple(v*scale for v in history[:3])+history[3:],.4),expected)

    def test_vacuum_and_removed_medium_finite(self):
        self.assertTrue(math.isfinite(alpha((0,0,0,0),(0,0,0,0),0)))
        self.assertLess(alpha((0,0,0,0),(1,1,1,.2),0),.005)

if __name__=='__main__':
    unittest.main(verbosity=2)
