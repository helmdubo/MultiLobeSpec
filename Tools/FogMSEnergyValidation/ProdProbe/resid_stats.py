# -*- coding: utf-8 -*-
"""Energy-weighted relative residual from FogMS.DumpSpatial rgba32f dumps (slab0: rgb=J, a=relative residual)."""
import sys, os, glob, numpy as np
N = 32
def stats(path):
    a = np.fromfile(path, dtype="<f4").reshape(-1, 4)
    s0 = a[:N * N * N]; J = s0[:, :3]; e = s0[:, 3]
    lum = 0.2126 * J[:, 0] + 0.7152 * J[:, 1] + 0.0722 * J[:, 2]
    w = np.maximum(lum, 0); ok = np.isfinite(e) & np.isfinite(w)
    wm = float((e[ok] * w[ok]).sum() / max(w[ok].sum(), 1e-20))
    p95 = float(np.percentile(e[ok], 95)); mx = float(e[ok].max())
    bright = w[ok] > np.percentile(w[ok], 50)
    return wm, p95, mx, float(e[ok][bright].max())
for p in sorted(glob.glob(sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "measure", "*", "*.rgba32f"))):
    try:
        wm, p95, mx, mxb = stats(p)
        print("%-58s weighted %.5f  p95 %.5f  max %.5f  max(bright half) %.5f" % (os.path.relpath(p, os.path.dirname(os.path.dirname(os.path.dirname(p)))), wm, p95, mx, mxb))
    except Exception as ex: print(p, "ERR", ex)
