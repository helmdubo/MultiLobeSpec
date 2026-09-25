# -*- coding: utf-8 -*-
"""Round 35 Part B: W/S dolly vs A/D strafe camera tremble. Frame-locked camera motion (one step per editor tick, the
diag34lib.capture slate callback) along the view axis (W forward, S backward) or the right axis (D right, A left), same
speed and path length, the dumped segment centred on the start camera. N consecutive real viewport frames per run.
Metric (as d2_move.py): temporal second difference |L(t+1) - 2L(t) + L(t-1)| of 8-bit luminance, mean over the ROI (frame
minus editor overlays: axis gizmo, a thin world-space debug line found per run, 8-px borders). A constant-speed move
changes the image almost linearly, so d2 isolates frame-to-frame tremble. Also reported: d2 of a 9x9 box-blurred image
(lighting-scale tremble, less sensitive to moving sharp geometry edges), p99, fraction of pixels with d2 > 4, d1 (mean
|L(t+1) - L(t)|, how much the image moves), and a depth-binned d2 using the SceneDepth of the start frame (bins: see DBINS).
Usage: python d35_dolly.py <variant[,variant]> [dirs=W,S,A,D] [step_cm=20] [N=40] [cam=start|owner|cam0] [tag]
       python d35_dolly.py roi <cam>   (static cloud ROI: current vs Density 0)   |   python d35_dolly.py reanalyse
Variants set their state, capture every direction, and revert (cvars to the values read before the run, Box/Height Fog
props to the snapshot taken at the start of the run)."""
import sys, os, time, json, math
import numpy as np
import diag35lib as L
d = L.d

CAMS = {"start": ((-5696.867655, -5800.397253, 2370.352606), (-0.324906, 50.138568, 0.0)),
        "high": ((-6000.0, 2906.0, 7000.0), (12.0, 0.0, 0.0)),
        "owner": ((-282.7, 8738.9, 2438.5), (4.08, 243.94, 0.0)),
        "cam0": ((-1644.241818024519, 17617.726190341542, 2442.208869304058), (1.4750940536441703, -58.26143420987347, 0.0))}
FREEZE = None
BOX = lambda s: s
V = {  # name: (cvars, box assignment, height-fog props)
    "cur": ({}, None, None), "cur_rep": ({}, None, None), "cur_rep2": ({}, None, None),
    "boxoff": ({}, "b.set_editor_property('enabled', False)", None),
    "dens0": ({}, "b.set_editor_property('density', 0.0)", None),
    "modeoff": ({}, "b.set_editor_property('scattering_mode', unreal.FogMSScatteringMode.OFF)", None),
    "det0": ({}, "b.set_editor_property('detail_strength', 0.0)", None),
    "soft04": ({}, "b.set_editor_property('softness', 0.4)", None),
    "soft08_det0": ({}, "b.set_editor_property('softness', 0.8); b.set_editor_property('detail_strength', 0.0)", None),
    "feather2000": ({}, "b.set_editor_property('density_edge_feather', 2000.0)", None),
    "gz128": ({"r.VolumetricFog.GridSizeZ": "128"}, None, None),
    "gz256": ({"r.VolumetricFog.GridSizeZ": "256"}, None, None),
    "gz384": ({"r.VolumetricFog.GridSizeZ": "384"}, None, None),
    "gz512": ({"r.VolumetricFog.GridSizeZ": "512"}, None, None),
    "dds16": ({"r.VolumetricFog.DepthDistributionScale": "16"}, None, None),
    "dds64": ({"r.VolumetricFog.DepthDistributionScale": "64"}, None, None),
    "vfd30k": ({}, None, {"volumetric_fog_distance": 30000.0}),
    "vfd60k": ({}, None, {"volumetric_fog_distance": 60000.0}),
    "vfd100k": ({}, None, {"volumetric_fog_distance": 100000.0}),
    "jit0": ({"r.VolumetricFog.Jitter": "0"}, None, None),
    "hw0.8": ({"r.VolumetricFog.HistoryWeight": "0.8"}, None, None),
    "hw0.95": ({"r.VolumetricFog.HistoryWeight": "0.95"}, None, None),
    "hw0": ({"r.VolumetricFog.HistoryWeight": "0"}, None, None),
    "inj_hyb": ({}, "b.set_editor_property('emissive_injection', True); b.set_editor_property('hybrid_single_scattering', True)", None),
    "ds8": ({"r.FogMS.Transport.DirectSamples": "8"}, None, None),
    "si1": ({"r.FogMS.Transport.SolveInterval": "1"}, None, None),
    "lumtv0": ({"r.Lumen.TranslucencyVolume.Enable": "0"}, None, None),
    "vi3": ({"r.FogMS.ViewIntegration": "3"}, None, None),
    "vi1": ({"r.FogMS.ViewIntegration": "1"}, None, None),
}
# W36 Depth Prefilter A/B (round-36 plugin: the Box property depth_prefilter must exist). Since W36 a Box loads with Emissive
# Injection + Hybrid on (new defaults, not serialized before), so 'cur' is injection + hybrid at the Box's Depth Prefilter
# (default 1). 'ovl' = the round-35 'cur' state (overlay: the overlay injects its own unfiltered density, MID density 0, so
# the prefilter cannot act there). ih_pfX = injection + hybrid at Depth Prefilter X (ih_pf0 = round-35 'inj_hyb').
# depth_prefilter joins the snapshot/restore list, so every variant (also in d35_slide.py) restores it afterwards.
if "depth_prefilter" not in L.BOXPROPS: L.BOXPROPS.append("depth_prefilter")
_IH = "b.set_editor_property('emissive_injection', True); b.set_editor_property('hybrid_single_scattering', True); "
V.update({
    "ovl": ({}, "b.set_editor_property('emissive_injection', False); b.set_editor_property('hybrid_single_scattering', False)", None),
    "ih_pf0": ({}, _IH + "b.set_editor_property('depth_prefilter', 0.0)", None),
    "ih_pf05": ({}, _IH + "b.set_editor_property('depth_prefilter', 0.5)", None),
    "ih_pf1": ({}, _IH + "b.set_editor_property('depth_prefilter', 1.0)", None),
    "lobe05": ({}, "b.set_editor_property('forward_scattering', 0.5); b.set_editor_property('forward_anisotropy', 0.6)", None),
    "ih_pf2": ({}, _IH + "b.set_editor_property('depth_prefilter', 2.0)", None),
})
DBINS =[0, 2000, 5000, 10000, 20000, 1e9]

