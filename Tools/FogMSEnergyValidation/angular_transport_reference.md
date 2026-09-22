# FogMS B3: angular transport reference

This is a CPU reference for the proposed B3 discretization. It does not certify
the UE shader, native receiver reconstruction, sky adapter or final image.
The original `transport_reference.py` is unchanged and supplies its PCG and
diagnostic wrapper. NumPy is the only third-party dependency.

## Chosen discretization

Use positive paired ordinates with first-order upwind finite volumes (FV) on
a uniform Cartesian grid. Cell dimensions may differ along X, Y and Z but are
constant throughout the box. For a direction `omega`, define

```
a_x = abs(omega.x) / h_x  (and similarly Y, Z)
C = a_x + a_y + a_z
I_cell = (q_cell + sum_axis a_axis * I_incoming_axis) / (sigma_t + C)
J = sum_direction weight_direction * I_cell
```

`q` is radiance per length, `I` and `J` are radiance, `sigma_t` and `sigma_s`
are inverse length. Weights sum to one, so physical powers require the usual
`4*pi` factor. The single cell value is also the outgoing radiance on all
downwind faces. This is a first-order FV representation; it is **not** the
exact exponential cell average used in B2's axis sweeps.

Every geometric face has one reciprocal open flag. An open face transfers the
same outgoing value from its upwind cell. A closed face supplies externally
specified incoming radiance on each side and absorbs the outgoing flux. There
is no independently chosen visibility per propagation direction. A complete
black wall therefore disconnects the transport graph, including oblique rays.
The geometry is still the Cartesian face approximation: it does not prove
coverage of objects that intersect no represented center-to-center link.

Internal supplied radiance is a **diffuse, face-wise approximation** of the
existing B2 surface/Lumen adapter, not a full angular surface BSDF. Optional
direction-resolved input is supported at exterior faces. Supplying this input
must not overwrite internal face radiance.

## Why the operator is suitable for PCG

For fixed direction, let `B_omega` be the upwind matrix. Its diagonal is
`sigma_t + C`; an open upwind connection has coefficient `-a_axis`. Since each
face is reciprocal, `B_minus_omega = transpose(B_omega)`. Neither member of the
pair may use a separate visibility flag or different weight.

For any real vector `v`, the quadratic form of the symmetric part of `B` is
the sum of nonnegative absorption, open-face differences and closed-face
sinks:

```
sum_cells sigma_t * v_cell^2
+ 1/2 * sum_internal_open_faces a_axis * (v_left - v_right)^2
+ 1/2 * sum_closed_face_sides a_axis * v_cell^2.
```

Every component has a sink at an exterior or opaque boundary for at least one
nonzero streaming component, making the directional symmetric part positive
definite on this finite grid. If `y = inverse(B) * v`, then
`v^T inverse(B) v = y^T B^T y > 0`. Thus the equally weighted inverse pair is
symmetric positive definite and so is their positive weighted sum `Lambda`.

The transformed scattering system remains

```
u = sqrt(sigma_s) * J
A = identity - sqrt(sigma_s) * Lambda * sqrt(sigma_s)
rhs = sqrt(sigma_s) * (Direct + Boundary)
```

For `0 <= sigma_s <= sigma_t`, the streaming solve is positive and the finite
domain with prescribed incoming boundaries is subcritical through absorption
or escape. The corresponding positive scattering operator has spectral
radius less than one. Combined with symmetry, this gives SPD `A`. Vacuum
cells use the same transformation without division by `sigma_s`. The Jacobi
diagonal is exactly

```
Lambda_ii = sum_direction weight / (sigma_t_i + C_direction)
A_ii = 1 - sigma_s_i * Lambda_ii.
```

The CPU tool additionally constructs the full dense operator on a heterogeneous
grid with a hole in a wall, vacuum and RGB albedo. It checks symmetry, positive
eigenvalues and PCG agreement with a dense solve. These checks are independent
of the algebraic argument; they do not authorize nonuniform grids, asymmetric
visibility or an anisotropic phase function without a new derivation.

## Conservation and its limits

Multiply the FV equation by cell volume. The outgoing flux on a face is
`abs(omega_axis) * face_area * I_cell`; the adjacent cell consumes exactly
that same number. Open internal faces cancel when summing the domain. What
remains is incoming boundary power, outgoing boundary/surface power,
absorption and source power. Signed source/solution vectors obey the identity,
which is necessary for unclamped PCG matrix-vector products.

At a converged scattering solution, `q = sigma_s * (Direct + J_diffuse)`, giving
the diffuse subsystem identity reported by B2. At finite iteration budget,
the balance defect equals the integrated source-closure residual. A test
deliberately exhausts the budget and requires that defect to be reported.

This discrete balance does **not** establish continuous angular accuracy,
native point-density/image energy conservation or coupling of fog energy back
into UE surfaces. The same qualifications as B2 remain in force.

## Quadrature decision and exact ordering

Default candidate: `gauss2x12`, 48 ordinates. Quality candidate:
`gauss3x16`, 96 ordinates. Both use Gauss-Legendre separately on `[0,1]` in
each hemisphere, combined with midpoint periodic azimuth. This avoids putting
ordinates exactly on the equator and makes the Z-normal isotropic irradiance
exact. Irradiance for other normals is approximate; do not compensate it with
a global brightness multiplier.

