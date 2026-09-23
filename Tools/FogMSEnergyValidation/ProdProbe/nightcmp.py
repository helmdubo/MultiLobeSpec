# -*- coding: utf-8 -*-
"""Twilight comparison at one sun pitch (default +8 = sun below the horizon): (A) Box enabled, authored delivery,
(B) Box disabled = native fog lighting of the same authored density, (C) Box enabled with full-field injection,
(D) hybrid injection. Reports mean brightness of the cloud region and of the darkest sky holes for each, so
"our fog glows at night" can be judged against native lighting. Restores sun rotation and actor state.
Usage: python nightcmp.py [pitch]"""
import sys, os, time, json, subprocess, glob, shutil
HERE = os.path.dirname(os.path.abspath(__file__)); M = os.path.join(HERE, "measure", "nightcmp"); os.makedirs(M, exist_ok=True)
SHOTS = r"D:/PersonalProjects/UE5/MimirHead_portfolio 5.7 5.8 - 3/Saved/Screenshots/WindowsEditor/*.png"
pitch = float(sys.argv[1]) if len(sys.argv) > 1 else 8.0
BOX = "b=[a for a in unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors() if a.get_actor_label()=='FogMS - Live Box'][0]\n"
SUN = ("L=[a for a in unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors() if isinstance(a, unreal.DirectionalLight)]\n"
       "s=L[0]\n")

def py(code):
    r = subprocess.run([sys.executable, os.path.join(HERE, "uemcp.py"), "py", code], capture_output=True, text=True, encoding="utf-8", errors="replace")
    try:
        j = json.loads(r.stdout); return ((j.get("result") or {}).get("output", "") or str(j.get("error", ""))).strip()
    except Exception: return r.stdout[-300:]
def cmd(c): subprocess.run([sys.executable, os.path.join(HERE, "uemcp.py"), "cmd", c], capture_output=True)
def shot(name):
    before = set(glob.glob(SHOTS)); cmd("HighResShot 1600x900"); time.sleep(5)
    new = sorted(set(glob.glob(SHOTS)) - before)
    if not new: print(name, "no screenshot"); return None
    dst = os.path.join(M, name + ".png"); shutil.copy(new[-1], dst); return dst

info = py("import unreal, json\n" + SUN + "r=s.get_actor_rotation(); print(json.dumps([r.pitch, r.yaw, r.roll]))")
rot0 = json.loads(info.splitlines()[-1]); print("SUN_ROT0", rot0)
subprocess.run([sys.executable, os.path.join(HERE, "boxstate.py"), "save", os.path.join(M, "state.json")], capture_output=True)
print(py("import unreal\n" + BOX + "b.set_editor_property('manual_animation_time', 100.0); b.set_editor_property('use_manual_animation_time', True); b.set_editor_property('transport_preset', unreal.FogMSTransportPreset.PRODUCTION); b.update_density()\n"
         + "sk=[a for a in unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors() if isinstance(a, unreal.SkyLight)]\n"
         + "c=sk[0].get_component_by_class(unreal.SkyLightComponent) if sk else None\n"
         + "print('SKYLIGHT', c.get_editor_property('real_time_capture') if c else None, c.get_editor_property('source_type') if c else None, c.intensity if c else None, 'mobility', sk[0].get_editor_property('mobility') if sk else None)"))
py("import unreal\n" + SUN + "s.set_actor_rotation(unreal.Rotator(roll=%f, pitch=%f, yaw=%f), False)" % (rot0[2], pitch, rot0[1]))
time.sleep(8)
cases = [("A_box_authored", "b.set_editor_property('enabled', True); b.set_editor_property('emissive_injection', False)"),
         ("B_native_only", "b.set_editor_property('enabled', False)"),
         ("C_box_injection", "b.set_editor_property('enabled', True); b.set_editor_property('emissive_injection', True); b.set_editor_property('hybrid_single_scattering', False)"),
         ("D_box_hybrid", "b.set_editor_property('enabled', True); b.set_editor_property('emissive_injection', True); b.set_editor_property('hybrid_single_scattering', True)")]
from PIL import Image; import numpy as np
res = {}
for name, code in cases:
    st = py("import unreal\n" + BOX + code + "; b.update_density(); print(b.get_editor_property('spatial_status'))")
    time.sleep(7)
    p = shot(name)
    if not p: continue
    a = np.asarray(Image.open(p).convert('RGB'), dtype=np.float64); lum = a.mean(axis=2)
    cloud = np.percentile(lum, 75); dark = np.percentile(lum, 5); mean = lum.mean()
    res[name] = (mean, cloud, dark)
    print("%-16s mean %6.1f | cloud(p75) %6.1f | dark(p5) %6.1f | %s" % (name, mean, cloud, dark, st[:80]))
if "B_native_only" in res:
    b = res["B_native_only"]
    for k, v in res.items():
        print("%-16s vs native: mean x%.3f | cloud x%.3f" % (k, v[0] / max(b[0], 1e-3), v[1] / max(b[1], 1e-3)))
py("import unreal\n" + SUN + "s.set_actor_rotation(unreal.Rotator(roll=%f, pitch=%f, yaw=%f), False); print('SUN_RESTORED')" % (rot0[2], rot0[0], rot0[1]))
py("import unreal\n" + BOX + "b.set_editor_property('enabled', True); b.set_editor_property('hybrid_single_scattering', False); b.update_density()")
subprocess.run([sys.executable, os.path.join(HERE, "boxstate.py"), "restore", os.path.join(M, "state.json")])
print("DONE")
