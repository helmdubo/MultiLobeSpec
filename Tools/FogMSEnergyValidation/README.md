# FogMS energy reference

`reference.py` is an independent **CPU reference**, not a GPU test and not a
measurement of the authored UE scene. Requires Python 3 and NumPy.

```powershell
python reference.py --output energy-reference.json
```

The slab has homogeneous extinction, isotropic scattering, no emission or
geometry, and unit isotropic radiance incident from both boundaries. Its width
is one; extinction equals total optical thickness. The discrete-ordinates
operator integrates the formal solution analytically over each cell, retaining
cell-average radiance. Each hemisphere uses Gauss-Legendre quadrature.

Assertions check the operator row balance, constant white-furnace solution at
albedo 1, and escaped plus absorbed flux against incident flux (2*pi). An albedo
0.9 case is repeated at 32/64/128 cells. These validate the independent reference;
they do not certify FogMS.

The output also evaluates B1-like variants: primary plus three added orders,
per-order damping .35/.5/1, and indirect-shadow blend 1/.5. These retain full
path coverage, accurate segment integration and common angular quadrature, so
they isolate the algebraic loss from order truncation/damping. They do not
reproduce B1's 5 m cutoff, 32-cubed sampling, geometry, Lumen, native fog mixture
or shader compositing. No result should be presented as the percentage error in
the user's screenshot.

For albedo 1 and optical thickness 1, reference center radiance is 1. The .35 / 3
added-order variant gives about .4362; even damping 1 with three orders gives
about .83344. Additional orders, domain coverage, conservative discretization and
native-source calibration require separate GPU validation before a physical
mode can be accepted. See `FogMS_Energy_Audit.md` in the plugin root.

Derivation background: [PBRT, Equation of Transfer](https://www.pbr-book.org/4ed/Light_Transport_II_Volume_Rendering/The_Equation_of_Transfer).
