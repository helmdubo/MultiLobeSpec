# -*- coding: utf-8 -*-
"""Round-35 verification (Part A) at the round-34 D1-D3 camera, frames via diag34lib.capture into measure/diag35.
  a2  : read r.Lumen.TranslucencyVolume.SpatialFilter / Temporal.Jitter, d1 variant a_cur_g0.7 (+ a_cur_g0.0 reference, no
        cvar changes), metrics as round 34 (d1 analyse + band-pass mottle + chroma mottle + grid fold), then fog g back to 0.
  a1  : d3 variants base_a, base_b, ds8, inj_hyb (DirectSamples 4 now alternates the sun only).
  a3  : status hints: Hybrid ticked with injection off; fog g=0.7 with overlay delivery; log warning count.
  a4  : after matedit_density.py: Emissive Injection on, Field Only (Debug) off/on, 4 frames each + status; restore.
  a4b : same with Hybrid ticked (mode 2 vs Field Only mode 3).
Usage: python d35_parta.py a2|a1|a3|a4|a4b"""
import sys, os, time, json, shutil, re
import numpy as np
import diag35lib as L
d = L.d
import d1_blocks, d3_spot

CAM0 = ((-1644.241818024519, 17617.726190341542, 2442.208869304058), (1.4750940536441703, -58.26143420987347, 0.0))
LOG = "E:/GITHUB/MultiLobeSpec/.codex-build/FogMS_Prod_20260922/Main35.log"

def mottle(dirname, M=None):
    from scipy.ndimage import gaussian_filter
    if M: old = d.M; d.M = M
    F = d.load(dirname)
    if M: d.M = old
    Lm = F.mean(axis=3).mean(0)[:540]; C = (F[..., 2] - F[..., 0]).mean(0)[:540]
    bp = gaussian_filter(Lm, 6) - gaussian_filter(Lm, 30); bc = gaussian_filter(C, 6) - gaussian_filter(C, 30)
    def fold(I, p):
        gx = np.abs(np.diff(I, axis=1)).mean(0); fx = np.array([gx[k::p].mean() for k in range(p)]); return round(float(fx.max() / fx.mean()), 3)
    return {"mean": round(float(Lm.mean()), 1), "mottle": round(float(bp.std()), 2), "chroma_mottle": round(float(bc.std()), 2),
            "grid32_x": fold(Lm, 32)}

def logcount(pat):
    try: return len(re.findall(pat, open(LOG, encoding="utf-8", errors="replace").read()))
    except Exception as e: return "ERR %s" % e

def a2():
    res = {"tlv_cvars": L.getcv(["r.Lumen.TranslucencyVolume.SpatialFilter", "r.Lumen.TranslucencyVolume.Temporal.Jitter"])}
    res["log_indirect_preview"] = [l.strip()[:400] for l in open(LOG, encoding="utf-8", errors="replace") if "FogMS indirect preview" in l]
    print(res, flush=True)
    d.set_cam(*CAM0)
    g0 = L.snapshot()["hf"]["volumetric_fog_scattering_distribution"]; res["g_before"] = g0
    for nm in ("a_cur_g0.7", "a_cur_g0.0"):
        g, cv, box = d1_blocks.V[nm]; assert not cv and not box
        d.set_g(g); time.sleep(6.0); st = d.status()
        d.capture("d1_" + nm, 4)
        r = d1_blocks.analyse("d1_" + nm); r.update(mottle("d1_" + nm)); r["status"] = st[:220]; res[nm] = r
        r34 = mottle("d1_" + nm, M=os.path.join(d.HERE, "measure", "diag34")); res[nm + "_r34"] = r34
        print(nm, r, "\n   r34:", r34, flush=True)
    for nm in ("i_ltvfilter_g0.7",):
        res[nm + "_r34"] = mottle("d1_" + nm, M=os.path.join(d.HERE, "measure", "diag34"))
    d.set_g(g0); res["g_after"] = L.snapshot()["hf"]["volumetric_fog_scattering_distribution"]
    json.dump(res, open(os.path.join(L.RES, "a2_blocks.json"), "w"), indent=1); print("g restored", res["g_after"])

def a1():
    d.set_cam(*CAM0)
    res = {}
    for nm in ("base_a", "base_b", "ds8", "inj_hyb"):
        cv, box = d3_spot.V[nm]
        for k, v in cv.items(): d.cmd("%s %s" % (k, v))
        st = d.setbox(box) if box else d.status()
        time.sleep(4.0)
        d.capture("d3_" + nm, 16)
        for k in cv: d.cmd("%s %s" % (k, d3_spot.BASE_CV[k]))
        if box: d.setbox(d3_spot.REVERT_BOX); time.sleep(2.0)
        r = d3_spot.analyse("d3_" + nm); r["status"] = st[:200]; res[nm] = r
        print(nm, {k: v for k, v in r.items() if k != "series"}, "\n   ", r["series"], flush=True)
        json.dump(res, open(os.path.join(L.RES, "a1_spot.json"), "w"), indent=1)
    print("after:", d.status()[:160], L.getcv(["r.FogMS.Transport.DirectSamples"]))

