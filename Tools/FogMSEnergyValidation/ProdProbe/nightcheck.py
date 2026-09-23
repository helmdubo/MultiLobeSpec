# -*- coding: utf-8 -*-
"""Sun-elevation sweep: rotates the atmosphere directional light from its current pitch down below the horizon, taking a
screenshot at each step, and reports the mean brightness inside the Box region vs the sky region. Expected after the round-14
fix: the Box darkens together with the sky as the sun sets (no glow from a below-horizon sun). Restores the light rotation and
the actor state at the end. Usage: python nightcheck.py [pitches, e.g. "-40,-20,-8,-2,2,6,12"]"""
import sys, os, time, json, subprocess, glob, shutil
HERE = os.path.dirname(os.path.abspath(__file__)); M = os.path.join(HERE, "measure", "night"); os.makedirs(M, exist_ok=True)
SHOTS = r"D:/PersonalProjects/UE5/MimirHead_portfolio 5.7 5.8 - 3/Saved/Screenshots/WindowsEditor/*.png"
pitches = [float(x) for x in (sys.argv[1] if len(sys.argv) > 1 else "-40,-20,-8,-2,2,6,12").split(",")]

def py(code):
    r = subprocess.run([sys.executable, os.path.join(HERE, "uemcp.py"), "py", code], capture_output=True, text=True, encoding="utf-8", errors="replace")
    try:
        j = json.loads(r.stdout); return ((j.get("result") or {}).get("output", "") or str(j.get("error", ""))).strip()
    except Exception: return r.stdout[-300:]
def cmd(c): subprocess.run([sys.executable, os.path.join(HERE, "uemcp.py"), "cmd", c], capture_output=True)
SUN = ("L=[a for a in unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors() if isinstance(a, unreal.DirectionalLight)]\n"
       "s=L[0]\n")
info = py("import unreal, json\n" + SUN + "r=s.get_actor_rotation(); print(json.dumps([r.pitch, r.yaw, r.roll]))")
rot0 = json.loads(info.splitlines()[-1]); print("SUN_ROT0", rot0)
subprocess.run([sys.executable, os.path.join(HERE, "boxstate.py"), "save", os.path.join(M, "state.json")], capture_output=True)
py("import unreal\nb=[a for a in unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors() if a.get_actor_label()=='FogMS - Live Box'][0]\nb.set_editor_property('manual_animation_time', 100.0); b.set_editor_property('use_manual_animation_time', True); b.update_density()")
from PIL import Image; import numpy as np
rows = []
for p in pitches:
    py("import unreal\n" + SUN + "s.set_actor_rotation(unreal.Rotator(roll=%f, pitch=%f, yaw=%f), False)" % (rot0[2], p, rot0[1]))
    time.sleep(6)  # native fog history 0.9 + our warm start settle
    before = set(glob.glob(SHOTS)); cmd("HighResShot 1600x900"); time.sleep(4)
    new = sorted(set(glob.glob(SHOTS)) - before)
    if not new: print("pitch %+.0f: no screenshot" % p); continue
    dst = os.path.join(M, "pitch_%+03d.png" % int(p)); shutil.copy(new[-1], dst)
    a = np.asarray(Image.open(dst).convert('RGB'), dtype=np.float64)
    box = a[250:550, 600:1200].mean(); sky = a[0:120, 0:400].mean(); floor = a[820:900, 0:300].mean()
    st = py("import unreal\nb=[a for a in unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors() if a.get_actor_label()=='FogMS - Live Box'][0]\nprint(b.get_editor_property('spatial_status'))")
    rows.append((p, box, sky, floor)); print("pitch %+5.1f | box %6.1f | sky %6.1f | floor %6.1f | %s" % (p, box, sky, floor, st[:60]))
print("--- box/sky ratio by pitch:", ["%+.0f:%.2f" % (p, b / max(s, 1e-3)) for p, b, s, f in rows])
py("import unreal\n" + SUN + "s.set_actor_rotation(unreal.Rotator(roll=%f, pitch=%f, yaw=%f), False); print('SUN_RESTORED')" % (rot0[2], rot0[0], rot0[1]))
subprocess.run([sys.executable, os.path.join(HERE, "boxstate.py"), "restore", os.path.join(M, "state.json")], capture_output=True)
print("DONE")
