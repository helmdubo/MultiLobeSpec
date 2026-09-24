# -*- coding: utf-8 -*-
"""D4: bring back the 22.09 density (strong multiple scattering) as a test, one factor at a time.
Per config: FogMS.DumpSpatial field stats (filled cells, core > 50 % of peak sigma, MS gain Total/Primary, as mediumcheck.py;
works while only one level viewport is realtime and delivery is the overlay) + 4 real viewport frames from three views
(owner camera, back-lit with the sun ~15 deg behind the cloud, sun behind the camera). Mean 8-bit luminance in the Box ROI
(pixels that change when the Box is disabled, per view) and ratio vs C0. Frozen animation (manual time 100), cloud hidden;
RTC stays ON (RTC 0 leaves this scene's real-time sky light without a capture: 'sky: none', image mean 95 -> 39).
Usage: python d4_density.py            (restores the authored density set, the Box snapshot and the camera at the end)"""
import sys, os, time, json, math, subprocess
import numpy as np
import diag34lib as d

OUT = os.path.join(d.HERE, "results", "diag34", "density"); os.makedirs(OUT, exist_ok=True)
CAM0 = ((-1644.241818024519, 17617.726190341542, 2442.208869304058), (1.4750940536441703, -58.26143420987347, 0.0))
TGT = (0.0, 2906.0, 3000.0); R = 18000.0; SUNYAW = 78.279533   # sun light travel yaw (DirectionalLight rotation)
def view_at(yaw_from_target_deg, z, pitch):
    a = math.radians(yaw_from_target_deg); p = (TGT[0] + R * math.cos(a), TGT[1] + R * math.sin(a), z)
    yaw = math.degrees(math.atan2(TGT[1] - p[1], TGT[0] - p[0])); return (p, (pitch, yaw, 0.0))
VIEWS = {"owner": CAM0, "backlit": view_at(SUNYAW + 15.0, 3000.0, 3.0), "sunbehind": view_at(SUNYAW + 180.0, 4000.0, -3.0)}
PROPS = ["density", "threshold", "softness", "detail_strength", "detail_scale", "density_edge_feather", "erosion_strength",
         "height_profile", "emissive_injection", "hybrid_single_scattering"]
OFF = "b.set_editor_property('height_profile', False); b.set_editor_property('height_profile_preset', unreal.FogMSHeightProfilePreset.NONE); b.set_editor_property('erosion_strength', 0.0)\n"
F2K = "b.set_editor_property('density_edge_feather', 2000.0)\n"
TS = "b.set_editor_property('threshold', 0.5); b.set_editor_property('softness', 0.1)\n"
REF = OFF + F2K + TS + "b.set_editor_property('detail_strength', 0.1); b.set_editor_property('detail_scale', 8.0)\n"
INJ = "b.set_editor_property('emissive_injection', True); b.set_editor_property('hybrid_single_scattering', True)\n"
CFG = [("C0_authored", ""), ("C0_rep", ""), ("C1_noprofile", OFF), ("C2_feather2000", OFF + F2K), ("C3_thr05_soft01", OFF + F2K + TS),
       ("C4_2209", REF + "b.set_editor_property('density', 0.5)\n"), ("C5_2209_dens1.6", REF + "b.set_editor_property('density', 1.6)\n"),
       ("C0_inj_hyb", INJ), ("C4_2209_inj_hyb", REF + "b.set_editor_property('density', 0.5)\n" + INJ)]

def dump_stats(tag):
    dd = os.path.join(d.M, "d4dump_" + tag); os.makedirs(dd, exist_ok=True)
    f = os.path.join(dd, "dump.rgba32f")
    if os.path.isfile(f): os.remove(f)
    d.cmd("FogMS.DumpSpatial " + dd.replace("\\", "/") + "/dump"); time.sleep(5)
    if not os.path.isfile(f): return {"dump": "none"}
    n = 32; a = np.fromfile(f, dtype=np.float32).reshape(-1, 4); sig = a[2 * n ** 3:3 * n ** 3, 3]; m = sig > 1e-7
    lum = lambda s: (0.2126 * s[:, 0] + 0.7152 * s[:, 1] + 0.0722 * s[:, 2]); T = lum(a[:n ** 3])[m]; P = lum(a[n ** 3:2 * n ** 3])[m]
    return {"filled": int(m.sum()), "core50": int((sig > 0.5 * sig.max()).sum()), "sigma_p50": float(np.percentile(sig[m], 50)),
            "sigma_max": float(sig.max()), "J_p50": float(np.percentile(T, 50)), "ms_gain": float(T.mean() / max(P.mean(), 1e-9))}

