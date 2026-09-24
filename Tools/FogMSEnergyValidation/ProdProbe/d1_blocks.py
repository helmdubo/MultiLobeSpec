# -*- coding: utf-8 -*-
"""D1: Height Fog 'Volumetric Scattering Distribution' g=0.6-0.8 produces black blocky squares inside the cloud.
Owner's camera, 4 consecutive real viewport frames per config (diag34lib.capture), g=0 vs g=0.7 across delivery modes and
engine switches. Metrics: mean luminance; 'dark block' fraction = pixels < 0.35 x their 65-px median neighbourhood
(blocks, not smooth darkening); dominant period (px) of the horizontal-gradient column profile (grid cell size).
Usage: python d1_blocks.py [variant ...]"""
import sys, os, time, json
import numpy as np
import diag34lib as d

BASE_CV = {"r.Lumen.TranslucencyVolume.Enable": "1", "r.VolumetricFog.InjectRaytracedLights": "1",
           "r.Lumen.TranslucencyVolume.GridPixelSize": "32", "r.VolumetricFog.GridPixelSize": "4",
           "r.Lumen.TranslucencyVolume.SpatialFilter": "0", "r.Lumen.TranslucencyVolume.Temporal.Jitter": "0",
           "r.FogMS.Transport.DirectSamples": "4"}
BOXSET = lambda inj, hyb: "b.set_editor_property('emissive_injection', %s); b.set_editor_property('hybrid_single_scattering', %s)" % (inj, hyb)
V = {}
for g in (0.0, 0.7):
    t = "g%.1f" % g
    V["a_cur_" + t] = (g, {}, None)
    V["b_injhyb_" + t] = (g, {}, BOXSET(True, True))
    V["c_injfull_" + t] = (g, {}, BOXSET(True, False))
    V["d_boxoff_" + t] = (g, {}, "b.set_editor_property('enabled', False)")
    V["e_lumtv0_" + t] = (g, {"r.Lumen.TranslucencyVolume.Enable": "0"}, None)
    V["f_rtl0_" + t] = (g, {"r.VolumetricFog.InjectRaytracedLights": "0"}, None)
    V["g_ltvpx16_" + t] = (g, {"r.Lumen.TranslucencyVolume.GridPixelSize": "16"}, None)
    V["h_vfpx8_" + t] = (g, {"r.VolumetricFog.GridPixelSize": "8"}, None)
    V["i_ltvfilter_" + t] = (g, {"r.Lumen.TranslucencyVolume.SpatialFilter": "1", "r.Lumen.TranslucencyVolume.Temporal.Jitter": "1"}, None)
for g in (0.3, 0.5, 0.55, 0.6, 0.8):
    V["s_cur_g%.2f" % g] = (g, {}, None)
V["a_cur_g0.7_rep"] = (0.7, {}, None)
V["d_boxoff_ltvpx16_g0.7"] = (0.7, {"r.Lumen.TranslucencyVolume.GridPixelSize": "16"}, "b.set_editor_property('enabled', False)")
V["d_boxoff_lumtv0_g0.7"] = (0.7, {"r.Lumen.TranslucencyVolume.Enable": "0"}, "b.set_editor_property('enabled', False)")
MODE_OFF = "b.set_editor_property('scattering_mode', unreal.FogMSScatteringMode.OFF)"
for g in (0.0, 0.7):
    t = "g%.1f" % g
    V["j_modeoff_" + t] = (g, {}, MODE_OFF)
    V["k_modeoff_sf1_" + t] = (g, {"r.Lumen.TranslucencyVolume.SpatialFilter": "1"}, MODE_OFF)
    V["l_cur_sf1_" + t] = (g, {"r.Lumen.TranslucencyVolume.SpatialFilter": "1"}, None)
    V["m_cur_tj1_" + t] = (g, {"r.Lumen.TranslucencyVolume.Temporal.Jitter": "1"}, None)
REVERT_BOX = (BOXSET(False, False) + "\nb.set_editor_property('enabled', True)"
              "\nb.set_editor_property('scattering_mode', unreal.FogMSScatteringMode.ANGULAR_TRANSPORT)")

def analyse(name):
    from scipy.ndimage import median_filter
    F = d.load(name); L = F.mean(axis=3); I = L.mean(0)
    med = median_filter(I, size=65)
    dark = (I < 0.35 * med) & (med > 20)
    gx = np.abs(np.diff(I, axis=1)); prof = gx.mean(0); prof = prof - prof.mean()
    sp = np.abs(np.fft.rfft(prof)); f = np.fft.rfftfreq(len(prof))
    sel = (f > 1 / 200) & (f < 1 / 6); k = np.argmax(sp * sel)
    return {"mean": round(float(I.mean()), 2), "dark_frac": round(float(dark.mean()), 4), "grid_period_px": round(1 / f[k], 1),
            "grid_peak_rel": round(float(sp[k] / np.median(sp[sel])), 1)}

if __name__ == "__main__":
    names = sys.argv[1:] or list(V)
    rp = os.path.join(d.M, "d1_results.json"); res = json.load(open(rp)) if os.path.isfile(rp) else {}
    for nm in names:
        g, cv, box = V[nm]
        d.set_g(g)
        for k, v in cv.items(): d.cmd("%s %s" % (k, v))
        st = d.setbox(box) if box else d.status()
        time.sleep(6.0)
        st = d.status()
        d.capture("d1_" + nm, 4)
        for k in cv: d.cmd("%s %s" % (k, BASE_CV[k]))
        if box: d.setbox(REVERT_BOX)
        r = analyse("d1_" + nm); r["status"] = st[:200]; res[nm] = r
        print(nm, r, flush=True)
        json.dump(res, open(rp, "w"), indent=1)
    d.set_g(0.0)
