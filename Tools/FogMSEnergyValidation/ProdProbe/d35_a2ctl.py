# -*- coding: utf-8 -*-
"""A2 control in the current lighting: re-create the round-34 forcing (TLV SpatialFilter 0 + Temporal.Jitter 0) at g=0.7 to show
the blocks come back only with it; restores both cvars to their pre-run values and g to its pre-run value."""
import os, time, json
import diag35lib as L
import d35_parta as P
d = L.d
pre = L.getcv(["r.Lumen.TranslucencyVolume.SpatialFilter", "r.Lumen.TranslucencyVolume.Temporal.Jitter"])
g0 = L.snapshot()["hf"]["volumetric_fog_scattering_distribution"]
d.set_cam(*P.CAM0)
res = json.load(open(os.path.join(L.RES, "a2_blocks.json")))
d.set_g(0.7); d.cmd("r.Lumen.TranslucencyVolume.SpatialFilter 0"); d.cmd("r.Lumen.TranslucencyVolume.Temporal.Jitter 0"); time.sleep(6)
d.capture("d1_ctl_sf0tj0_g0.7", 4)
L.setcv({k: "%d" % v for k, v in pre.items()}); d.set_g(g0); time.sleep(2)
r = P.mottle("d1_ctl_sf0tj0_g0.7"); res["ctl_sf0tj0_g0.7"] = r
res["ctl_restored"] = {"cvars": L.getcv(list(pre)), "g": L.snapshot()["hf"]["volumetric_fog_scattering_distribution"]}
print(r, res["ctl_restored"])
json.dump(res, open(os.path.join(L.RES, "a2_blocks.json"), "w"), indent=1)