def basis(rot):
    p, y = math.radians(rot[0]), math.radians(rot[1])
    fwd = (math.cos(p) * math.cos(y), math.cos(p) * math.sin(y), math.sin(p))
    right = (-math.sin(y), math.cos(y), 0.0)
    return fwd, right

def roi_mask(F):
    """Frame minus 8-px borders, the axis gizmo, and a thin dark vertical world-space debug line present in this scene
    (found in every frame: pixels darker than BOTH neighbours 3 px away by > 25 levels, columns with > 80 such rows;
    union over frames, +-4 px)."""
    Lm = F.mean(axis=3)
    H, W = Lm.shape[1:]
    m = np.ones((H, W), bool); m[:8] = m[-8:] = False; m[:, :8] = m[:, -8:] = False
    m[H - 110:, :110] = False
    for f in Lm:
        c = f[:, 3:-3]
        thin = ((f[:, :-6] - c) > 25) & ((f[:, 6:] - c) > 25)
        for x in np.where(thin.sum(axis=0) > 80)[0] + 3: m[:, max(0, x - 4): x + 5] = False
    return m

def cloud_roi(cam):
    p = os.path.join(d.M, "roi_%s.npy" % cam)
    return np.load(p) if os.path.isfile(p) else None

def make_cloud_roi(cam):
    """Static captures at the camera: current state and Density 0 (no cloud). Cloud ROI = |on - dens0| > 8 levels, minus
    pixels near strong edges of the no-cloud image (geometry silhouettes), dilated 6 px."""
    from scipy.ndimage import uniform_filter, binary_dilation, sobel
    loc, rot = CAMS[cam]
    d.set_cam(loc, rot); time.sleep(3); d.capture("ref_%s_on" % cam, 4)
    snap = L.snapshot()
    d.setbox("b.set_editor_property('density', 0.0)"); time.sleep(6); d.capture("ref_%s_dens0" % cam, 4)
    d.setbox(L.box_assign(snap["box"])); time.sleep(4)
    A = d.load("ref_%s_on" % cam).mean(axis=3).mean(0); B = d.load("ref_%s_dens0" % cam).mean(axis=3).mean(0)
    g = np.hypot(sobel(uniform_filter(B, 3), 0), sobel(uniform_filter(B, 3), 1))
    roi = (np.abs(A - B) > 8) & ~binary_dilation(g > 60, iterations=6)
    np.save(os.path.join(d.M, "roi_%s.npy" % cam), roi)
    print("cloud ROI", cam, "fraction", round(float(roi.mean()), 3), "status", d.status()[:80])
    return roi