def a3():
    res = {}
    n0 = logcount(r"Hybrid Single Scattering is ignored")
    res["hyb_on"] = d.setbox("b.set_editor_property('hybrid_single_scattering', True)"); time.sleep(3); res["hyb_on_later"] = d.status()
    res["log_warnings_after_on"] = logcount(r"Warning: FogMS .*Hybrid Single Scattering is ignored")
    time.sleep(3); res["log_warnings_after_6s"] = logcount(r"Warning: FogMS .*Hybrid Single Scattering is ignored")
    res["hyb_off"] = d.setbox("b.set_editor_property('hybrid_single_scattering', False)"); time.sleep(2)
    d.set_g(0.7); time.sleep(3); res["g07_status"] = d.status()
    d.set_g(0.0); time.sleep(3); res["g0_status"] = d.status()
    res["log_ignored_lines_total_before"] = n0
    res["log_lines"] = [l.strip()[:300] for l in open(LOG, encoding="utf-8", errors="replace") if "Hybrid Single Scattering is ignored" in l]
    for k, v in res.items(): print(k, ":", v, flush=True)
    json.dump(res, open(os.path.join(L.RES, "a3_status.json"), "w"), indent=1)

def a4():
    res = {}
    d.set_cam(*CAM0)
    res["inj_on"] = d.setbox("b.set_editor_property('emissive_injection', True)"); time.sleep(8)
    res["inj_on_status"] = d.status()
    d.capture("a4_fieldonly_off", 4)
    res["fo_on"] = d.setbox("b.set_editor_property('debug_field_only', True)"); time.sleep(6)
    res["fo_on_status"] = d.status()
    d.capture("a4_fieldonly_on", 4)
    res["fo_off"] = d.setbox("b.set_editor_property('debug_field_only', False)\nb.set_editor_property('emissive_injection', False)"); time.sleep(6)
    res["restored_status"] = d.status()
    for nm in ("a4_fieldonly_off", "a4_fieldonly_on"):
        F = d.load(nm); res[nm + "_mean"] = round(float(F.mean()), 2)
        shutil.copy(os.path.join(d.M, nm, "f003.png"), os.path.join(L.RES, nm + ".png"))
    for k, v in res.items(): print(k, ":", v, flush=True)
    json.dump(res, open(os.path.join(L.RES, "a4_fieldonly.json"), "w"), indent=1)

def a4b():
    """Same as a4 with Hybrid ticked: mode 2 (native single scattering x T_sun + J_ms) vs Field Only (mode 3: full J, native off)."""
    res = json.load(open(os.path.join(L.RES, "a4_fieldonly.json")))
    d.set_cam(*CAM0)
    d.setbox("b.set_editor_property('emissive_injection', True); b.set_editor_property('hybrid_single_scattering', True)"); time.sleep(8)
    res["hyb_status"] = d.status(); d.capture("a4_hyb_fieldonly_off", 4)
    d.setbox("b.set_editor_property('debug_field_only', True)"); time.sleep(6)
    res["hyb_fo_status"] = d.status(); d.capture("a4_hyb_fieldonly_on", 4)
    d.setbox("b.set_editor_property('debug_field_only', False)\nb.set_editor_property('hybrid_single_scattering', False)\n"
             "b.set_editor_property('emissive_injection', False)"); time.sleep(6)
    res["hyb_restored_status"] = d.status()
    A = d.load("a4_hyb_fieldonly_off").mean(0); B = d.load("a4_hyb_fieldonly_on").mean(0); D = np.abs(A - B).mean(axis=2)
    res["hyb_vs_fo_absdiff_mean_p99"] = [round(float(D.mean()), 2), round(float(np.percentile(D, 99)), 1)]
    for nm in ("a4_hyb_fieldonly_off", "a4_hyb_fieldonly_on"):
        shutil.copy(os.path.join(d.M, nm, "f003.png"), os.path.join(L.RES, nm + ".png"))
    for k in ("hyb_status", "hyb_fo_status", "hyb_restored_status", "hyb_vs_fo_absdiff_mean_p99"): print(k, ":", res[k], flush=True)
    json.dump(res, open(os.path.join(L.RES, "a4_fieldonly.json"), "w"), indent=1)

if __name__ == "__main__":
    {"a1": a1, "a2": a2, "a3": a3, "a4": a4, "a4b": a4b}[sys.argv[1]]()