Index order is
`hemisphere_index * polar_count * azimuth_count + polar_index * azimuth_count + k`.
Hemisphere signs are `(-1, +1)`; polar nodes are ascending; `k=0..A-1`.

```
phi = 2*pi*(k+0.5)/A
omega = (sqrt(1-mu^2)*cos(phi), sqrt(1-mu^2)*sin(phi), sign*mu)
weight = polar_weight / (2*A)
```

For 48 directions: `P=2, A=12`, nodes `(1 +/- 1/sqrt(3))/2`, polar weights
`(1/2,1/2)`; every direction weighs `1/48`.

For 96 directions: `P=3, A=16`, nodes
`((1-sqrt(3/5))/2, 1/2, (1+sqrt(3/5))/2)`, polar weights
`(5/18,4/9,5/18)`; direction weights are respectively
`(5/576,1/72,5/576)` per ring in each hemisphere.

The tool also implements cube-tile midpoint directions with exact tile solid
angles (`6*N*N` directions). Their exact tile weights do not make a midpoint
sample an exact integral of cosine or transport. They are retained as measured
alternatives, not selected merely for cubic symmetry.

## Measured accuracy, separate from solver correctness

The angular comparison is a homogeneous infinite plane slab with optical
thickness 4, albedo 0.9 and unit incoming radiance at both boundaries. For each
quadrature, two separate 1D solves use the same angular directions: exact
exponential cell averages and first-order FV. A fine half-range Gauss reference
uses 128 total directions and is checked at 64, 128 and 256 total directions.
Rotating the slab changes the angular projection; this is not a GPU rotated
box test and not a 3D rotated-wall accuracy claim.

At 32 slab cells, worst errors over the six documented normals are:

| Quadrature | Directions | Angular-only RMS | FV spatial RMS, same angles | Combined FV RMS |
|---|---:|---:|---:|---:|
| B2 axes | 6 | 16.617% | 1.830% | 16.827% |
| Cube N=2 | 24 | 3.433% | 1.896% | 4.857% |
| Cube N=3 | 54 | 2.042% | 1.750% | 2.670% |
| Cube N=4 | 96 | 0.700% | 1.806% | 2.065% |
| Half-range GL 2x12 | 48 | 0.844% | 1.811% | 2.240% |
| Half-range GL 3x16 | 96 | 0.354% | 1.771% | 1.819% |

The B2 row's spatial/combined columns describe a hypothetical six-direction
**FV** method, not the actual B2 exponential solver. The angular-only column
does correspond to its reference angular limitation.

A denser deterministic survey of 102 normals increases the worst measured
angular RMS to 0.868% for GL48 and 0.394% for GL96. Maximum absolute isotropic
irradiance bias in this survey is 2.532% and 1.189%, respectively. These are
sampled maxima, not global bounds. GL48's bias on X/Y normals is +2.531%; it
is zero on Z. The reference reports every worst-case normal.

At 64/128 cells, worst six-normal combined FV errors become 1.504%/1.137%
for GL48 and 1.040%/0.631% for GL96. The isolated spatial error roughly halves
as cell size halves. Pure absorption has the FV transmission
`(1 + tau/N)^(-N)` and converges to Beer `exp(-tau)`; it is not Beer-exact at
finite N. The 12m vacuum-gap check asserts zero additional range attenuation,
while preserving that declared absorption discretization bias.

These results support GL48 as an initial quality/cost candidate and GL96 as
a higher-angular-quality option. They do not measure GPU cost or prove 32^3
spatial sufficiency for detailed Perlin density. Upwind diffusion can broaden
fine lighting features. There is no claim that adding directions cures coarse
spatial density or geometry sampling.

## API and reproduction

```python
medium = AngularTransport(sigma_t, albedo, (hx, hy, hz), "gauss2x12")
medium.faces["x"].open[...] = reciprocal_boolean_mask
medium.faces["x"].plus[...] = incoming_rgb_to_positive_x_side
medium.faces["x"].minus[...] = incoming_rgb_to_negative_x_side
# Optional exterior-only sky value: RGB or full face-shape RGB array.
medium.external_incoming[(direction_index, "x")] = sky_radiance
result = medium.solve(direct_field, max_iterations=64, tolerance=1e-10)
```

Arrays are `[z,y,x]` or `[z,y,x,RGB]`; extent/spacing arguments are X,Y,Z.
Faces use the B2 layout with N+1 cells on the corresponding axis. The result
uses the same `total`, `diffuse`, `source`, `iterations`, residual and `energy`
keys as B2. The diagnostic `outgoing` mapping differs: key is direction index,
value is that ordinate's FV cell field `[z,y,x,RGB]`, not a B2 endpoint face
array. Primary/secondary GPU comparisons should use the named field/flux keys.

```powershell
python -P angular_transport_reference.py --cells 6 --output b3-gl48.json
python -P angular_transport_reference.py --quadrature gauss3x16 --cells 8 --skip-angular --output b3-gl96.json
```

Checks include paired quadrature validity, heterogeneous dense symmetry/SPD,
RGB PCG versus dense solution, signed flux identity, cold-start furnaces at
three optical thicknesses, constant/directional vacuum input, a 12m vacuum
gap, Beer convergence, thin black wall separation with a nonzero lit control,
finite-budget diagnostics and doubled-source linearity. Full mode adds slab
discrete flux checks, angular and spatial refinement, plus the rotation survey.
All operations are local CPU calculations; the command does not modify UE.
