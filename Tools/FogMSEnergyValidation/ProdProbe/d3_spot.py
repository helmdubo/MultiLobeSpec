# -*- coding: utf-8 -*-
"""D3: spot-light flicker inside the cloud. Static camera (the owner's), N consecutive real viewport frames per variant
(diag34lib.capture), time series of the mean luminance in the spot-glow window, amplitude and dominant period in frames.
Usage: python d3_spot.py [variant ...]   (default: all). Each variant sets its state, waits, captures, and reverts."""
import sys, os, time, json
import numpy as np
import diag34lib as d

WIN = (454, 600, 727, 873)   # y0,y1,x0,x1: spot glow window found in the first static capture (1117x813 pane)
N = int(os.environ.get("D3N", "16"))
BASE_CV = {"r.FogMS.Transport.SolveInterval": "2", "r.VolumetricFog.InjectRaytracedLights": "1", "r.FogMS.MaxBoxesPerFrame": "4",
           "r.VolumetricFog.Jitter": "1", "r.VolumetricFog.HistoryWeight": "0.9", "r.Lumen.TranslucencyVolume.Enable": "1"}
BOXSET = lambda inj, hyb: "b.set_editor_property('emissive_injection', %s); b.set_editor_property('hybrid_single_scattering', %s)" % (inj, hyb)
V = {
    "base_a":     ({}, None),
    "base_b":     ({}, None),
    "si1":        ({"r.FogMS.Transport.SolveInterval": "1"}, None),
    "si4":        ({"r.FogMS.Transport.SolveInterval": "4"}, None),
    "boxoff":     ({}, "b.set_editor_property('enabled', False)"),
    "rtl0":       ({"r.VolumetricFog.InjectRaytracedLights": "0"}, None),
    "mbpf1":      ({"r.FogMS.MaxBoxesPerFrame": "1"}, None),
    "jit0":       ({"r.VolumetricFog.Jitter": "0"}, None),
    "hw0":        ({"r.VolumetricFog.HistoryWeight": "0"}, None),
    "lumtv0":     ({"r.Lumen.TranslucencyVolume.Enable": "0"}, None),
    "inj_hyb":    ({}, BOXSET(True, True)),
    "inj_full":   ({}, BOXSET(True, False)),
    "inj_hyb_si1": ({"r.FogMS.Transport.SolveInterval": "1"}, BOXSET(True, True)),
    "ds8":        ({"r.FogMS.Transport.DirectSamples": "8"}, None),
    "ds8_si1":    ({"r.FogMS.Transport.DirectSamples": "8", "r.FogMS.Transport.SolveInterval": "1"}, None),
    "inj_hyb_ds8": ({"r.FogMS.Transport.DirectSamples": "8"}, BOXSET(True, True)),
    "base_c":     ({}, None),
    "rtc1_base":  ({}, None),
    "rtc1_ds8":   ({"r.FogMS.Transport.DirectSamples": "8"}, None),
}
BASE_CV["r.FogMS.Transport.DirectSamples"] = "4"
REVERT_BOX = BOXSET(False, False) + "\nb.set_editor_property('enabled', True)"

def analyse(name):
    F = d.load(name); L = F.mean(axis=3)
    y0, y1, x0, x1 = WIN
    s = L[:, y0:y1, x0:x1].mean(axis=(1, 2)); g = L.mean(axis=(1, 2))
    ac = s - s.mean()
    spec = np.abs(np.fft.rfft(ac)); k = int(np.argmax(spec[1:]) + 1) if len(spec) > 1 else 0
    period = len(s) / k if k else 0
    d2 = np.abs(np.diff(L, axis=0))[:, y0:y1, x0:x1].mean()
    return {"spot_mean": round(float(s.mean()), 2), "spot_pp": round(float(s.max() - s.min()), 2),
            "spot_rel_std": round(float(s.std() / max(s.mean(), 1e-6)), 4), "period_frames": round(period, 2),
            "frame_absdiff_win": round(float(d2), 3), "glob_pp": round(float(g.max() - g.min()), 3),
            "series": [round(float(x), 1) for x in s]}

if __name__ == "__main__":
    names = sys.argv[1:] or list(V)
    res = {}
    rp = os.path.join(d.M, "d3_results.json")
    if os.path.isfile(rp): res = json.load(open(rp))
    for nm in names:
        cv, box = V[nm]
        for k, v in cv.items(): d.cmd("%s %s" % (k, v))
        st = d.setbox(box) if box else d.status()
        time.sleep(4.0)
        d.capture("d3_" + nm, N)
        for k in cv: d.cmd("%s %s" % (k, BASE_CV[k]))
        if box: d.setbox(REVERT_BOX); time.sleep(2.0)
        r = analyse("d3_" + nm); r["status"] = st[:160]; res[nm] = r
        print(nm, {k: v for k, v in r.items() if k != "series"}, "\n   ", r["series"], flush=True)
        json.dump(res, open(rp, "w"), indent=1)
