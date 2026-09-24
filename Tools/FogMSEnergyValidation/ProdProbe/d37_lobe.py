# -*- coding: utf-8 -*-
"""W37 look series: forward lobe (F1a, material FogMS_ForwardLobe) and Sun Softness (F2, transport pass 2) against the W36
default, live sun, same views as d35_look.py sun: 'against' (looking toward the sun past the cloud, sun ~10 deg off axis),
'front' (sun behind the camera), 'mine' (owner camera). The Box must be in Emissive Injection + Hybrid (the W36 default);
the Height Fog phase is left as it is (recorded). Configs (Box properties in memory only, restored at the end):
  W0_w36          Forward Scattering 0, Sun Softness 0 (the W36 image)
  F1_s05_g06      Forward Scattering 0.5, Anisotropy 0.6, Depth 0.5, Back Floor 0.25
  F2_s08_g06      0.8 / 0.6 / 0.5 / 0.25
  F3_s08_g08      0.8 / 0.8 / 0.5 / 0.25
  S1_soft3        Sun Softness 3 deg (lobe off)
  W0_repeat       W0 again: the repeat noise floor for every ratio
6 real viewport frames per config/view (diag34lib.capture): the mean of the last two is the image (PNG), Box ROI = pixels
that differ from the Box-off frame by > 4 (8-bit); per view: ROI mean luminance, ratio vs W0_w36, and 'flicker' = mean
|frame(i) - frame(i-1)| over the ROI for the last 4 frames / ROI mean (solve alternation of DirectSamples 4, F2 criterion
<= 1.5 %). Expected (fwd_lobe_check.py table, multiple-scattering part only): 'against' brighter with the lobe, 'front'
a little darker (Back Floor keeps it >= 1 - s(1 - F) of the MS part), 'mine' depends on the owner camera.
Needs the material patched by matedit_density.py (FogMS_ForwardLobe); aborts otherwise. Writes results/diag37/lobe.json,
results/diag37/lobe_sheet.png, frames in measure/diag37. Restores the Box, the owner camera and the throttle flag.
Usage: python d37_lobe.py   (the lead runs it; nothing here is saved to the level)"""
import os, json, time, math
import numpy as np
from PIL import Image, ImageDraw
import diag34lib as d
import diag35lib as L
import d4_density as D4

RES = os.path.join(d.HERE, "results", "diag37"); OUT = os.path.join(RES, "lobe"); os.makedirs(OUT, exist_ok=True)
d.M = os.path.join(d.HERE, "measure", "diag37"); os.makedirs(d.M, exist_ok=True)
OWNER = ((-282.70751614825076, 8738.93414348489, 2438.480836716684), (4.075094774365425, 243.93856991827488, 0.0))
# W37 properties (not in diag35lib.BOXPROPS): read first, restored at the end.
W37PROPS = ["forward_scattering", "forward_anisotropy", "forward_depth", "back_floor", "sun_softness"]
LOBE = lambda s, g: {"forward_scattering": s, "forward_anisotropy": g, "forward_depth": 0.5, "back_floor": 0.25, "sun_softness": 0.0}
OFF = {"forward_scattering": 0.0, "forward_anisotropy": 0.6, "forward_depth": 0.5, "back_floor": 0.25, "sun_softness": 0.0}
CFG = [("W0_w36", OFF), ("F1_s05_g06", LOBE(0.5, 0.6)), ("F2_s08_g06", LOBE(0.8, 0.6)), ("F3_s08_g08", LOBE(0.8, 0.8)),
       ("S1_soft3", dict(OFF, sun_softness=3.0)), ("W0_repeat", OFF)]
NFRAMES = 6

def sun_views():
    """As d35_look.py: 'against' = far side of the cloud looking toward the sun (~10 deg off in yaw), 'front' = sun behind."""
    out = d.py("import unreal\n"
               "s=[a for a in unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors() if isinstance(a,unreal.DirectionalLight)][0]\n"
               "v=s.get_actor_forward_vector(); print('FWD %r %r %r' % (v.x, v.y, v.z))")
    fx, fy, fz = [float(x) for x in out.split("FWD")[-1].split()[:3]]
    travel = math.degrees(math.atan2(fy, fx)); elev = math.degrees(math.asin(max(-1.0, min(1.0, -fz))))
    return {"against": D4.view_at(travel + 10.0, 3000.0, 4.0), "front": D4.view_at(travel + 180.0 + 10.0, 3500.0, -2.0),
            "mine": OWNER}, travel, elev

def material_has_lobe():
    out = d.py("import unreal\nm=unreal.load_asset('/MultiLobeSpec/FogMS/M_FogMS_Density')\n"
               "n=[e for e in unreal.MaterialEditingLibrary.get_material_expressions(m) if isinstance(e, unreal.MaterialExpressionCustom)"
               " and str(e.get_editor_property('description'))=='FogMS_ForwardLobe']\n"
               "src=unreal.MaterialEditingLibrary.get_material_property_input_node(m, unreal.MaterialProperty.MP_EMISSIVE_COLOR)\n"
               "print('LOBE %d %s' % (len(n), bool(n) and src==n[0]))")
    return "LOBE 1 True" in out

def read_w37():
    out = d.py("import unreal, json\n" + d.BOX + "\nprint('W37 '+json.dumps({p: float(b.get_editor_property(p)) for p in %r}))" % (W37PROPS,))
    return json.loads(out.split("W37 ")[-1].splitlines()[0])