def lumimg(name):
    F = d.load(name); return (0.2126 * F[..., 0] + 0.7152 * F[..., 1] + 0.0722 * F[..., 2]).mean(0)

if __name__ == "__main__":
    snap = os.path.join(d.M, "d4_boxstate.json")
    print(subprocess.run([sys.executable, os.path.join(d.HERE, "boxstate.py"), "save", snap], capture_output=True, text=True).stdout.strip()[:100])
    auth = json.loads(d.py("import unreal, json\n" + d.BOX + "\nprint(json.dumps({p: (float(v) if isinstance(v, float) else v) for p, v in ((p, b.get_editor_property(p)) for p in %r)}))" % PROPS).splitlines()[-1])
    json.dump(auth, open(os.path.join(d.M, "d4_authored.json"), "w"), indent=1)
    sun = d.py("import unreal\n" + d.ACT + "\ns=A['DirectionalLight']; c=s.get_component_by_class(unreal.DirectionalLightComponent)\nprint('SUN rot', s.get_actor_rotation(), 'intensity', c.get_editor_property('intensity'))")
    print("AUTHORED", auth, "\n", sun)
    pre = int(json.load(open(snap)).get("height_profile_preset", 0))
    RESTORE = "b.set_editor_property('height_profile_preset', unreal.FogMSHeightProfilePreset.cast(%d))\n" % pre + "\n".join("b.set_editor_property(%r, %r)" % (p, v) for p, v in auth.items())
    FREEZE = "b.set_editor_property('manual_animation_time', 100.0); b.set_editor_property('use_manual_animation_time', True)\n"
    res = {"authored": auth, "sun": sun, "views": {k: [list(v[0]), list(v[1])] for k, v in VIEWS.items()}, "cfg": {}}
    # Box ROI per view: pixels that change when the Box is disabled (authored set)
    d.setbox(FREEZE); roi = {}
    for vn, (loc, rot) in VIEWS.items():
        d.set_cam(loc, rot); time.sleep(3); d.capture("d4_%s_on" % vn, 4)
        d.setbox("b.set_editor_property('enabled', False)"); time.sleep(4); d.capture("d4_%s_boxoff" % vn, 4)
        d.setbox("b.set_editor_property('enabled', True)"); time.sleep(6)
        roi[vn] = np.abs(lumimg("d4_%s_on" % vn) - lumimg("d4_%s_boxoff" % vn)) > 3.0
        print("ROI", vn, "fraction", round(float(roi[vn].mean()), 3), flush=True)
    for tag, assign in CFG:
        d.setbox(RESTORE + "\n" + FREEZE + assign); time.sleep(10)
        st = d.status(); r = {"status": st[:220]}
        if "emissive_injection', True" not in assign: r.update(dump_stats(tag))
        for vn, (loc, rot) in VIEWS.items():
            d.set_cam(loc, rot); time.sleep(3)
            nm = "d4_%s_%s" % (vn, tag); d.capture(nm, 4)
            L = lumimg(nm); r["lum_" + vn] = round(float(L[roi[vn]].mean()), 2)
            import shutil; shutil.copy(os.path.join(d.M, nm, "f000.png"), os.path.join(OUT, "%s__%s.png" % (tag, vn)))
        res["cfg"][tag] = r
        print(tag, {k: (round(v, 5) if isinstance(v, float) else v) for k, v in r.items() if k != "status"}, "|", st[:90], flush=True)
        json.dump(res, open(os.path.join(d.M, "d4_results.json"), "w"), indent=1)
    d.setbox(RESTORE)
    print(subprocess.run([sys.executable, os.path.join(d.HERE, "boxstate.py"), "restore", snap], capture_output=True, text=True).stdout.strip()[:200])
    print("AFTER", d.py("import unreal\n" + d.BOX + "\nprint({p: str(b.get_editor_property(p)) for p in %r})" % PROPS))
    d.set_cam(*CAM0)
