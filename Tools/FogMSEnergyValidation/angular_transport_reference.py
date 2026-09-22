"""B3 angular finite-volume transport oracle (NumPy, CPU only).

Positive paired ordinate quadratures, conservative first-order upwind sweeps,
reciprocal Cartesian faces and symmetric sqrt(sigma_s) PCG. The companion
transport_reference.py supplies the unchanged B2 PCG/flux reporting machinery.
Angular integration error and upwind spatial bias are reported SEPARATELY.
Run with --output receipt.json; no UE process, files or configuration are changed.
"""
from __future__ import annotations

import argparse
import importlib.util
import itertools
import json
import math
from pathlib import Path
import sys
import time

import numpy as np

_spec = importlib.util.spec_from_file_location("fogms_b2_cpu_reference", Path(__file__).with_name("transport_reference.py"))
_b2 = importlib.util.module_from_spec(_spec)
sys.modules[_spec.name] = _b2
_spec.loader.exec_module(_b2)
FOUR_PI = 4 * math.pi


def cube_quadrature(n):
    """6*n*n unit midpoint ordinates; EXACT solid-angle weights of cube tiles.

    The direction is a representative point, not an exact integral of |mu|.
    Thus exact weights do NOT imply exact isotropic boundary irradiance.
    """
    if not isinstance(n, int) or n < 1:
        raise ValueError("Cube subdivision must be a positive integer")
    d, w = [], []
    f = lambda x, y: math.atan2(x*y, math.sqrt(1+x*x+y*y))
    for axis, sign, i, j in itertools.product(range(3), (-1, 1), range(n), range(n)):
        x0, x1 = -1+2*i/n, -1+2*(i+1)/n
        y0, y1 = -1+2*j/n, -1+2*(j+1)/n
        direction = np.zeros(3)
        direction[axis] = sign
        direction[(axis+1) % 3], direction[(axis+2) % 3] = (x0+x1)/2, (y0+y1)/2
        d.append(direction / np.linalg.norm(direction))
        w.append((f(x1,y1)-f(x0,y1)-f(x1,y0)+f(x0,y0))/FOUR_PI)
    return np.asarray(d), np.asarray(w)


def half_gauss_quadrature(polar, azimuth):
    """Product quadrature: separate Gauss-Legendre [0,1] in each hemisphere.

    Midpoint periodic azimuth, positive normalized weights. Exact |omega_z|
    first moment; other normals remain approximate. Even azimuth gives ± pairs.
    """
    if polar < 1 or azimuth < 4 or azimuth % 2:
        raise ValueError("Require polar >=1 and even azimuth >=4")
    mu, weights = np.polynomial.legendre.leggauss(polar)
    mu, weights = (mu+1)/2, weights/2
    d, w = [], []
    for sign, (m, a), k in itertools.product((-1, 1), zip(mu, weights), range(azimuth)):
        phi = 2*math.pi*(k+.5)/azimuth
        radius = math.sqrt(1-m*m)
        d.append([radius*math.cos(phi), radius*math.sin(phi), sign*m])
        w.append(a/(2*azimuth))
    return np.asarray(d), np.asarray(w)


def ordinate_set(name):
    if name.startswith("cube"):
        return cube_quadrature(int(name[4:]))
    if name.startswith("gauss"):
        p, a = map(int, name[5:].split("x"))
        return half_gauss_quadrature(p, a)
    raise ValueError(f"Unknown quadrature: {name}")