def w37_assign(props):
    return "\n".join("b.set_editor_property(%r, %r)" % (p, float(v)) for p, v in props.items())

def frames(name):
    return d.load(name).astype(np.float32)

def main():
    if not material_has_lobe():
        raise SystemExit("M_FogMS_Density has no FogMS_ForwardLobe feeding Emissive: run matedit_density.py in the editor first.")
    snap = L.snapshot(); w37 = read_w37()
    VIEWS, travel, elev = sun_views()
    res = {"cfg": {}, "views": {k: [list(v[0]), list(v[1])] for k, v in VIEWS.items()}, "sun": [travel, elev],
           "fog_g": snap["hf"]["volumetric_fog_scattering_distribution"], "w37_before": w37, "box_before": snap["box"]}
    if not (snap["box"].get("emissive_injection") and snap["box"].get("hybrid_single_scattering")):
        print("WARNING: the Box is not in Emissive Injection + Hybrid; Forward Scattering is ignored there.", flush=True)
    L.set_throttle(False)
    boxoff = {}
    try:
        d.setbox("b.set_editor_property('enabled', False)\n"); time.sleep(3)
        for vn, (loc, rot) in VIEWS.items():
            d.set_cam(loc, rot); time.sleep(4)
            boxoff[vn] = frames(d.capture("lobe_boxoff_" + vn, n=4).split(os.sep)[-1])[-2:].mean(axis=0)
        d.setbox(L.box_assign(snap["box"])); time.sleep(3)
        for name, props in CFG:
            d.setbox(L.box_assign(snap["box"]) + "\n" + w37_assign(props)); time.sleep(6)
            st = d.status(); res["cfg"][name] = {"props": props, "status": st[:260]}
            print(name, "status:", st[:260], flush=True)
            for vn, (loc, rot) in VIEWS.items():
                d.set_cam(loc, rot); time.sleep(5)
                F = frames(d.capture("lobe_%s_%s" % (name, vn), n=NFRAMES).split(os.sep)[-1])
                img = F[-2:].mean(axis=0)
                Image.fromarray(np.clip(img, 0, 255).astype(np.uint8)).save(os.path.join(OUT, "%s_%s.png" % (name, vn)))
                lum = img.mean(axis=2); roi = np.abs(lum - boxoff[vn].mean(axis=2)) > 4.0
                entry = {"mean_roi": round(float(lum[roi].mean()), 2) if roi.any() else None, "roi_frac": round(float(roi.mean()), 3)}
                if roi.any() and len(F) >= 5:
                    L4 = F[-4:].mean(axis=3)
                    diffs = [float(np.abs(L4[i] - L4[i - 1])[roi].mean()) for i in range(1, len(L4))]
                    entry["flicker"] = round(float(np.mean(diffs)) / max(entry["mean_roi"], 1e-6), 4)
                res["cfg"][name][vn] = entry
                print(name, vn, entry, flush=True)
    finally:
        d.setbox(L.box_assign(snap["box"]) + "\n" + w37_assign(w37))
        d.cmd("r.BufferVisualizationDumpFrames 0"); d.cmd("r.DumpingMovie 0"); d.restore_overview()
        d.set_cam(*OWNER); L.set_throttle(snap["throttle"])
        res["w37_restored"] = read_w37()
    for name in res["cfg"]:
        for vn in VIEWS:
            a, b = res["cfg"][name].get(vn, {}).get("mean_roi"), res["cfg"]["W0_w36"].get(vn, {}).get("mean_roi")
            if a and b: res["cfg"][name][vn]["ratio_vs_W0"] = round(a / b, 3)
    json.dump(res, open(os.path.join(RES, "lobe.json"), "w"), indent=1)
    # Contact sheet: rows = configs, columns = views.
    W, H = 420, 236; LAB = 170
    sheet = Image.new("RGB", (LAB + W * len(VIEWS), 22 + H * len(CFG)), (18, 18, 18)); dr = ImageDraw.Draw(sheet)
    for j, vn in enumerate(VIEWS): dr.text((LAB + j * W + 6, 5), vn, fill=(230, 230, 230))
    for i, (name, props) in enumerate(CFG):
        dr.text((6, 22 + i * H + 8), name, fill=(240, 220, 90))
        dr.text((6, 22 + i * H + 24), "s %.1f g %.1f soft %.0f" % (props["forward_scattering"], props["forward_anisotropy"],
                                                                     props["sun_softness"]), fill=(200, 200, 200))
        for j, vn in enumerate(VIEWS):
            p = os.path.join(OUT, "%s_%s.png" % (name, vn))
            if not os.path.isfile(p): continue
            sheet.paste(Image.open(p).convert("RGB").resize((W, H)), (LAB + j * W, 22 + i * H))
            e = res["cfg"][name].get(vn, {})
            txt = ("lum x%.3f" % e["ratio_vs_W0"] if e.get("ratio_vs_W0") else "") + (
                "  flk %.1f%%" % (100 * e["flicker"]) if e.get("flicker") is not None else "")
            ImageDraw.Draw(sheet).text((LAB + j * W + 6, 22 + i * H + H - 16), txt, fill=(255, 255, 0))
    sheet.save(os.path.join(RES, "lobe_sheet.png"))
    print("SHEET done; W37 props restored:", res["w37_restored"] == w37, flush=True)

if __name__ == "__main__":
    main()
