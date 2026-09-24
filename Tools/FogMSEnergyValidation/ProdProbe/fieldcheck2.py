# -*- coding: utf-8 -*-
"""Reproduce the delivab session-1 anomaly: camera INSIDE the Box (near its centre), frozen density, clouds hidden,
RTC off. Shots: I1 as-is / I2 Indirect 0 / I3 Indirect 1 / I4 Scattering Mode Off / I5 Transport again / I6 Indirect 0
/ I7 Indirect 1. Then the same first three shots from an outside camera (O1..O3). Restores the viewport camera.
Usage: FOGMS_LOG=<log> python fieldcheck2.py"""
import sys, os, time, subprocess, json, re, shutil
HERE = os.path.dirname(os.path.abspath(__file__)); M = os.path.join(HERE, "measure"); LOG = os.environ.get("FOGMS_LOG", "")
BOX = "b=[a for a in unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors() if a.get_actor_label()=='FogMS - Live Box'][0]"
def py(code):
    r = subprocess.run([sys.executable, os.path.join(HERE, "uemcp.py"), "py", code], capture_output=True, text=True, encoding="utf-8", errors="replace")
    try:
        j = json.loads(r.stdout); return ((j.get("result") or {}).get("output", "") or str(j.get("error", ""))).strip()
    except Exception:
        return r.stdout[-400:]
def cmd(c): subprocess.run([sys.executable, os.path.join(HERE, "uemcp.py"), "cmd", c], capture_output=True)
def logtext(): return open(LOG, encoding="utf-8", errors="replace").read() if LOG and os.path.isfile(LOG) else ""
def shot(name, wait=10.0):
    time.sleep(wait)
    rx = r"High resolution screenshot saved as (.*?\.png)"; before = len(re.findall(rx, logtext()))
    cmd("HighResShot 1600x900")
    for _ in range(20):
        time.sleep(1); m = re.findall(rx, logtext())
        if len(m) > before and os.path.isfile(m[-1].strip()):
            dst = os.path.join(M, "fc2_%s.png" % name); shutil.copy(m[-1].strip(), dst); print("SHOT", name); return dst
    print("SHOT", name, "FAILED"); return None
def status(tag): print(tag, "|", py("import unreal\n" + BOX + "\nprint(b.get_editor_property('spatial_status'))")[:160])
def setp(assign): py("import unreal\n" + BOX + "\n" + assign + "\nb.update_density()")
snap = os.path.join(M, "fieldcheck2_state.json")
print(subprocess.run([sys.executable, os.path.join(HERE, "boxstate.py"), "save", snap], capture_output=True, text=True).stdout.strip()[:120])
print("CAM0 |", py("import unreal\nles=unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem)\nloc,rot=les.get_level_viewport_camera_info()\nprint(repr((loc.x,loc.y,loc.z,rot.pitch,rot.yaw,rot.roll)))"))
cam0 = None
try:
    cam0 = eval(re.search(r"\(([^)]*)\)", py("import unreal\nles=unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem)\nloc,rot=les.get_level_viewport_camera_info()\nprint(repr((loc.x,loc.y,loc.z,rot.pitch,rot.yaw,rot.roll)))")).group(0))
except Exception as e:
    print("cam parse failed", e)
print("CLOUDS |", py("import unreal\neas=unreal.get_editor_subsystem(unreal.EditorActorSubsystem)\nL=[a for a in eas.get_all_level_actors() if isinstance(a, unreal.VolumetricCloud)]\nfor a in L: a.set_is_temporarily_hidden_in_editor(True)\nprint('hidden', len(L))"))
cmd("r.SkyLight.RealTimeReflectionCapture 0"); cmd("r.FogMS.World.Indirect 1")
setp("b.set_editor_property('scattering_mode', unreal.FogMSScatteringMode.ANGULAR_TRANSPORT); b.set_editor_property('transport_preset', unreal.FogMSTransportPreset.PRODUCTION)\n"
     "b.set_editor_property('manual_animation_time', 100.0); b.set_editor_property('use_manual_animation_time', True)\n"
     "b.set_editor_property('emissive_injection', True); b.set_editor_property('hybrid_single_scattering', True)")