def analyse(name, cam=None):
    from scipy.ndimage import uniform_filter
    from scipy.signal import savgol_filter
    F = d.load(name); Lm = F.mean(axis=3)
    m = roi_mask(F)
    Lb = uniform_filter(Lm, size=(1, 9, 9))
    d2 = np.abs(Lm[2:] - 2 * Lm[1:-1] + Lm[:-2]); d2b = np.abs(Lb[2:] - 2 * Lb[1:-1] + Lb[:-2])
    hp = np.abs(Lb - savgol_filter(Lb, 9, 2, axis=0))[4:-4]      # residual of a 9-frame local quadratic fit (blurred image)
    d1 = np.abs(np.diff(Lm, axis=0))
    r = {"d2": round(float(d2[:, m].mean()), 3), "d2_blur": round(float(d2b[:, m].mean()), 3), "hp9": round(float(hp[:, m].mean()), 3),
         "d2_p99": round(float(np.percentile(d2[:, m], 99)), 2), "frac_d2_gt4": round(float((d2[:, m] > 4).mean()), 4),
         "d1": round(float(d1[:, m].mean()), 3), "mean": round(float(Lm[:, m].mean()), 2), "frames": int(F.shape[0])}
    c = cloud_roi(cam) if cam else None
    if c is not None:
        mc = m & c
        r.update({"c_d2_blur": round(float(d2b[:, mc].mean()), 3), "c_hp9": round(float(hp[:, mc].mean()), 3),
                  "c_d1": round(float(d1[:, mc].mean()), 3), "c_frac": round(float(mc.mean()), 3)})
    np.save(os.path.join(d.M, name, "d2bmap.npy"), (d2b.mean(0) * m).astype(np.float32))
    return r

def wobble(name, cam=None, tile=64):
    """Tile-mean 'wobble': per 64x64 tile (>= 80 % valid ROI pixels) the mean luminance time series, minus a local quadratic
    trend (Savitzky-Golay window 21, order 2: removes the smooth change a constant-speed move causes, whether zoom or pan),
    residual RMS in 8-bit levels. Median and 75th percentile over tiles, and the dominant period (frames) of the tile-averaged
    residual power spectrum. Coherent pan/zoom of static content changes a tile mean smoothly; depth-slice aliasing pumps it."""
    from scipy.signal import savgol_filter
    F = d.load(name); Lm = F.mean(axis=3); T, H, W = Lm.shape
    m = roi_mask(F); c = cloud_roi(cam) if cam else None
    if c is not None: m = m & c
    res, specs = [], []
    for y in range(0, H - tile + 1, tile):
        for x in range(0, W - tile + 1, tile):
            mm = m[y:y + tile, x:x + tile]
            if mm.mean() < 0.8: continue
            s = Lm[:, y:y + tile, x:x + tile][:, mm].mean(axis=1)
            r = (s - savgol_filter(s, 21, 2))[3:-3]
            res.append(float(np.sqrt((r ** 2).mean())))
            specs.append(np.abs(np.fft.rfft(r - r.mean())) ** 2)
    if not res: return {}
    P = np.mean(specs, axis=0); f = np.fft.rfftfreq(T - 6); k = int(np.argmax(P[1:]) + 1)
    return {"wob_med": round(float(np.median(res)), 3), "wob_p75": round(float(np.percentile(res, 75)), 3),
            "wob_period": round(float(1.0 / f[k]), 1), "wob_tiles": len(res)}

PATHS = {  # same world segment for every direction: horizontal line u through 'centre'; W/S look along u, D/A look 90 deg
    # left/right of u and strafe along it (D: +u entering, A: -u... see path_runs). Centre 35 m ahead of the start camera,
    # where the path enters the cloud between the pillars.
    "edge": {"centre_from": "start", "ahead_cm": 3500.0},
    # geometry-free: 70 m up (pillar tops end at 55 m), pitch 12 deg, horizontal path +x into the cumulus mass
    "high": {"centre_from": "high", "ahead_cm": 0.0},
}

