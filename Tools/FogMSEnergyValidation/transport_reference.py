"""Six-ordinate, cell-average transport oracle; no UE or camera history.

Arrays are [z,y,x,RGB], sigma_t is [z,y,x] in 1/cm, cell_size_cm is (x,y,z).
The operator is conservative for its SIX-DIRECTION quadrature, not an accurate
angular integration of arbitrary 3D lighting. Run this file for analytic and
numerical checks. NumPy is the only dependency; no files are written by default.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass
import json
import math
from pathlib import Path
import time

import numpy as np


AXES = {"x": 2, "y": 1, "z": 0}
WEIGHT = 1.0 / 6.0
FOUR_PI = 4.0 * math.pi


def beer_coefficients(tau):
    """Return T, f=(1-T)/tau, g=(1-f)/tau, including the vacuum limit."""
    tau = np.asarray(tau, dtype=np.float64)
    if np.any(~np.isfinite(tau)) or np.any(tau < 0):
        raise ValueError("Optical depth must be finite and nonnegative")
    t = np.exp(-tau)
    small = tau < 1.0e-3
    safe = np.where(small, 1.0, tau)
    f = -np.expm1(-tau) / safe
    g = (1.0 - f) / safe
    f_series = 1 + tau * (-.5 + tau * (1/6 + tau * (-1/24 + tau/120)))
    g_series = .5 + tau * (-1/6 + tau * (1/24 + tau * (-1/120 + tau/720)))
    return t, np.where(small, f_series, f), np.where(small, g_series, g)


def _slice(array_axis, index):
    indices = [slice(None)] * 3
    indices[array_axis] = index
    return tuple(indices)


@dataclass
class Faces:
    """A single reciprocal open flag, independent incoming RGB on either side.

    At face index f, plus travels toward cell f; minus toward cell f-1.
    The corresponding value is used only when that face is closed. External
    faces are always closed; internal closed faces are imposed opaque boundaries.
    """
    open: np.ndarray
    plus: np.ndarray
    minus: np.ndarray


class SixAxisTransport:
    def __init__(self, sigma_t, albedo=1.0, cell_size_cm=(100., 100., 100.)):
        self.sigma_t = np.asarray(sigma_t, dtype=np.float64).copy()
        if self.sigma_t.ndim != 3 or min(self.sigma_t.shape) < 1:
            raise ValueError("sigma_t must be a nonempty [z,y,x] array")
        if not np.all(np.isfinite(self.sigma_t)) or np.any(self.sigma_t < 0):
            raise ValueError("sigma_t must be finite and nonnegative")
        self.shape = self.sigma_t.shape
        self.spacing = dict(zip(("x", "y", "z"), map(float, cell_size_cm)))
        if len(self.spacing) != 3 or not all(math.isfinite(h) and h > 0 for h in self.spacing.values()):
            raise ValueError("cell_size_cm must contain three finite positive sizes")
        a = np.asarray(albedo, dtype=np.float64)
        if a.shape == self.shape:
            a = a[..., None]
        self.albedo = np.broadcast_to(a, self.shape + (3,)).copy()
        if not np.all(np.isfinite(a)) or np.any(a < 0) or np.any(a > 1):
            raise ValueError("albedo must be finite in [0,1]")
        self.sigma_s = self.sigma_t[..., None] * self.albedo
        self.sigma_a = self.sigma_t[..., None] * (1 - self.albedo)
        self.sqrt_s = np.sqrt(self.sigma_s)
        self.volume = math.prod(self.spacing.values())
        self.faces = {}
        self.coefficients = {}
        self.lambda_diagonal = np.zeros(self.shape)
        mean_f = np.zeros(self.shape)
        for axis, array_axis in AXES.items():
            h = self.spacing[axis]
            t, f, g = beer_coefficients(self.sigma_t * h)
            self.coefficients[axis] = t, f, g
            self.lambda_diagonal += (h / 3) * g
            mean_f += f / 3
            face_shape = list(self.shape)
            face_shape[array_axis] += 1
            opened = np.ones(face_shape, dtype=bool)
            opened[_slice(array_axis, 0)] = False
            opened[_slice(array_axis, self.shape[array_axis])] = False
            self.faces[axis] = Faces(opened, np.zeros(tuple(face_shape) + (3,)),
                                     np.zeros(tuple(face_shape) + (3,)))
        # Algebraically 1 - sigma_s * Lambda_ii, without subtraction near 1.
        self.system_diagonal = 1 - self.albedo + self.albedo * mean_f[..., None]

    def _rgb(self, value, name="field"):
        field = np.asarray(value, dtype=np.float64)
        if field.shape == self.shape:
            field = field[..., None]
        field = np.broadcast_to(field, self.shape + (3,))
        if not np.all(np.isfinite(field)):
            raise ValueError(f"{name} must be finite")
        return field

    def close_face(self, axis, index, plus=0., minus=0.):
        f = self.faces[axis]
        s = _slice(AXES[axis], index)
        f.open[s] = False
        f.plus[s] = plus
        f.minus[s] = minus

    def set_isotropic_boundary(self, radiance, include_internal=False):
        """Set radiance into every domain side, optionally also closed walls."""
        value = np.asarray(radiance, dtype=np.float64)
        if not np.all(np.isfinite(value)) or np.any(value < 0):
            raise ValueError("Boundary radiance must be finite and nonnegative")
        for axis, array_axis in AXES.items():
            f = self.faces[axis]
            if include_internal:
                f.plus[~f.open] = value
                f.minus[~f.open] = value
            else:
                f.plus[_slice(array_axis, 0)] = value
                f.minus[_slice(array_axis, self.shape[array_axis])] = value

    def sweep(self, source, boundary=False, flux=False):
        """Formal solution of dI/ds + sigma_t I = source.

        Source is radiance/cm and MAY BE SIGNED (PCG matrix-vector products).
        Each cell's exact mean, not its endpoint, contributes to J. Open faces
        share exactly the same outgoing/incoming value, with no interpolation.
        """
        q = self._rgb(source, "source")
        j = np.zeros(self.shape + (3,))
        totals = {k: np.zeros(3) for k in ("domain_in", "domain_out", "wall_in", "wall_out")}
        outgoing = {}
        for axis, array_axis in AXES.items():
            n = self.shape[array_axis]
            h = self.spacing[axis]
            area = self.volume / h
            t, f, g = self.coefficients[axis]
            faces = self.faces[axis]
            if np.any(faces.open[_slice(array_axis, 0)]) or np.any(faces.open[_slice(array_axis, n)]):
                raise ValueError("External faces cannot be open")
            for sign in (1, -1):
                indices = range(n) if sign == 1 else range(n - 1, -1, -1)
                supplied = faces.plus if sign == 1 else faces.minus
                if boundary and (not np.all(np.isfinite(supplied)) or np.any(supplied < 0)):
                    raise ValueError("Boundary radiance must be finite and nonnegative")
                previous = np.zeros(q[_slice(array_axis, 0)].shape)
                face_out = np.zeros_like(supplied) if flux else None
                for index in indices:
                    s = _slice(array_axis, index)
                    enter_index = index if sign == 1 else index + 1
                    exit_index = index + 1 if sign == 1 else index
                    enter = _slice(array_axis, enter_index)
                    leave = _slice(array_axis, exit_index)
                    incident = np.where(faces.open[enter][..., None], previous,
                                        supplied[enter] if boundary else 0.)
                    j[s] += WEIGHT * (incident * f[s][..., None] + q[s] * (h * g[s])[..., None])
                    previous = incident * t[s][..., None] + q[s] * (h * f[s])[..., None]
                    if flux:
                        factor = FOUR_PI * WEIGHT * area
                        in_kind = "domain_in" if enter_index in (0, n) else "wall_in"
                        out_kind = "domain_out" if exit_index in (0, n) else "wall_out"
                        totals[in_kind] += factor * np.sum(incident[~faces.open[enter]], axis=0)
                        totals[out_kind] += factor * np.sum(previous[~faces.open[leave]], axis=0)
                        face_out[leave] = previous
                if flux:
                    outgoing[(axis, sign)] = face_out
        return {"J": j, "flux": totals, "outgoing": outgoing}

    def apply(self, source):
        """Lambda(source), with homogeneous zero boundary conditions."""
        return self.sweep(source)["J"]

    def boundary_field(self):
        return self.sweep(0., boundary=True)["J"]

    def matrix(self, u):
        u = self._rgb(u, "u")
        return u - self.sqrt_s * self.apply(self.sqrt_s * u)

    def solve(self, direct=0., max_iterations=64, tolerance=1.e-10):
        """Cold-start diagonal PCG; returns final Total=Direct+Boundary+Lambda(q).

        Direct is an already phase-normalized primary incident field, not q.
        q=sqrt(sigma_s)*u. No division by sigma_s is needed, including vacuum.
        No positivity clamp is applied to signed PCG vectors or matvec results.
        """
        direct = self._rgb(direct, "direct")
        if np.any(direct < 0) or max_iterations < 0 or tolerance <= 0:
            raise ValueError("Require nonnegative direct, iteration budget and positive tolerance")
        b = direct + self.boundary_field()
        rhs = self.sqrt_s * b
        u = np.zeros_like(rhs)
        r = rhs.copy()
        z = r / self.system_diagonal
        p = z.copy()
        dot = lambda a, b: np.sum(a * b, axis=(0, 1, 2))
        rhs_norm = np.sqrt(dot(rhs, rhs))
        target = tolerance * rhs_norm
        rz = dot(r, z)
        active = rhs_norm > 0
        iterations = 0
        history = []
        for iteration in range(max_iterations):
            if not np.any(active):
                break
            ap = self.matrix(p)
            denominator = dot(p, ap)
            if np.any(denominator[active] <= 0) or not np.all(np.isfinite(denominator)):
                raise ArithmeticError("PCG lost positive definiteness; do not clamp or continue silently")
            alpha = np.divide(rz, denominator, out=np.zeros(3), where=active)
            u += p * alpha
            r -= ap * alpha
            r_norm = np.sqrt(dot(r, r))
            iterations = iteration + 1
            history.append(np.divide(r_norm, rhs_norm, out=np.zeros(3), where=rhs_norm > 0).tolist())
            active = r_norm > target
            z = r / self.system_diagonal
            new_rz = dot(r, z)
            beta = np.divide(new_rz, rz, out=np.zeros(3), where=active & (rz != 0))
            p = z + p * beta
            p[..., ~active] = 0.
            rz = new_rz
        q = self.sqrt_s * u
        final = self.sweep(q, boundary=True, flux=True)
        diffuse = final["J"]
        total = direct + diffuse
        true_r = rhs - self.matrix(u)
        relative_r = np.divide(np.sqrt(dot(true_r, true_r)), rhs_norm,
                               out=np.zeros(3), where=rhs_norm > 0)
        equation_r = total - b - self.apply(self.sigma_s * total)
        scale = np.max(np.abs(total), axis=(0, 1, 2))
        relative_equation_r = np.divide(np.max(np.abs(equation_r), axis=(0, 1, 2)), scale,
                                        out=np.zeros(3), where=scale > 0)
        energy = final["flux"].copy()
        energy["absorbed_diffuse"] = FOUR_PI * self.volume * np.sum(self.sigma_a * diffuse, axis=(0, 1, 2))
        energy["direct_scattering_source"] = FOUR_PI * self.volume * np.sum(self.sigma_s * direct, axis=(0, 1, 2))
        incoming = energy["domain_in"] + energy["wall_in"] + energy["direct_scattering_source"]
        defect = energy["domain_out"] + energy["wall_out"] + energy["absorbed_diffuse"] - incoming
        energy["balance_defect"] = defect
        energy["relative_balance_defect"] = np.divide(defect, incoming, out=np.zeros(3), where=incoming > 0)
        energy["source_closure_defect"] = FOUR_PI * self.volume * np.sum(q - self.sigma_s * total, axis=(0, 1, 2))
        return {"total": total, "diffuse": diffuse, "source": q, "u": u,
                "iterations": iterations, "converged": bool(np.all(relative_r <= tolerance * 1.1)),
                "linear_relative_residual": relative_r, "equation_relative_residual": relative_equation_r,
                "energy": energy, "outgoing": final["outgoing"], "recurrence_history": history}


def _relative_error(a, b):
    return float(np.linalg.norm(a - b) / max(np.linalg.norm(b), 1.e-30))


def _summary(solution):
    return {"iterations": solution["iterations"], "converged": solution["converged"],
            "linear_relative_residual": solution["linear_relative_residual"].tolist(),
            "equation_relative_residual": solution["equation_relative_residual"].tolist(),
            "min_total": float(np.min(solution["total"])), "max_total": float(np.max(solution["total"])),
            "energy": {k: v.tolist() for k, v in solution["energy"].items()}}


def _slab_comparison(tau=4., albedo=.9, n=64, directions=64):
    """Independent dense 1D slab solves: six axes (four mu=0) versus Gauss.

    This comparison is NOT the 3D solver's correctness oracle. It quantifies
    angular error independently, using the same analytic cell-average physics.
    """
    def solve(mu, weights, parallel_weight):
        i = np.arange(n)
        distance = np.abs(i[:, None] - i[None, :])
        b = np.zeros(n)
        k = np.eye(n) * (parallel_weight * albedo)
        ts = []
        for m, w in zip(mu, weights):
            t, f, _ = beer_coefficients(tau / n / m)
            b += w * f * (t**i + t**(n - 1 - i))
            term = w * albedo * (1 - t) * f * t**np.maximum(distance - 1, 0)
            np.fill_diagonal(term, 2 * w * albedo * (1 - f))
            k += term
            ts.append(t)
        j = np.linalg.solve(np.eye(n) - k, b)
        outgoing = 0.
        for m, w, t in zip(mu, weights, ts):
            left = t**n + albedo * (1 - t) * np.sum(j * t**i)
            right = t**n + albedo * (1 - t) * np.sum(j * t**(n - 1 - i))
            outgoing += w * m * (left + right)
        incoming = 2 * np.sum(weights * mu)
        absorbed = tau * (1 - albedo) * np.mean(j)
        return j, {"center_J": float(.5 * (j[n//2-1] + j[n//2])),
                   "mean_J": float(j.mean()), "incoming_over_4pi": float(incoming),
                   "escaped_fraction": float(outgoing / incoming),
                   "absorbed_fraction": float(absorbed / incoming),
                   "relative_balance_defect": float((outgoing + absorbed - incoming) / incoming)}
    mu, w = np.polynomial.legendre.leggauss(directions // 2)
    mu, w = (mu + 1) / 2, w / 4  # each sign's weights; both hemispheres sum to 1
    six_j, six = solve(np.array([1.]), np.array([1/6]), 4/6)
    ref_j, ref = solve(mu, w, 0.)
    return {"scope": "infinite plane-parallel slab, two unit-radiance boundaries; not a 3D scene",
            "tau": tau, "albedo": albedo, "cells": n, "gauss_directions": directions,
            "six_axes": six, "gauss_reference": ref,
            "six_vs_gauss_relative_rms_J": _relative_error(six_j, ref_j),
            "six_axis_isotropic_face_flux_bias": -1/3,
            "angular_accuracy_verdict": "MEASURED APPROXIMATION; no angular-accuracy PASS claimed"}


def validate(cells=16):
    checks = []
    metrics = {}
    def check(name, condition, detail=None):
        checks.append({"name": name, "pass": bool(condition), "detail": detail})
    rng = np.random.default_rng(20260921)
    # Independent path quadrature checks both formal endpoint and cell mean.
    quadrature_x, quadrature_w = np.polynomial.legendre.leggauss(128)
    quadrature_x, quadrature_w = (quadrature_x + 1)/2, quadrature_w/2
    for tau in (0., 1.e-9, 1.e-4, .1, 1., 10., 100.):
        t, f, g = beer_coefficients(tau)
        h, incoming, q = 73., .7, .023
        sigma = tau/h
        path = incoming + q*h*quadrature_x if tau == 0 else (
            incoming*np.exp(-tau*quadrature_x) + q*h*(-np.expm1(-tau*quadrature_x))/tau)
        average = incoming*f + q*h*g
        outgoing = incoming*t + q*h*f
        check(f"formal_cell_average_tau_{tau:g}", abs(average - np.dot(path, quadrature_w)) < 2.e-12)
        check(f"cell_balance_tau_{tau:g}", abs(outgoing-incoming-h*(q-sigma*average)) < 2.e-12)
    # Heterogeneous, nonuniform scale, vacuum cells and shared opaque faces.
    sigma = rng.uniform(0., .012, (7, 8, 9))
    sigma[2:4, 3:5, 1:4] = 0.
    test = SixAxisTransport(sigma, rng.uniform(.2, 1., sigma.shape + (3,)), (75., 120., 200.))
    test.close_face("x", 4)
    test.faces["x"].open[2:5, 2:6, 4] = True
    symmetry = []
    for i in range(4):
        p = rng.normal(size=sigma.shape + (3,))
        q = rng.normal(size=p.shape)
        lp, lq = test.apply(p), test.apply(q)
        numerator = abs(np.sum(p*lq) - np.sum(lp*q))
        relative = float(numerator / max(np.linalg.norm(p)*np.linalg.norm(lq), 1.e-30))
        symmetry.append(relative)
        check(f"heterogeneous_reciprocity_{i}", relative < 2.e-14, relative)
    q = rng.normal(size=sigma.shape + (3,))
    check("signed_matvec_odd_symmetry", np.array_equal(test.apply(-q), -test.apply(q)))
    p = rng.normal(size=q.shape)
    check("system_positive_quadratic_form", np.sum(p*test.matrix(p)) > 0.)
    check("zero_source_zero_boundary", not np.any(test.apply(0.)))
    metrics["reciprocity_relative_defect"] = symmetry
    # White furnace from a cold zero-PCG start; no initialization with J=1.
    furnace = []
    for tau in (.1, 1., 4., 8., 16.):
        medium = SixAxisTransport(np.full((8, 8, 8), tau/800), 1., (100.,)*3)
        medium.set_isotropic_boundary([1., .7, 2.])
        result = medium.solve(max_iterations=64)
        error = float(np.max(np.abs(result["total"] / np.array([1., .7, 2.]) - 1)))
        balance = float(np.max(np.abs(result["energy"]["relative_balance_defect"])))
        check(f"white_furnace_tau_{tau:g}", result["converged"] and error < 2.e-9 and balance < 2.e-9,
              {"iterations": result["iterations"], "max_relative_error": error, "flux_defect": balance})
        furnace.append({"tau": tau, "max_relative_error": error, **_summary(result)})
    hetero = SixAxisTransport(sigma, 1., (75., 120., 200.))
    hetero.close_face("x", 4, [1., .7, 2.], [1., .7, 2.])
    hetero.set_isotropic_boundary([1., .7, 2.])
    result = hetero.solve(max_iterations=64)
    error = float(np.max(np.abs(result["total"] / np.array([1., .7, 2.]) - 1)))
    check("heterogeneous_wall_and_vacuum_furnace", result["converged"] and error < 2.e-9, error)
    metrics["furnace"] = furnace
    # One-sided pure absorption, analytic per-line transmitted T and cell means.
    absorption = SixAxisTransport(np.full((4, 5, 16), .003), 0., (100., 70., 120.))
    absorption.faces["x"].plus[:, :, 0] = [1., 2., .5]
    result = absorption.solve()
    t, f, _ = beer_coefficients(.3)
    analytic = f*np.exp(-.3*np.arange(16))[None, None, :, None]*np.array([1., 2., .5])/6
    transmitted = result["outgoing"][("x", 1)][:, :, -1]
    check("pure_absorption_cell_average_Beer", np.max(abs(result["total"]-analytic)) < 3.e-15)
    check("pure_absorption_outgoing_Beer", np.max(abs(transmitted-np.exp(-4.8)*np.array([1., 2., .5]))) < 3.e-15)
    check("pure_absorption_flux", np.max(abs(result["energy"]["relative_balance_defect"])) < 2.e-14)
    # More than the previous five-metre cutoff: a 12 m vacuum gap does not absorb.
    gap_sigma = np.full((3, 3, 16), .005)
    gap_sigma[:, :, 2:14] = 0.
    gap = SixAxisTransport(gap_sigma, 0., (100.,)*3)
    gap.faces["x"].plus[:, :, 0] = 1.
    result = gap.solve()
    transmitted = result["outgoing"][("x", 1)][:, :, -1]
    check("vacuum_gap_1200cm_no_range_cutoff", np.max(abs(transmitted-math.exp(-2.))) < 3.e-15)
    # A full reciprocal black plane partitions the transport graph exactly.
    wall = SixAxisTransport(np.full((8, 8, 8), .003), .95)
    wall.close_face("x", 4)
    wall.faces["x"].plus[:, :, 0] = 1.
    result = wall.solve(max_iterations=64)
    check("black_wall_zero_leakage", not np.any(result["total"][:, :, 4:]))
    check("black_wall_flux_includes_surface_sink", result["converged"] and
          np.max(abs(result["energy"]["relative_balance_defect"])) < 2.e-9 and
          np.all(result["energy"]["wall_out"] > 0))
    # Dense independently solved system validates PCG algebra and RGB albedo.
    dense = SixAxisTransport(rng.uniform(0., .02, (3, 3, 3)), .8, (30., 80., 140.))
    dense.close_face("z", 1, [.3, .3, .3], [.1, .1, .1])
    dense.set_isotropic_boundary(.5)
    n = dense.sigma_t.size
    lam = np.empty((n, n))
    for index in range(n):
        basis = np.zeros(dense.shape)
        basis.flat[index] = 1.
        lam[:, index] = dense.apply(basis)[..., 0].ravel()
    check("explicit_Lambda_symmetric", np.max(abs(lam-lam.T)) < 2.e-14)
    direct = rng.uniform(0., 1., dense.shape + (3,))
    b = direct + dense.boundary_field()
    oracle = np.linalg.solve(np.eye(n)-lam*dense.sigma_s[..., 0].ravel()[None, :], b.reshape(n, 3)).reshape(b.shape)
    result = dense.solve(direct, max_iterations=64, tolerance=1.e-12)
    check("PCG_vs_dense_solve", _relative_error(result["total"], oracle) < 2.e-12)
    check("direct_source_diffuse_flux_balance", np.max(abs(result["energy"]["relative_balance_defect"])) < 2.e-12)
    # Field budget comparison; 64 iterations are a reference ONLY if converged.
    z, y, x = np.indices((cells,)*3, dtype=float)/(cells-1)
    field_sigma = (8/(cells*100))*(.15+.85*(.5+.5*np.sin(5*x)*np.cos(4*y)*np.sin(3*z)))
    field = SixAxisTransport(field_sigma, [1., .95, .8])
    field.set_isotropic_boundary([.4, .8, .2])
    direct = np.stack((.1+np.exp(-((x-.25)**2+(y-.5)**2+(z-.4)**2)/.08), .2+x*.3, .05+z*.2), axis=-1)
    start = time.perf_counter()
    r24 = field.solve(direct, max_iterations=24, tolerance=1.e-11)
    elapsed24 = time.perf_counter()-start
    start = time.perf_counter()
    r64 = field.solve(direct, max_iterations=64, tolerance=1.e-11)
    elapsed64 = time.perf_counter()-start
    check("budget64_reference_converged", r64["converged"])
    check("budget64_finite_nonnegative", np.all(np.isfinite(r64["total"])) and np.min(r64["total"]) >= -1.e-12)
    check("budget64_flux_balance", np.max(abs(r64["energy"]["relative_balance_defect"])) < 2.e-9)
    doubled = field.solve(2*direct, max_iterations=64, tolerance=1.e-11)
    # Source linearity requires scaling boundary together with the direct source.
    field.set_isotropic_boundary([.8, 1.6, .4])
    doubled = field.solve(2*direct, max_iterations=64, tolerance=1.e-11)
    check("all_sources_double_linearly", _relative_error(doubled["total"], 2*r64["total"]) < 2.e-10)
    # A deliberately exhausted budget must expose error rather than report PASS.
    field.set_isotropic_boundary([.4, .8, .2])
    one = field.solve(direct, max_iterations=1, tolerance=1.e-11)
    check("exhausted_budget_not_converged", not one["converged"])
    closure_difference = one["energy"]["balance_defect"]-one["energy"]["source_closure_defect"]
    check("finite_iteration_defect_accounted", np.max(abs(closure_difference)) < 2.e-12*
          max(np.max(abs(one["energy"]["direct_scattering_source"])), 1.))
    metrics["budget"] = {"shape": list(field.shape), "cold_start": "zero u every solve; no history",
                          "24": _summary(r24), "64": _summary(r64), "one": _summary(one),
                          "24_vs_64_relative_rms": _relative_error(r24["total"], r64["total"]),
                          "one_vs_64_relative_rms": _relative_error(one["total"], r64["total"]),
                          "cpu_numpy_seconds_24": elapsed24, "cpu_numpy_seconds_64": elapsed64,
                          "timing_scope": "one CPU NumPy solve including diagnostics; not GPU performance"}
    angular = _slab_comparison()
    check("six_axis_slab_discrete_flux", abs(angular["six_axes"]["relative_balance_defect"]) < 2.e-13)
    check("gauss_slab_reference_flux", abs(angular["gauss_reference"]["relative_balance_defect"]) < 2.e-13)
    metrics["angular_error_separate_from_conservation"] = angular
    return {"scope": "CPU six-axis conservative discretization oracle; no GPU/native receiver validation",
            "verdict": "PASS" if all(c["pass"] for c in checks) else "FAIL",
            "checks": len(checks), "passed": sum(c["pass"] for c in checks),
            "results": checks, "metrics": metrics}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cells", type=int, choices=(8, 16, 32), default=16)
    parser.add_argument("--output", type=Path, help="Optional JSON receipt; default prints summary only")
    args = parser.parse_args()
    report = validate(args.cells)
    if args.output:
        args.output.write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps({"verdict": report["verdict"], "checks": report["checks"], "passed": report["passed"],
                      "failures": [r for r in report["results"] if not r["pass"]],
                      "budget": report["metrics"]["budget"],
                      "angular": report["metrics"]["angular_error_separate_from_conservation"]}, indent=2))
    raise SystemExit(0 if report["verdict"] == "PASS" else 1)