# Camera inside the Box: centre shifted 25% of the extent along -X, looking along +X, slightly down.
print("CAM_IN |", py("import unreal\n" + BOX + "\nles=unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem)\no,e=b.get_actor_bounds(False)\np=unreal.Vector(o.x-0.25*e.x, o.y, o.z)\nles.set_level_viewport_camera_info(p, unreal.Rotator(roll=0.0, pitch=-5.0, yaw=0.0))\nprint(p, e)"))
time.sleep(10); status("I1 as-is"); shot("I1_asis")
cmd("r.FogMS.World.Indirect 0"); time.sleep(4); status("I2 indirect 0"); shot("I2_indirect0")
cmd("r.FogMS.World.Indirect 1"); time.sleep(4); status("I3 indirect 1"); shot("I3_indirect1")
setp("b.set_editor_property('scattering_mode', unreal.FogMSScatteringMode.OFF)"); time.sleep(4); status("I4 mode off"); shot("I4_mode_off", 4)
setp("b.set_editor_property('scattering_mode', unreal.FogMSScatteringMode.ANGULAR_TRANSPORT); b.set_editor_property('emissive_injection', True); b.set_editor_property('hybrid_single_scattering', True)")
time.sleep(4); status("I5 mode on"); shot("I5_mode_on")
cmd("r.FogMS.World.Indirect 0"); time.sleep(4); status("I6 indirect 0"); shot("I6_indirect0")
cmd("r.FogMS.World.Indirect 1"); time.sleep(4); status("I7 indirect 1"); shot("I7_indirect1")
# Outside camera: 1.6 extents away along -X, looking at the centre.
print("CAM_OUT |", py("import unreal\n" + BOX + "\nles=unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem)\no,e=b.get_actor_bounds(False)\np=unreal.Vector(o.x-1.6*e.x, o.y, o.z+0.6*e.z)\nles.set_level_viewport_camera_info(p, unreal.Rotator(roll=0.0, pitch=-12.0, yaw=0.0))\nprint(p)"))
time.sleep(8); status("O1 as-is"); shot("O1_asis")
cmd("r.FogMS.World.Indirect 0"); time.sleep(4); status("O2 indirect 0"); shot("O2_indirect0")
cmd("r.FogMS.World.Indirect 1"); time.sleep(4); status("O3 indirect 1"); shot("O3_indirect1")
cmd("r.SkyLight.RealTimeReflectionCapture 1")
if cam0:
    print("CAM restore |", py("import unreal\nles=unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem)\nles.set_level_viewport_camera_info(unreal.Vector(%r,%r,%r), unreal.Rotator(roll=%r, pitch=%r, yaw=%r))\nprint('ok')" % (cam0[0], cam0[1], cam0[2], cam0[5], cam0[3], cam0[4])))
print(subprocess.run([sys.executable, os.path.join(HERE, "boxstate.py"), "restore", snap], capture_output=True, text=True).stdout.strip()[:160])
try:
    from PIL import Image; import numpy as np
    def load(n): return np.asarray(Image.open(os.path.join(M, "fc2_%s.png" % n)).convert('RGB'), dtype=np.float64)[100:800, 200:1400]
    for ref, names in (("I1_asis", ["I2_indirect0", "I3_indirect1", "I4_mode_off", "I5_mode_on", "I6_indirect0", "I7_indirect1"]), ("O1_asis", ["O2_indirect0", "O3_indirect1"])):
        R = load(ref)
        for n in names:
            try:
                X = load(n); mse = ((X - R) ** 2).mean()
                print("%-13s vs %s: PSNR %5.1f dB | mean ratio %.4f | mean abs diff %.2f/255" % (n, ref, 10 * np.log10(255 ** 2 / max(mse, 1e-9)), X.mean() / R.mean(), np.abs(X - R).mean()))
            except Exception as e:
                print(n, "ERR", e)
except Exception as e:
    print("metrics ERR", e)