class AngularTransport(_b2.SixAxisTransport):
    """Conservative upwind FV on uniform (possibly anisotropic) Cartesian cells.

    q is radiance / length, J is ordinate-weighted radiance, weights sum to 1.
    All angular values leaving a cell equal its upwind FV cell value. Paired
    directions must share the SAME reciprocal open-face mask. Closed-face
    supplied radiance is diffuse/isotropic on each side, as in the B2 adapter.
    """
    def __init__(self, sigma_t, albedo=1., cell_size_cm=(100.,)*3, quadrature="gauss2x12"):
        super().__init__(sigma_t, albedo, cell_size_cm)
        self.directions, self.weights = ordinate_set(quadrature) if isinstance(quadrature, str) else quadrature
        self.directions = np.asarray(self.directions, dtype=float).copy()
        self.weights = np.asarray(self.weights, dtype=float).copy()
        if (self.directions.ndim != 2 or self.directions.shape[1] != 3 or
            len(self.weights) != len(self.directions) or np.any(self.weights <= 0) or
            not np.all(np.isfinite(self.directions)) or not np.all(np.isfinite(self.weights)) or
            not np.allclose(np.linalg.norm(self.directions, axis=1), 1, atol=1.e-13) or
            abs(self.weights.sum()-1) > 1.e-13):
            raise ValueError("Require positive normalized weights and unit directions")
        for d, w in zip(self.directions, self.weights):
            opposite = np.linalg.norm(self.directions+d, axis=1) < 1.e-12
            if np.count_nonzero(opposite) != 1 or abs(self.weights[opposite][0]-w) > 1.e-13:
                raise ValueError("PCG requires exactly matched ± ordinate weights")
        self.rates = np.abs(self.directions)/np.asarray(cell_size_cm)[None, :]
        self.rates[self.rates < 1.e-16] = 0.
        # Optional direction-resolved SKY/domain input, keyed by (direction,axis).
        # Full face shape + RGB or broadcastable RGB. Internal faces retain the
        # diffuse B2 supplied radiance even when this external override exists.
        self.external_incoming = {}
        self.lambda_diagonal = sum(w/(self.sigma_t+sum(r)) for w, r in zip(self.weights, self.rates))
        self.system_diagonal = 1-self.sigma_s*self.lambda_diagonal[..., None]
        xyz = np.indices(self.shape).reshape(3, -1)[::-1]
        self._xyz = xyz
        self._fronts = {}
        for d in self.directions:
            signs = tuple(np.sign(d).astype(int))
            if signs not in self._fronts:
                rank = np.zeros(self.sigma_t.size, dtype=int)
                for a in range(3):
                    if signs[a] != 0:
                        rank += xyz[a] if signs[a] > 0 else self.shape[2-a]-1-xyz[a]
                self._fronts[signs] = [np.flatnonzero(rank == k) for k in range(rank.max()+1)]

    def sweep(self, source, boundary=False, flux=False):
        q = self._rgb(source, "source").reshape(-1, 3)
        sigma = self.sigma_t.ravel()
        j = np.zeros_like(q)
        totals = {k: np.zeros(3) for k in ("domain_in", "domain_out", "wall_in", "wall_out")}
        outgoing = {}
        xyz = self._xyz
        strides = (1, self.shape[2], self.shape[1]*self.shape[2])
        for di, (direction, weight, rates) in enumerate(zip(self.directions, self.weights, self.rates)):
            signs = tuple(np.sign(direction).astype(int))
            incident = []
            denominator = sigma + sum(rates)
            for a, (axis, sign, rate) in enumerate(zip(("x", "y", "z"), signs, rates)):
                if rate == 0:
                    continue
                faces = self.faces[axis]
                enter = xyz.copy()
                enter[a] += int(sign < 0)
                leave = xyz.copy()
                leave[a] += int(sign > 0)
                enter = tuple(enter[::-1])
                leave = tuple(leave[::-1])
                opened = faces.open[enter]
                supplied = (faces.plus if sign > 0 else faces.minus)[enter]
                external_enter = xyz[a] == (0 if sign > 0 else self.shape[2-a]-1)
                if boundary and (di,axis) in self.external_incoming:
                    override = np.broadcast_to(np.asarray(self.external_incoming[(di,axis)],dtype=float),faces.plus.shape)[enter]
                    supplied = np.where(external_enter[:,None],override,supplied)
                if boundary and (not np.all(np.isfinite(supplied)) or np.any(supplied < 0)):
                    raise ValueError("Boundary radiance must be finite and nonnegative")
                upstream = np.arange(sigma.size)-sign*strides[a]
                # Never index out of domain: exterior faces MUST be closed.
                if np.any(opened & external_enter):
                    raise ValueError("External face cannot be open")
                upstream = np.clip(upstream, 0, sigma.size-1)
                incident.append((a, axis, rate, opened, supplied, upstream, enter, leave, external_enter))
            angular = np.zeros_like(q)
            for front in self._fronts[signs]:
                numerator = q[front].copy()
                for a, axis, rate, opened, supplied, upstream, enter, leave, external_enter in incident:
                    value = np.where(opened[front, None], angular[upstream[front]], supplied[front] if boundary else 0.)
                    numerator += rate*value
                angular[front] = numerator/denominator[front, None]
            j += weight*angular
            if flux:
                for a, axis, rate, opened, supplied, upstream, enter, leave, external_enter in incident:
                    faces = self.faces[axis]
                    factor = FOUR_PI*weight*self.volume*rate
                    boundary_in = supplied if boundary else np.zeros_like(supplied)
                    closed_in = ~opened
                    totals["domain_in"] += factor*np.sum(boundary_in[closed_in & external_enter], axis=0)
                    totals["wall_in"] += factor*np.sum(boundary_in[closed_in & ~external_enter], axis=0)
                    closed_out = ~faces.open[leave]
                    external_exit = xyz[a] == (self.shape[2-a]-1 if signs[a] > 0 else 0)
                    totals["domain_out"] += factor*np.sum(angular[closed_out & external_exit], axis=0)
                    totals["wall_out"] += factor*np.sum(angular[closed_out & ~external_exit], axis=0)
                outgoing[di] = angular.reshape(self.shape+(3,)).copy()
        return {"J": j.reshape(self.shape+(3,)), "flux": totals, "outgoing": outgoing}


