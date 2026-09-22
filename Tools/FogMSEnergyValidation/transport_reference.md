# Six-ordinate conservative reference

`transport_reference.py` implements exact cell-average full-line transport along
the six Cartesian directions. It is an independent CPU reference for B2, not a
measurement of Unreal Engine or proof of angular accuracy.

```powershell
python transport_reference.py --cells 32 --output transport-reference.json
```

The test suite checks 44 conditions: homogeneous and heterogeneous white
furnaces, zero density/scattering, absorption/Beer, reciprocal black walls,
propagation across a vacuum gap, symmetry of the transport operator, PCG against
a dense linear solve, convergence-budget exhaustion and diffuse flux balance.

Coefficients use inverse centimetres. Cell arrays are `[z,y,x]`, RGB is the final
dimension. `SixAxisTransport(sigma_t, albedo, cell_size_cm=(x,y,z))` owns shared
faces: each open face passes the same outgoing radiance into its neighbour.
Closed faces have separate incident radiance for either side. There is no
source interpolation through a barrier and no arbitrary maximum path radius.

For isotropic scattering, `J = Bdirect + Bboundary + Lambda(sigma_s * J)`.
With `u = sqrt(sigma_s) * J`, diagonal-preconditioned conjugate gradients solve
`(I - sqrt(sigma_s) Lambda sqrt(sigma_s)) u = sqrt(sigma_s) B`.
`Lambda` is symmetric on this uniform Cartesian grid with reciprocal barriers.
Signed intermediate vectors must not be clamped. Cells with zero scattering
need no division: reconstruct `J = B + Lambda(sqrt(sigma_s) * u)`.

The diffuse energy equation includes boundary/domain and obstacle inflow and
outflow, absorption of diffuse radiance, and the volume source
`4*pi*sum(cell_volume*sigma_s*Bdirect)`. It does not claim a complete energy
balance for the external direct-light beam or surface GI system.

**Conservation and angular accuracy are separate.** Six axes give isotropic
irradiance `2*pi*L/3` on an axis-aligned boundary, versus physical `pi*L`.
For a slab with optical thickness 4 and albedo .9, six axes differ from the
64-ordinate Gauss reference by approximately 16.59% RMS mean radiance, while
each scheme conserves its own discretized flux. Do not conceal this error by
rescaling brightness. B2 is a controlled isotropic discretization with a
measured angular limitation, not a converged continuous-direction renderer.
