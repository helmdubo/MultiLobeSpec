# -*- coding: utf-8 -*-
"""Round 35 Part B, 'grid slide': the camera stays still and only the volumetric-fog froxel slices move in depth, by
growing the Height Fog's VolumetricFogStartDistance by <step> cm per frame (every slice at depth z << View Distance moves
~<step> cm, like a W dolly of that speed; the fog nearer than the start distance is cut, so the camera must look at a
cloud farther than the total slide), or by scaling View Distance by (1+eps) per frame (slices move by z*eps, nothing is
cut). Nothing else in the frame moves, so ANY temporal change is the depth resampling of the medium (plus the static
noise floor, measured with step 0). Metrics (static camera, no motion confound): d2 / d2_blur (temporal second
difference, raw and 9x9-blurred luminance), hp9 (residual of a 9-frame local quadratic fit), tile wobble (d35_dolly),
and the mean absolute change from the first to the last frame.
Usage: python d35_slide.py <variant[,variant]> [mode=start|scale] [step=20 (cm) or eps=0.004] [N=40] [cam=high] [tag]
Variants = d35_dolly.V (cvars / Box / Height Fog); the Height Fog start distance and View Distance are restored after
every run (to the values read before it)."""
import sys, os, time, json
import numpy as np
import diag35lib as L
import d35_dolly as B
d = L.d

def analyse(name):
    r = B.analyse(name); r.update(B.wobble(name))
    F = d.load(name).mean(axis=3); m = B.roi_mask(d.load(name))
    r["first_last"] = round(float(np.abs(F[-1] - F[0])[m].mean()), 3)
    return r

def run(variant, mode="start", step=20.0, N=40, cam="high", tag=""):
    cv, box, hf = B.V[variant]
    loc, rot = B.CAMS[cam]
    pre_cv = L.getcv(list(cv)) if cv else {}
    snap = L.snapshot()
    hf0 = {k: snap["hf"][k] for k in ("volumetric_fog_start_distance", "volumetric_fog_distance")}
    for k, v in cv.items(): d.cmd("%s %s" % (k, v))
    if hf: L.set_hf(hf)
    st = d.setbox(box) if box else d.status()
    d.set_cam(loc, rot); time.sleep(6.0)
    rp = os.path.join(L.RES, "b_slide.json"); res = json.load(open(rp)) if os.path.isfile(rp) else {}
    nm = "s_%s_%s_%s%g%s" % (cam, variant, mode, step, tag)
    if mode == "start": L.capture_slide(nm, N, start_step=step)
    else: L.capture_slide(nm, N, start_step=0.0, dist_scale_step=step)
    L.set_hf(dict(hf0, **(hf or {})))
    r = analyse(nm); r["status"] = st[:160]; res[nm] = r
    print("%-34s d2 %.3f d2b %.3f hp9 %.3f wob %s/%s per %s first-last %.2f mean %.1f" % (nm, r["d2"], r["d2_blur"], r["hp9"], r.get("wob_med"), r.get("wob_p75"), r.get("wob_period"), r["first_last"], r["mean"]), flush=True)
    json.dump(res, open(rp, "w"), indent=1)
    for k, v in pre_cv.items(): d.cmd("%s %s" % (k, ("%d" % v) if float(v).is_integer() else ("%g" % v)))
    if box or hf: d.setbox(L.box_assign(snap["box"]))
    L.set_hf(hf0 if not hf else snap["hf"])
    time.sleep(2.0)
    return r

if __name__ == "__main__":
    a = sys.argv[1:]
    mode = a[1] if len(a) > 1 else "start"
    step = float(a[2]) if len(a) > 2 else (20.0 if mode == "start" else 0.004)
    N = int(a[3]) if len(a) > 3 else 40
    cam = a[4] if len(a) > 4 else "high"
    tag = a[5] if len(a) > 5 else ""
    for v in a[0].split(","): run(v, mode, step, N, cam, tag)
