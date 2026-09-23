# -*- coding: utf-8 -*-
"""Relative difference between two transport dumps' J fields (slab0 rgb): energy-weighted mean |dJ|/J_ref and L2."""
import sys, numpy as np
N = 32
def J(p): return np.fromfile(p, dtype="<f4").reshape(-1, 4)[:N**3, :3].astype(np.float64)
a, b = J(sys.argv[1]), J(sys.argv[2])
la = 0.2126*a[:,0]+0.7152*a[:,1]+0.0722*a[:,2]; lb = 0.2126*b[:,0]+0.7152*b[:,1]+0.0722*b[:,2]
ok = np.isfinite(la) & np.isfinite(lb)
rel_l2 = np.sqrt(((a-b)[ok]**2).sum() / max((b[ok]**2).sum(), 1e-30))
w = np.maximum(lb[ok], 0); wm = float((np.abs(la-lb)[ok] * w).sum() / max((lb[ok]*w).sum(), 1e-30))
bright = w > np.percentile(w, 50)
print("%s vs %s: rel L2 %.4f | weighted mean rel diff %.4f | mean lum ratio %.4f | bright-half mean |dJ|/J %.4f" % (
    sys.argv[1].split('/')[-2], sys.argv[2].split('/')[-2], rel_l2, wm, float(la[ok].sum()/max(lb[ok].sum(),1e-30)),
    float((np.abs(la-lb)[ok][bright] / np.maximum(lb[ok][bright], 1e-9)).mean())))
