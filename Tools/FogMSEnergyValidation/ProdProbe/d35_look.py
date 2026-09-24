# -*- coding: utf-8 -*-
"""Round 35 'look' series for the art lead: current delivery (overlay, fog phase 0) versus the proposed one (Emissive
Injection + hybrid, then feather 2000, then a forward fog phase), same three views as D4 plus his current camera.
4 real viewport frames per config/view (diag34lib.capture), the mean of the last two saved as PNG; labelled contact sheet
results/diag35/look_sheet.png; Box-ROI mean luminance ratios vs L0 in results/diag35/look.json.
Restores the Box snapshot, Height Fog phase and the owner camera at the end. Usage: python d35_look.py"""
import os, json, time
import numpy as np
from PIL import Image, ImageDraw
import diag34lib as d
import diag35lib as L
import d4_density as D4

import sys as _s
TAG = "look_sun" if "sun" in _s.argv else "look"
OUT = os.path.join(d.HERE, "results", "diag35", TAG); os.makedirs(OUT, exist_ok=True)
OWNER = ((-282.70751614825076, 8738.93414348489, 2438.480836716684), (4.075094774365425, 243.93856991827488, 0.0))
import math, sys
def sun_views():
    """Views from the LIVE sun: 'against' = camera on the far side of the cloud, looking toward the sun (sun ~10 deg off
    axis in yaw); 'front' = sun behind the camera. Target and radius as D4."""
    out = d.py("import unreal\n"
               "s=[a for a in unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors() if isinstance(a,unreal.DirectionalLight)][0]\n"
               "v=s.get_actor_forward_vector(); print('FWD %r %r %r' % (v.x, v.y, v.z))")
    fx, fy, fz = [float(x) for x in out.split("FWD")[-1].split()[:3]]
    travel = math.degrees(math.atan2(fy, fx)); elev = math.degrees(math.asin(max(-1.0, min(1.0, -fz))))
    return {"against": D4.view_at(travel + 10.0, 3000.0, 4.0), "front": D4.view_at(travel + 180.0 + 10.0, 3500.0, -2.0)}, travel, elev
if "sun" in sys.argv:
    VIEWS, SUN_TRAVEL, SUN_ELEV = sun_views(); VIEWS = dict(VIEWS, mine=OWNER)
else:
    VIEWS = {"mine": OWNER, "backlit": D4.VIEWS["backlit"], "sunbehind": D4.VIEWS["sunbehind"]}
INJ = "b.set_editor_property('emissive_injection', True); b.set_editor_property('hybrid_single_scattering', True)\n"
F2K = "b.set_editor_property('density_edge_feather', 2000.0)\n"
CFG = [("L0_now_overlay", "", 0.0), ("L1_inj_hyb", INJ, 0.0), ("L2_inj_hyb_f2000", INJ + F2K, 0.0),
       ("L3_inj_hyb_f2000_g06", INJ + F2K, 0.6), ("L4_inj_hyb_f2000_g08", INJ + F2K, 0.8)]
if "sun" in sys.argv: CFG = [CFG[0], CFG[1], CFG[3], CFG[4]]

def frame(name):
    F = d.load(name).astype(np.float32)
    return F[-2:].mean(axis=0)

def main():
    snap = L.snapshot(); g0 = snap["hf"]["volumetric_fog_scattering_distribution"]
    L.set_throttle(False)
    res = {"cfg": {}, "views": {k: [list(v[0]), list(v[1])] for k, v in VIEWS.items()}, "sun": [globals().get("SUN_TRAVEL"), globals().get("SUN_ELEV")]}
    boxoff = {}
    try:
        d.setbox("b.set_editor_property('enabled', False)\n"); time.sleep(3)
        for vn, (loc, rot) in VIEWS.items():
            d.set_cam(loc, rot); time.sleep(4)
            boxoff[vn] = frame(d.capture(TAG + "_boxoff_" + vn, n=4).split(os.sep)[-1])
        d.setbox(L.box_assign(snap["box"])); time.sleep(3)
        for name, assign, g in CFG:
            d.setbox(L.box_assign(snap["box"]))
            if assign: d.setbox(assign)
            d.set_g(g); time.sleep(6)
            st = d.status(); res["cfg"][name] = {"status": st[:200], "g": g}
            for vn, (loc, rot) in VIEWS.items():
                d.set_cam(loc, rot); time.sleep(5)
                img = frame(d.capture(TAG + "_%s_%s" % (name, vn), n=4).split(os.sep)[-1])
                Image.fromarray(np.clip(img, 0, 255).astype(np.uint8)).save(os.path.join(OUT, "%s_%s.png" % (name, vn)))
                lum = img.mean(axis=2); roi = np.abs(lum - boxoff[vn].mean(axis=2)) > 4.0
                res["cfg"][name][vn] = {"mean_roi": round(float(lum[roi].mean()), 2) if roi.any() else None,
                                        "roi_frac": round(float(roi.mean()), 3)}
                print(name, vn, res["cfg"][name][vn], flush=True)
    finally:
        d.setbox(L.box_assign(snap["box"])); d.set_g(g0)
        d.cmd("r.BufferVisualizationDumpFrames 0"); d.cmd("r.DumpingMovie 0"); d.restore_overview()
        d.set_cam(*OWNER); L.set_throttle(snap["throttle"])
    for name in res["cfg"]:
        for vn in VIEWS:
            a, b = res["cfg"][name].get(vn, {}).get("mean_roi"), res["cfg"]["L0_now_overlay"].get(vn, {}).get("mean_roi")
            if a and b: res["cfg"][name][vn]["ratio_vs_L0"] = round(a / b, 3)
    json.dump(res, open(os.path.join(d.HERE, "results", "diag35", TAG + ".json"), "w"), indent=1)
    # Contact sheet: rows = configs, columns = views.
    W, H = 420, 236; LAB = 170
    sheet = Image.new("RGB", (LAB + W * len(VIEWS), 22 + H * len(CFG)), (18, 18, 18)); dr = ImageDraw.Draw(sheet)
    for j, vn in enumerate(VIEWS): dr.text((LAB + j * W + 6, 5), vn, fill=(230, 230, 230))
    for i, (name, _, g) in enumerate(CFG):
        dr.text((6, 22 + i * H + 8), name, fill=(240, 220, 90)); dr.text((6, 22 + i * H + 24), "fog g %.1f" % g, fill=(200, 200, 200))
        for j, vn in enumerate(VIEWS):
            p = os.path.join(OUT, "%s_%s.png" % (name, vn))
            if not os.path.isfile(p): continue
            im = Image.open(p).convert("RGB").resize((W, H)); sheet.paste(im, (LAB + j * W, 22 + i * H))
            r = res["cfg"][name].get(vn, {}).get("ratio_vs_L0")
            if r: ImageDraw.Draw(sheet).text((LAB + j * W + 6, 22 + i * H + H - 16), "lum x%.2f" % r, fill=(255, 255, 0))
    sheet.save(os.path.join(d.HERE, "results", "diag35", TAG + "_sheet.png")); print("SHEET done", flush=True)

if __name__ == "__main__":
    main()
