# -*- coding: utf-8 -*-
"""Stability soak in the running editor: Production preset + Emissive Injection + async late publish, live density animation.
Orbits the viewport camera, toggles the sun intensity, nudges the Box, and polls the actor status. Usage: python soak.py [minutes]
Requires the editor foreground (fg.ps1). Prints status changes and a final summary; check the editor log for ensures afterwards.
Restores camera, sun, Box location and actor settings at the end."""
import sys, time, subprocess, os, math, json
HERE = os.path.dirname(os.path.abspath(__file__))
minutes = float(sys.argv[1]) if len(sys.argv) > 1 else 5.0
BOX = "b=[a for a in unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors() if a.get_actor_label()=='FogMS - Live Box'][0]"
SUN = ("L=[a for a in unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors() if isinstance(a, unreal.DirectionalLight)]\n"
       "c=L[0].get_component_by_class(unreal.DirectionalLightComponent) if L else None\n")

def py(code):
    r = subprocess.run([sys.executable, os.path.join(HERE, "uemcp.py"), "py", code], capture_output=True, text=True, encoding="utf-8", errors="replace")
    try:
        j = json.loads(r.stdout)
        return (j.get("result") or {}).get("output", "") or str(j.get("error", ""))
    except Exception:
        return r.stdout[-300:]

def cmd(c):
    subprocess.run([sys.executable, os.path.join(HERE, "uemcp.py"), "cmd", c], capture_output=True)

info = py("import unreal, json\n" + BOX + "\n" + SUN +
          "loc,rot=unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_level_viewport_camera_info()\n"
          "bl=b.get_actor_location()\n"
          "print(json.dumps([loc.x,loc.y,loc.z,rot.pitch,rot.yaw,rot.roll,bl.x,bl.y,bl.z,c.intensity if c else -1]))")
try:
    cam = json.loads(info.strip().splitlines()[-1])
except Exception:
    print("camera info failed:", info); sys.exit(1)
cx, cy, cz = cam[6], cam[7], cam[8]
radius = math.hypot(cam[0] - cx, cam[1] - cy) or 1500.0
height = cam[2]
sun0 = cam[9]
print("orbit center %.0f %.0f | radius %.0f | height %.0f | sun %.1f" % (cx, cy, radius, height, sun0))
print(py("import unreal\n" + BOX + "\nb.resume_density_animation(); b.set_editor_property('transport_preset', unreal.FogMSTransportPreset.PRODUCTION); b.set_editor_property('emissive_injection', True); b.update_density()\nprint('SETUP', b.get_editor_property('spatial_status'))"))
cmd("r.FogMS.Transport.AsyncCompute 1")
t0 = time.time(); step = 0; statuses = {}
while time.time() - t0 < minutes * 60:
    ang = step * 0.06
    x, y = cx + radius * math.cos(ang), cy + radius * math.sin(ang)
    yaw = math.degrees(math.atan2(cy - y, cx - x))
    code = "import unreal\n" + BOX + "\n"
    code += "unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).set_level_viewport_camera_info(unreal.Vector(%f,%f,%f), unreal.Rotator(-12,%f,0))\n" % (x, y, height, yaw)
    if step % 40 == 20:
        code += "b.set_actor_location(b.get_actor_location()+unreal.Vector(0,0,15), False, False); b.update_density()\n"
    if step % 40 == 30:
        code += "b.set_actor_location(b.get_actor_location()-unreal.Vector(0,0,15), False, False); b.update_density()\n"
    if step % 25 == 10:
        code += SUN + "if c: c.set_intensity(0.0 if c.intensity > 1.0 else %f)\n" % (sun0 if sun0 > 0 else 140.0)
    code += "print('S', b.get_editor_property('spatial_status'))"
    out = py(code)
    s = out.strip().splitlines()[-1] if out.strip() else out
    statuses[s] = statuses.get(s, 0) + 1
    if step % 20 == 0:
        print("t=%4.0fs step %d | %s" % (time.time() - t0, step, s[:150]))
    step += 1
    time.sleep(0.5)
print("--- status histogram")
for s, n in sorted(statuses.items(), key=lambda kv: -kv[1]):
    print("%4d  %s" % (n, s[:160]))
cmd("r.FogMS.Transport.AsyncCompute 0")
print(py("import unreal\n" + BOX + "\n" + SUN +
         "if c: c.set_intensity(%f)\n" % (sun0 if sun0 >= 0 else 140.0) +
         "b.set_actor_location(unreal.Vector(%f,%f,%f), False, False)\n" % (cx, cy, cz) +
         "unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).set_level_viewport_camera_info(unreal.Vector(%f,%f,%f), unreal.Rotator(%f,%f,%f))\n" % tuple(cam[0:6]) +
         "b.set_editor_property('emissive_injection', False); b.set_editor_property('transport_preset', unreal.FogMSTransportPreset.CUSTOM); b.set_editor_property('angular_quality', unreal.FogMSAngularQuality.HIGH96); b.set_editor_property('transport_iterations', 16); b.set_editor_property('transport_tolerance', -1.0); b.update_density()\n"
         "print('RESTORED', b.get_actor_location(), b.get_editor_property('spatial_status'))"))