def path_runs(name):
    loc, rot = CAMS[PATHS[name]["centre_from"]]
    y = math.radians(rot[1]); u = (math.cos(y), math.sin(y), 0.0)
    c = tuple(loc[i] + u[i] * PATHS[name]["ahead_cm"] for i in range(3))
    nu = tuple(-x for x in u); pt = rot[0] if name != "edge" else 0.0
    return c, {"W": ((pt, rot[1], 0.0), u), "S": ((pt, rot[1], 0.0), nu),
               "D": ((pt, rot[1] - 90.0, 0.0), u), "A": ((pt, rot[1] + 90.0, 0.0), u),
               "Ax": ((pt, rot[1] + 90.0, 0.0), nu), "0": ((pt, rot[1], 0.0), (0.0, 0.0, 0.0))}

def run(variant, dirs=("W", "S", "A", "D"), step=20.0, N=40, cam="owner", tag=""):
    cv, box, hf = V[variant]
    if cam in PATHS:
        loc, per = path_runs(cam)
        vec = {k: v[1] for k, v in per.items()}; rots = {k: v[0] for k, v in per.items()}
    else:
        loc, rot = CAMS[cam]
        fwd, right = basis(rot)
        vec = {"W": fwd, "S": tuple(-c for c in fwd), "D": right, "A": tuple(-c for c in right), "0": (0.0, 0.0, 0.0)}
        rots = {k: rot for k in vec}
    pre_cv = L.getcv(list(cv)) if cv else {}
    snap = L.snapshot() if (box or hf) else None
    for k, v in cv.items(): d.cmd("%s %s" % (k, v))
    if hf: L.set_hf(hf)
    st = d.setbox(box) if box else d.status()
    time.sleep(6.0)
    out = {}
    rp = os.path.join(L.RES, "b_dolly.json"); res = json.load(open(rp)) if os.path.isfile(rp) else {}
    for D in dirs:
        u = vec[D]; mv = tuple(c * step for c in u)
        c0 = tuple(loc[i] - u[i] * step * (12 + N / 2.0) for i in range(3))
        d.set_cam(c0, rots[D]); time.sleep(2.5)
        nm = "b_%s_%s_%s_s%g%s" % (cam, variant, D, step, tag)
        d.capture(nm, N, move=mv if D != "0" else None)
        rc = None if cam in PATHS else cam
        r = analyse(nm, rc); r.update(wobble(nm, rc)); r["status"] = d.status()[:160]
        res[nm] = r; out[D] = r
        print("%-34s wob %s/%s per %s | d2b %.3f hp9 %.3f | cloud d2b %s hp9 %s d1 %s | d1 %.3f mean %.1f" % (nm, r.get("wob_med"), r.get("wob_p75"), r.get("wob_period"), r["d2_blur"], r["hp9"], r.get("c_d2_blur"), r.get("c_hp9"), r.get("c_d1"), r["d1"], r["mean"]), flush=True)
        json.dump(res, open(rp, "w"), indent=1)
    # revert
    for k, v in pre_cv.items(): d.cmd("%s %s" % (k, ("%d" % v) if float(v).is_integer() else ("%g" % v)))
    if snap:
        d.setbox(L.box_assign(snap["box"])); L.set_hf(snap["hf"])
    time.sleep(2.0)
    return out, st

if __name__ == "__main__":
    a = sys.argv[1:]
    if a[0] == "roi":
        make_cloud_roi(a[1]); sys.exit(0)
    if a[0] == "reanalyse":
        rp = os.path.join(L.RES, "b_dolly.json"); res = json.load(open(rp))
        for nm in list(res):
            cam = nm.split("_")[1]; cam = None if cam in PATHS else cam; st = res[nm].get("status", ""); res[nm] = analyse(nm, cam); res[nm].update(wobble(nm, cam)); res[nm]["status"] = st
            print(nm, {k: v for k, v in res[nm].items() if k != "status"}, flush=True)
        json.dump(res, open(rp, "w"), indent=1); sys.exit(0)
    variant = a[0]
    dirs = tuple(a[1].split(",")) if len(a) > 1 else ("W", "S", "A", "D")
    step = float(a[2]) if len(a) > 2 else 20.0
    N = int(a[3]) if len(a) > 3 else 40
    cam = a[4] if len(a) > 4 else "owner"
    tag = a[5] if len(a) > 5 else ""
    for v in variant.split(","):
        out, st = run(v, dirs, step, N, cam, tag)
        print(v, "| status:", st[:140], flush=True)
