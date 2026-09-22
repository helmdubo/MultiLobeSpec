# FogMS: W/S and custom SSFS field check

Use the existing `/Game/FogMS_Test/FogMS_Box` map. Do not create a new level or replace its authored Box, sun, sky, density or wind settings. Engine files are not modified. Check `FogMS_SSFS_Report.md` for the installed package and measured limitations.

## Candidate

Enter each line separately in the UE console:

```text
r.Fog.ScreenSpaceScattering 0
r.Fog.SeparateComposition 0
r.FogMS.ViewIntegration 0
r.FogMS.SSFS 1
r.FogMS.SSFS.Amount 1
r.FogMS.SSFS.Radius 24
```

This enables the independent full-HDR screen scattering pass on the previous temporal view integration. Runtime switches do not require FogMS.Apply or a shader rebuild. SSFS does not require expensive ViewIntegration3 and does not repair W/S sampling. Current correction: `FogMS_SSFS_FogLight_Fix.md`.

## W/S comparison

Experimental only: mode3 measured up to77ms in the owner view and is no longer the selected project preset. Do not enable it as part of normal SSFS testing.

Disable only screen scattering with `r.FogMS.SSFS 0`. Face an opening in the density from outside the Box, then travel toward and away from it with W/S over a substantial distance. Compare `r.FogMS.ViewIntegration 3` against `r.FogMS.ViewIntegration 0` at the same speed and route. Mode0 is the previous temporal view-integration path; it does not disable the FogMS transport solver. Do not use modes1/2 for field acceptance.

Check the opening's contour while moving and immediately after stopping. Failure: repeated edge jumps, a new dark fringe, a visible brightness ramp after stopping, or unacceptable frame time. Thin detail and camera-dependent XY froxel resolution are still limitations. The automatic sparse captures do not establish absence of continuous-motion shimmer.

## Solar disk and silhouettes

Keep integration0 and compare `r.FogMS.SSFS 0` / `1`. First look at lit structure inside the fog with the sun disk outside the view: bright/dark fog features should soften and spread even without a bright background source. Then look through the same density at the sun. Amount1/Radius24 should produce a smooth local patch; Amount.5 can retain a saturated sharp core and Radius48 spreads it further. Failure: no change in illuminated dense fog, rectangular hot patches, dark outlines, a sharp light boundary leaking across an opaque pillar, or a new temporal lag.

Move across a density opening with the sun behind it, and separately inspect pillar silhouettes. SSFS uses the shared integrated fog mask and is an image-space approximation; it is not Box-only, a new scattering order, or physical anisotropy in B3. Disable it when checking transport energy or native parity.

## Return to the previous rendering path

```text
r.FogMS.SSFS 0
r.FogMS.ViewIntegration 0
```

The current Box density, animation and transport settings remain authored independently. Leave the existing RHI crash compatibility guard unchanged.
