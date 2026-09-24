# -*- coding: utf-8 -*-
"""D2: lighting in the cloud 'trembles' while the camera moves. Scripted slow lateral camera translation (STEP cm per
frame along the camera's right vector, one step per editor tick, dumping N consecutive real viewport frames) vs a static
camera. Metric: temporal second difference |f(t+1) - 2 f(t) + f(t-1)| (8-bit luminance, mean over ROI): a smooth
translation changes the image almost linearly, so this isolates frame-to-frame 'tremble'. The spot window of D3 is
excluded from the ROI. Also reports the fraction of ROI pixels whose second difference exceeds 4/255.
Usage: python d2_move.py [variant ...]"""
import sys, os, time, json, math
import numpy as np
import diag34lib as d

CAM0 = ((-1644.241818024519, 17617.726190341542, 2442.208869304058), (1.4750940536441703, -58.26143420987347, 0.0))
N = int(os.environ.get("D2N", "40")); STEP = float(os.environ.get("D2STEP", "4.0"))
yaw = math.radians(CAM0[1][1]); RIGHT = (-math.sin(yaw) * STEP, math.cos(yaw) * STEP, 0.0)
SPOT = (454, 600, 727, 873)
BASE_CV = {"r.FogMS.Transport.SolveInterval": "2", "r.FogMS.Transport.DirectSamples": "4", "r.VolumetricFog.Jitter": "1",
           "r.VolumetricFog.HistoryWeight": "0.9", "r.Lumen.TranslucencyVolume.Enable": "1", "r.FogMS.ViewIntegration": "0"}
BOXSET = lambda inj, hyb: "b.set_editor_property('emissive_injection', %s); b.set_editor_property('hybrid_single_scattering', %s)" % (inj, hyb)
V = {  # name: (cvars, box assignment, moving)
    "static":      ({}, None, False),
    "move_a":      ({}, None, True),
    "move_b":      ({}, None, True),
    "move_si1":    ({"r.FogMS.Transport.SolveInterval": "1"}, None, True),
    "move_ds8":    ({"r.FogMS.Transport.DirectSamples": "8"}, None, True),
    "static_ds8":  ({"r.FogMS.Transport.DirectSamples": "8"}, None, False),
    "move_boxoff": ({}, "b.set_editor_property('enabled', False)", True),
    "move_jit0":   ({"r.VolumetricFog.Jitter": "0"}, None, True),
    "move_hw0.5":  ({"r.VolumetricFog.HistoryWeight": "0.5"}, None, True),
    "move_lumtv0": ({"r.Lumen.TranslucencyVolume.Enable": "0"}, None, True),
    "move_vi3":    ({"r.FogMS.ViewIntegration": "3"}, None, True),
    "move_inj_hyb": ({}, BOXSET(True, True), True),
    "move_inj_full": ({}, BOXSET(True, False), True),
    "move_inj_hyb_ds8": ({"r.FogMS.Transport.DirectSamples": "8"}, BOXSET(True, True), True),
    "static_inj_hyb": ({}, BOXSET(True, True), False),
    "fast_ds8":    ({"r.FogMS.Transport.DirectSamples": "8"}, None, "fast"),
    "fast_ds8_b":  ({"r.FogMS.Transport.DirectSamples": "8"}, None, "fast"),
    "fast_boxoff": ({"r.FogMS.Transport.DirectSamples": "8"}, "b.set_editor_property('enabled', False)", "fast"),
    "fast_inj_hyb_ds8": ({"r.FogMS.Transport.DirectSamples": "8"}, BOXSET(True, True), "fast"),
    "fast_vi3_ds8": ({"r.FogMS.Transport.DirectSamples": "8", "r.FogMS.ViewIntegration": "3"}, None, "fast"),
    "yaw_ds8":     ({"r.FogMS.Transport.DirectSamples": "8"}, None, "yaw"),
    "yaw_boxoff":  ({"r.FogMS.Transport.DirectSamples": "8"}, "b.set_editor_property('enabled', False)", "yaw"),
    "yaw_inj_hyb_ds8": ({"r.FogMS.Transport.DirectSamples": "8"}, BOXSET(True, True), "yaw"),
    "yaw_cur":     ({}, None, "yaw"),
}
MOVES = {True: RIGHT, "fast": tuple(5 * c for c in RIGHT), "yaw": (0.0, 0.0, 0.0, 0.08)}
REVERT_BOX = BOXSET(False, False) + "\nb.set_editor_property('enabled', True)"

def analyse(name):
    F = d.load(name); L = F.mean(axis=3)
    mask = np.ones(L.shape[1:], bool); y0, y1, x0, x1 = SPOT; mask[y0:y1, x0:x1] = False
    mask[:, :8] = mask[:, -8:] = False
    d2 = np.abs(L[2:] - 2 * L[1:-1] + L[:-2])
    d1 = np.abs(np.diff(L, axis=0))
    return {"d2_roi": round(float(d2[:, mask].mean()), 3), "d2_p99": round(float(np.percentile(d2[:, mask], 99)), 2),
            "frac_d2_gt4": round(float((d2[:, mask] > 4).mean()), 4), "d1_roi": round(float(d1[:, mask].mean()), 3),
            "d2_spot": round(float(d2[:, y0:y1, x0:x1].mean()), 3), "mean": round(float(L[:, mask].mean()), 2)}

if __name__ == "__main__":
    names = sys.argv[1:] or list(V)
    rp = os.path.join(d.M, "d2_results.json"); res = json.load(open(rp)) if os.path.isfile(rp) else {}
    for nm in names:
        cv, box, moving = V[nm]
        d.set_cam(*CAM0)
        for k, v in cv.items(): d.cmd("%s %s" % (k, v))
        st = d.setbox(box) if box else d.status()
        time.sleep(4.0)
        d.capture("d2_" + nm, N, move=MOVES[moving] if moving else None)
        for k in cv: d.cmd("%s %s" % (k, BASE_CV[k]))
        if box: d.setbox(REVERT_BOX); time.sleep(2.0)
        r = analyse("d2_" + nm); r["status"] = st[:140]; res[nm] = r
        print(nm, r, flush=True)
        json.dump(res, open(rp, "w"), indent=1)
    d.set_cam(*CAM0)
