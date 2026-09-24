# -*- coding: utf-8 -*-
"""Round 36 sky / god-rays test, no plugin code, nothing saved: what the engine's own Volumetric Cloud + cloud shadows +
volumetric fog phase give (RDR2-style shafts under clouds, cloud shadows on the ground), and whether the FogMS Box reacts.
Configs change the sun (cast cloud shadows, shadows on atmosphere), the Volumetric Cloud layer altitude/height and the
Height Fog (phase, haze density) in memory only; every property is read first and restored at the end.
Views from the LIVE sun: 'against' (looking toward the sun past the Box), 'ground' (high camera looking down-sun at the
terrain, sun behind), 'mine' (owner camera). 4 frames per view, mean of the last two. Sheet: results/diag36/sky_sheet.png.
Usage: python d36_sky.py"""
import os, json, time, math
import numpy as np
from PIL import Image, ImageDraw
import diag34lib as d
import diag35lib as L
import d4_density as D4

RES = os.path.join(d.HERE, "results", "diag36"); OUT = os.path.join(RES, "sky"); os.makedirs(OUT, exist_ok=True)
OWNER = ((-282.70751614825076, 8738.93414348489, 2438.480836716684), (4.075094774365425, 243.93856991827488, 0.0))
FIND = ("import unreal\nA=unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()\n"
        "sun=[a for a in A if a.get_class().get_name()=='DirectionalLight'][0].light_component\n"
        "vc=[a for a in A if a.get_class().get_name()=='VolumetricCloud'][0].get_component_by_class(unreal.VolumetricCloudComponent)\n"
        "hf=[a for a in A if a.get_class().get_name()=='ExponentialHeightFog'][0].component\n")
PROPS = {"sun": ["cast_cloud_shadows", "cast_shadows_on_atmosphere", "cloud_shadow_strength", "cloud_shadow_on_atmosphere_strength",
                 "cloud_shadow_on_surface_strength"],
         "vc": ["layer_bottom_altitude", "layer_height"],
         "hf": ["volumetric_fog_scattering_distribution", "fog_density"]}

def read_props():
    code = FIND + "import json\nout={}\n"
    for obj, names in PROPS.items():
        code += "out[%r]={n: %s.get_editor_property(n) for n in %r}\n" % (obj, obj, names)
    code += "print('PROPS '+json.dumps(out))"
    return json.loads(d.py(code).split("PROPS ")[-1].splitlines()[0])

def set_props(p):
    code = FIND
    for obj, vals in p.items():
        for n, v in vals.items(): code += "%s.set_editor_property(%r, %r)\n" % (obj, n, v)
    code += "print('SET ok')"
    return d.py(code)

def views():
    out = d.py("import unreal\ns=[a for a in unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors() "
               "if isinstance(a,unreal.DirectionalLight)][0]\nv=s.get_actor_forward_vector(); print('FWD %r %r %r' % (v.x, v.y, v.z))")
    fx, fy, fz = [float(x) for x in out.split("FWD")[-1].split()[:3]]
    travel = math.degrees(math.atan2(fy, fx))
    against = D4.view_at(travel + 10.0, 2500.0, 8.0)
    # High camera up-sun of the target, looking down-sun (sun behind) at the terrain.
    a = math.radians(travel + 180.0); gp = (D4.TGT[0] + 22000 * math.cos(a), D4.TGT[1] + 22000 * math.sin(a), 9000.0)
    ground = (gp, (-18.0, travel, 0.0))
    return {"against": against, "ground": ground, "mine": OWNER}, travel

SHADOWS = {"sun": {"cast_cloud_shadows": True, "cast_shadows_on_atmosphere": True, "cloud_shadow_strength": 1.0,
                   "cloud_shadow_on_atmosphere_strength": 1.0, "cloud_shadow_on_surface_strength": 1.0}}
LOW = {"vc": {"layer_bottom_altitude": 1.5, "layer_height": 3.0}}
def merge(*ps):
    out = {}
    for p in ps:
        for k, v in p.items(): out.setdefault(k, {}).update(v)
    return out
CFG = [("S0_now", {}), ("S1_cloud_shadows", SHADOWS), ("S2_shadows_low_clouds", merge(SHADOWS, LOW)),
       ("S3_+fog_g07", merge(SHADOWS, LOW, {"hf": {"volumetric_fog_scattering_distribution": 0.7}})),
       ("S4_+haze_x3", merge(SHADOWS, LOW, {"hf": {"volumetric_fog_scattering_distribution": 0.7}}, {"hf": {"fog_density": None}}))]

def frame(name):
    return d.load(name).astype(np.float32)[-2:].mean(axis=0)

def main():
    orig = read_props(); snap = L.snapshot(); V, travel = views()
    CFG[-1][1]["hf"]["fog_density"] = float(orig["hf"]["fog_density"]) * 3.0
    L.set_throttle(False); res = {"orig": orig, "sun_travel_yaw": travel, "views": {k: [list(v[0]), list(v[1])] for k, v in V.items()}, "cfg": {}}
    try:
        for name, p in CFG:
            set_props(orig);
            if p: set_props(p)
            time.sleep(8)
            res["cfg"][name] = {"props": p, "status": d.status()[:200]}
            for vn, (loc, rot) in V.items():
                d.set_cam(loc, rot); time.sleep(6)
                img = frame(d.capture("sky_%s_%s" % (name, vn), n=4).split(os.sep)[-1])
                Image.fromarray(np.clip(img, 0, 255).astype(np.uint8)).save(os.path.join(OUT, "%s_%s.png" % (name, vn)))
                res["cfg"][name][vn] = {"mean": round(float(img.mean()), 2)}
                print(name, vn, res["cfg"][name][vn], flush=True)
    finally:
        set_props(orig); d.cmd("r.BufferVisualizationDumpFrames 0"); d.cmd("r.DumpingMovie 0"); d.restore_overview()
        d.set_cam(*OWNER); L.set_throttle(snap["throttle"])
        res["restored"] = read_props()
    json.dump(res, open(os.path.join(RES, "sky.json"), "w"), indent=1, default=str)
    W, H = 420, 236; LAB = 190
    sheet = Image.new("RGB", (LAB + W * len(V), 22 + H * len(CFG)), (18, 18, 18)); dr = ImageDraw.Draw(sheet)
    for j, vn in enumerate(V): dr.text((LAB + j * W + 6, 5), vn, fill=(230, 230, 230))
    for i, (name, _) in enumerate(CFG):
        dr.text((6, 22 + i * H + 8), name, fill=(240, 220, 90))
        for j, vn in enumerate(V):
            pth = os.path.join(OUT, "%s_%s.png" % (name, vn))
            if os.path.isfile(pth): sheet.paste(Image.open(pth).convert("RGB").resize((W, H)), (LAB + j * W, 22 + i * H))
    sheet.save(os.path.join(RES, "sky_sheet.png")); print("SHEET done; restored", res["restored"] == orig, flush=True)

if __name__ == "__main__":
    main()