def _slab_solve(mu, weights, n=64, tau=4., albedo=.9, method="exponential"):
    """Independent dense infinite plane slab; two incoming unit boundaries.

    Exponential method is exact formal cell-average B2 style. Upwind is 1D FV.
    Projection to slab normal removes transverse escape; NOT a small 3D box.
    """
    mu, weights = np.asarray(mu), np.asarray(weights)
    positive = mu > 1.e-12
    parallel = np.sum(weights[np.abs(mu) <= 1.e-12])
    ms, ws = mu[positive], weights[positive]
    index = np.arange(n)
    distance = np.abs(index[:, None]-index[None, :])
    kernel = np.eye(n)*parallel*albedo
    b = np.zeros(n)
    endpoints = []
    for m, w in zip(ms, ws):
        if method == "exponential":
            t, f, _ = _b2.beer_coefficients(tau/n/m)
            b += w*f*(t**index+t**(n-1-index))
            k = w*albedo*(1-t)*f*t**np.maximum(distance-1, 0)
            np.fill_diagonal(k, 2*w*albedo*(1-f))
        elif method == "upwind":
            t = 1/(1+tau/n/m)
            b += w*(t**(index+1)+t**(n-index))
            k = w*albedo*(1-t)*t**distance
            np.fill_diagonal(k, 2*w*albedo*(1-t))
        else:
            raise ValueError(method)
        kernel += k
        endpoints.append((m, w, t))
    j = np.linalg.solve(np.eye(n)-kernel, b)
    outgoing = 0.
    for m, w, t in endpoints:
        left = t**n+albedo*(1-t)*np.sum(j*t**index)
        right = t**n+albedo*(1-t)*np.sum(j*t**(n-1-index))
        outgoing += w*m*(left+right)
    incoming = 2*np.sum(ms*ws)
    absorbed = tau*(1-albedo)*j.mean()
    return j, {"center_J": float((j[n//2-1]+j[n//2])/2), "mean_J": float(j.mean()),
               "relative_flux_defect": float((outgoing+absorbed-incoming)/incoming),
               "incoming_over_4pi": float(incoming)}


def _relative(a, b):
    return float(np.linalg.norm(a-b)/max(np.linalg.norm(b), 1.e-30))


def angular_metrics(cells=(32, 64, 128)):
    names = ("cube1", "cube2", "cube3", "cube4", "gauss2x12", "gauss3x16")
    normals = {"x": [1,0,0], "y": [0,1,0], "z": [0,0,1],
               "xy": [1,1,0], "xyz": [1,1,1], "oblique": [1,2,3]}
    normals = {k: np.asarray(v, dtype=float)/np.linalg.norm(v) for k, v in normals.items()}
    gauss_mu, gauss_w = np.polynomial.legendre.leggauss(64)
    gauss_mu, gauss_w = (gauss_mu+1)/2, gauss_w/4
    gauss_mu, gauss_w = np.r_[gauss_mu, -gauss_mu], np.r_[gauss_w, gauss_w]
    records = {}
    for n in cells:
        reference, ref_meta = _slab_solve(gauss_mu, gauss_w, n=n)
        per_set = {}
        for name in names:
            directions, weights = ordinate_set(name)
            rotations = {}
            for label, normal in normals.items():
                mu = directions@normal
                exact, emeta = _slab_solve(mu, weights, n=n)
                fv, fmeta = _slab_solve(mu, weights, n=n, method="upwind")
                rotations[label] = {"isotropic_irradiance_relative_bias": float(2*np.sum(weights*np.abs(mu))-1),
                                    "angular_only_rms_vs_gauss": _relative(exact, reference),
                                    "fv_spatial_rms_vs_same_ordinates": _relative(fv, exact),
                                    "fv_total_rms_vs_gauss": _relative(fv, reference),
                                    "exponential": emeta, "upwind": fmeta}
            per_set[name] = {"directions": len(weights), "rotations": rotations,
                             "angular_worst_rms": max(r["angular_only_rms_vs_gauss"] for r in rotations.values()),
                             "fv_spatial_worst_rms": max(r["fv_spatial_rms_vs_same_ordinates"] for r in rotations.values()),
                             "fv_total_worst_rms": max(r["fv_total_rms_vs_gauss"] for r in rotations.values()),
                             "exact_center_rotation_range": max(r["exponential"]["center_J"] for r in rotations.values())-min(r["exponential"]["center_J"] for r in rotations.values())}
        records[str(n)] = {"reference": ref_meta, "sets": per_set}
    # Continuous angular reference convergence independent of proposed sets.
    fine = {}
    for order in (32, 64, 128):
        mu, w = np.polynomial.legendre.leggauss(order)
        mu, w = (mu+1)/2, w/4
        j, meta = _slab_solve(np.r_[mu,-mu], np.r_[w,w], n=max(cells))
        fine[str(2*order)] = meta
    # A denser direction survey detects errors hidden by axes and diagonals.
    # It is a deterministic SAMPLE, not a proof of a global angular maximum.
    survey_normals = list(normals.values())
    golden = math.pi*(3-math.sqrt(5))
    for k in range(96):
        z = (k+.5)/96
        r = math.sqrt(1-z*z)
        survey_normals.append(np.array([r*math.cos(k*golden),r*math.sin(k*golden),z]))
    survey_reference, _ = _slab_solve(gauss_mu,gauss_w,n=32)
    survey = {}
    for name in names:
        directions, weights = ordinate_set(name)
        measurements = []
        for normal in survey_normals:
            mu = directions@normal
            exact, _ = _slab_solve(mu,weights,n=32)
            fv, _ = _slab_solve(mu,weights,n=32,method="upwind")
            measurements.append({"normal":normal.tolist(),
                                 "angular_rms":_relative(exact,survey_reference),
                                 "fv_total_rms":_relative(fv,survey_reference),
                                 "face_flux_bias":float(2*np.sum(weights*np.abs(mu))-1)})
        survey[name] = {"samples":len(measurements),
                        "worst_angular":max(measurements,key=lambda r:r["angular_rms"]),
                        "worst_total":max(measurements,key=lambda r:r["fv_total_rms"]),
                        "worst_absolute_face_flux_bias":max(measurements,key=lambda r:abs(r["face_flux_bias"]))}
    return {"scope": "infinite homogeneous plane slab tau=4, albedo=.9, unit radiance at both faces; rotations change only angular projection",
            "spatial_method": "upwind FV has first-order spatial diffusion; exponential comparator isolates angular error",
            "grid_refinement": records, "gauss_reference_angular_refinement": fine,
            "rotation_survey_32_cells":survey}


def validate(quadrature="gauss2x12", cells=6, angular=True):
    start = time.perf_counter()
    checks, metrics = [], {}
    def check(name, passed, detail=None):
        checks.append({"name": name, "pass": bool(passed), "detail": detail})
    rng = np.random.default_rng(20260922)
    d, w = ordinate_set(quadrature)
    check("quadrature_positive_normalized", np.all(w > 0) and abs(w.sum()-1) < 2.e-14)
    check("quadrature_zero_first_moment", np.max(abs(w@d)) < 2.e-14)
    check("quadrature_unit_directions", np.max(abs(np.linalg.norm(d,axis=1)-1)) < 2.e-14)
    sigma = rng.uniform(.001,.014,(3,3,3)); sigma[1,1,1] = 0
    medium = AngularTransport(sigma, [.9,.8,.7], (50.,80.,110.), quadrature)
    medium.close_face("x", 1, [.3,.2,.5], [.1,.7,.2])
    medium.faces["x"].open[1,1,1] = True
    medium.set_isotropic_boundary([.7,.4,.3])
    count = sigma.size
    lam = np.zeros((count,count))
    for i in range(count):
        basis = np.zeros(sigma.shape); basis.flat[i] = 1
        lam[:,i] = medium.apply(basis)[...,0].ravel()
    symmetry = float(np.max(abs(lam-lam.T)))
    eigenvalues = np.linalg.eigvalsh(lam)
    check("dense_Lambda_symmetric", symmetry < 2.e-13, symmetry)
    check("dense_Lambda_positive_definite", eigenvalues.min() > 0, float(eigenvalues.min()))
    system_min = []
    for c in range(3):
        s = medium.sqrt_s[...,c].ravel()
        system_min.append(float(np.linalg.eigvalsh(np.eye(count)-s[:,None]*lam*s[None,:]).min()))
    check("dense_PCG_system_SPD", min(system_min) > 0, system_min)
    direct = rng.uniform(0,1,sigma.shape+(3,))
    b = direct+medium.boundary_field()
    oracle = np.empty_like(b)
    for c in range(3):
        oracle[...,c] = np.linalg.solve(np.eye(count)-lam*medium.sigma_s[...,c].ravel()[None,:], b[...,c].ravel()).reshape(sigma.shape)
    result = medium.solve(direct,max_iterations=64,tolerance=1.e-12)
    check("PCG_vs_dense_RGB", result["converged"] and _relative(result["total"],oracle) < 2.e-11, _relative(result["total"],oracle))
    check("direct_source_diffuse_flux_balance", max(abs(result["energy"]["relative_balance_defect"])) < 2.e-11,
          result["energy"]["relative_balance_defect"].tolist())
    check("Lambda_diagonal_matches_closed_form", max(abs(np.diag(lam)-medium.lambda_diagonal.ravel())) < 2.e-13)
    signed = rng.normal(size=sigma.shape+(3,))
    check("signed_matvec_odd_symmetry", np.array_equal(medium.apply(-signed),-medium.apply(signed)))
    check("zero_sources_zero_result", not np.any(medium.apply(0)))
    # Formal discrete flux identity also holds BEFORE PCG convergence.
    formal = medium.sweep(signed,boundary=True,flux=True)
    f = formal["flux"]
    defect = f["domain_out"]+f["wall_out"]-f["domain_in"]-f["wall_in"]+FOUR_PI*medium.volume*np.sum(medium.sigma_t[...,None]*formal["J"]-signed,axis=(0,1,2))
    check("signed_formal_cell_flux_identity", np.max(abs(defect)) < 2.e-7, defect.tolist())
    furnace = []
    for tau in (.1,4.,16.):
        obj = AngularTransport(np.full((cells,)*3,tau/(100*cells)),1.,quadrature=quadrature)
        obj.set_isotropic_boundary([1.,.7,2.])
        res = obj.solve(max_iterations=64,tolerance=1.e-11)
        error = float(np.max(abs(res["total"]/[1.,.7,2.]-1)))
        flux = float(np.max(abs(res["energy"]["relative_balance_defect"])))
        check(f"furnace_tau_{tau}",res["converged"] and error<2.e-9 and flux<2.e-9,{"error":error,"flux":flux,"iterations":res["iterations"]})
        furnace.append(_b2._summary(res))
    vacuum = AngularTransport(np.zeros((cells,)*3),0.,quadrature=quadrature)
    vacuum.set_isotropic_boundary([1.,.7,2.])
    vac = vacuum.solve()
    check("vacuum_constant_radiance",np.max(abs(vac["total"]-[1.,.7,2.]))<2.e-14)
    check("vacuum_flux_balance",np.max(abs(vac["energy"]["relative_balance_defect"]))<2.e-14)
    angular_boundary_expected = np.zeros(3)
    for di,(direction,weight) in enumerate(zip(vacuum.directions,vacuum.weights)):
        radiance = np.array([1.+.3*direction[2],.4+.2*direction[0],.7])
        angular_boundary_expected += weight*radiance
        for axis in ("x","y","z"):
            vacuum.external_incoming[(di,axis)] = radiance
    directional_vac = vacuum.solve()
    check("direction_resolved_vacuum_boundary",np.max(abs(directional_vac["total"]-angular_boundary_expected))<2.e-14)
    check("direction_resolved_boundary_flux",np.max(abs(directional_vac["energy"]["relative_balance_defect"]))<2.e-14)
    # 1D ordinates isolate transport through a 12m vacuum gap without side escape.
    # FV pure absorption is (1+sigma*h)^-1, NOT Beer per finite cell; refinement
    # must approach Beer rather than a loose tolerance silently blessing a bias.
    axial_pair = (np.array([[1.,0.,0.],[-1.,0.,0.]]),np.array([.5,.5]))
    gap_sigma = np.full((1,1,16),.005);gap_sigma[:,:,2:14] = 0.
    gap = AngularTransport(gap_sigma,0.,(100.,100.,100.),axial_pair)
    gap.faces["x"].plus[:,:,0] = 1.
    gap_res = gap.solve()
    expected_transmission = (1+.005*100)**-4
    check("vacuum_gap_1200cm_no_range_loss",np.max(abs(gap_res["outgoing"][0][:,:,-1]-expected_transmission))<2.e-14)
    pure_errors = []
    for n in (16,32,64,128):
        pure = AngularTransport(np.full((1,1,n),.004),0.,(1000/n,100.,100.),axial_pair)
        pure.faces["x"].plus[:,:,0] = 1.
        pure_res = pure.solve()
        endpoint = pure_res["outgoing"][0][0,0,-1,0]
        check(f"pure_absorption_discrete_upwind_{n}",abs(endpoint-(1+4/n)**-n)<2.e-14)
        pure_errors.append(float(abs(endpoint-math.exp(-4))))
    check("pure_absorption_refines_toward_Beer",all(b<a*.6 for a,b in zip(pure_errors,pure_errors[1:])),pure_errors)
    # A reciprocal full plane partitions the graph for ALL oblique directions.
    wall = AngularTransport(np.full((cells,)*3,.004),.95,quadrature=quadrature)
    wall.close_face("x",cells//2)
    wall.faces["x"].plus[:,:,0] = 1.
    res = wall.solve(max_iterations=64,tolerance=1.e-11)
    check("thin_black_wall_zero_leakage",not np.any(res["total"][:,:,cells//2:]))
    check("thin_black_wall_nonzero_control",float(np.max(res["total"][:,:,:cells//2])) > .05)
    check("wall_surface_sink_balance",res["converged"] and np.max(abs(res["energy"]["relative_balance_defect"]))<2.e-9)
    # Deliberately under-converged output must not claim balance or convergence.
    one = medium.solve(direct,max_iterations=1,tolerance=1.e-12)
    check("exhausted_budget_reported",not one["converged"])
    flux_closure = one["energy"]["balance_defect"]-one["energy"]["source_closure_defect"]
    check("finite_budget_defect_accounted",np.max(abs(flux_closure))<2.e-8,flux_closure.tolist())
    medium.set_isotropic_boundary([1.4,.8,.6]); medium.close_face("x",1,[.6,.4,1.],[.2,1.4,.4]);medium.faces["x"].open[1,1,1] = True
    doubled = medium.solve(2*direct,max_iterations=64,tolerance=1.e-12)
    check("all_sources_double_linearly",_relative(doubled["total"],2*result["total"])<2.e-11)
    metrics.update({"quadrature":quadrature,"directions":len(w),"furnace":furnace,"dense_result":_b2._summary(result),
                    "pure_absorption_Beer_absolute_endpoint_error":dict(zip(("16","32","64","128"),pure_errors))})
    if angular:
        metrics["accuracy_separate_from_conservation"] = angular_metrics()
        for n, record in metrics["accuracy_separate_from_conservation"]["grid_refinement"].items():
            for name, entry in record["sets"].items():
                for normal, r in entry["rotations"].items():
                    check(f"slab_discrete_balance_{n}_{name}_{normal}",max(abs(r[k]["relative_flux_defect"]) for k in ("exponential","upwind"))<2.e-11)
        # Angular accuracy remains a measurement, NOT a balance PASS.
    return {"scope":"CPU B3 finite-volume ordinate reference only; GPU and native receiver untested here",
            "verdict":"PASS" if all(c["pass"] for c in checks) else "FAIL", "checks":len(checks),
            "passed":sum(c["pass"] for c in checks),"results":checks,"metrics":metrics,
            "elapsed_seconds":time.perf_counter()-start}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--quadrature",default="gauss2x12",choices=("cube1","cube2","cube3","cube4","gauss2x12","gauss3x16"))
    parser.add_argument("--cells",type=int,default=6,choices=(4,6,8,16))
    parser.add_argument("--skip-angular",action="store_true")
    parser.add_argument("--output",type=Path)
    args = parser.parse_args()
    report = validate(args.quadrature,args.cells,not args.skip_angular)
    if args.output:
        args.output.write_text(json.dumps(report,indent=2),encoding="utf-8")
    print(json.dumps({"verdict":report["verdict"],"checks":report["checks"],"passed":report["passed"],
                      "failures":[r for r in report["results"] if not r["pass"]],
                      "elapsed_seconds":report["elapsed_seconds"],
                      "angular_32":report["metrics"].get("accuracy_separate_from_conservation",{}).get("grid_refinement",{}).get("32",{})},indent=2))
    raise SystemExit(0 if report["verdict"] == "PASS" else 1)
