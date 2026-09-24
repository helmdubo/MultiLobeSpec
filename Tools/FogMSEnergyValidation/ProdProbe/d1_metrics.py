# -*- coding: utf-8 -*-
"""D1 metrics over the captured d1_* runs: 'grid lock' = the horizontal+vertical luminance-gradient profile folded modulo
the Lumen translucency-volume cell (32 px, 16 px for the GridPixelSize-16 run) and modulo 4/8 px (volumetric-fog froxel),
reported as peak/mean of the folded profile (1.0 = no grid; blocks aligned to the screen-space TLV grid give a sharp peak);
'mottle' = std of a band-pass (Gauss sigma 6 minus sigma 30) luminance image over the upper 2/3 of the frame (cloud)."""
import os, glob, json
import numpy as np
from scipy.ndimage import gaussian_filter
import diag34lib as d

def fold(I, p):
    gx = np.abs(np.diff(I, axis=1)).mean(0); gy = np.abs(np.diff(I, axis=0)).mean(1)
    fx = np.array([gx[k::p].mean() for k in range(p)]); fy = np.array([gy[k::p].mean() for k in range(p)])
    return round(float(fx.max() / fx.mean()), 2), round(float(fy.max() / fy.mean()), 2)

rows = {}
for dd in sorted(g for g in glob.glob(os.path.join(d.M, "d1_*")) if os.path.isdir(g)):
    n = os.path.basename(dd)[3:]
    L = d.load("d1_" + n).mean(axis=3); I = L.mean(0)[:540]
    bp = gaussian_filter(I, 6) - gaussian_filter(I, 30)
    rows[n] = {"mean": round(float(I.mean()), 1), "mottle": round(float(bp.std()), 2),
               "grid32_xy": fold(I, 32), "grid16_xy": fold(I, 16), "grid8_xy": fold(I, 8)}
    print("%-24s mean %6.1f mottle %5.2f grid32 %s grid16 %s grid8 %s" % (n, rows[n]["mean"], rows[n]["mottle"], rows[n]["grid32_xy"], rows[n]["grid16_xy"], rows[n]["grid8_xy"]))
json.dump(rows, open(os.path.join(d.M, "d1_metrics.json"), "w"), indent=1)
