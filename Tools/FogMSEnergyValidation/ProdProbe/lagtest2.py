# -*- coding: utf-8 -*-
"""Convergence after a light change, measured by the transport residual (FogMS.DumpSpatial json)."""
import sys, os, time, json, subprocess
S = os.path.dirname(os.path.abspath(__file__)); label, light, off_value, quality, iters, warm = sys.argv[1:7]
OUT = os.path.join(S, "measure", "lag2_" + label); os.makedirs(OUT, exist_ok=True)
def br(*a): return subprocess.run([sys.executable, os.path.join(S, "uemcp.py"), *a], capture_output=True, text=True, encoding="utf-8", errors="replace").stdout
def setlight(v): br("py", "import unreal\nfor a in unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors():\n    if a.get_actor_label()=='%s': a.get_component_by_class(unreal.LightComponent).set_intensity(%s)" % (light, v))
def resid(tag):
    p = os.path.join(OUT, tag).replace("\\", "/"); br("cmd", "FogMS.DumpSpatial %s" % p); time.sleep(1.2)
    try: return json.load(open(p + ".json"))["maxRelativeCellResidual"]
    except Exception as e: return None
br("py", "import unreal\nb=[a for a in unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors() if a.get_actor_label()=='FogMS - Live Box'][0]\nb.set_editor_property('angular_quality', unreal.FogMSAngularQuality.%s)\nb.set_editor_property('transport_iterations', %d)" % (quality, int(iters)))
br("cmd", "r.FogMS.Transport.WarmStart %s" % warm)
on_value = {"PointLight": 80000, "FogMS - Sun": 140}[light]
setlight(on_value); time.sleep(6)
rows = [("on_steady", resid("on_steady"))]
t0 = time.time(); setlight(off_value)
for dt in (0.0, 0.5, 1.0, 2.0, 4.0):
    while time.time() - t0 < dt: time.sleep(0.02)
    rows.append(("off_%.1fs" % (time.time() - t0), resid("off_%d" % int(dt * 10))))
time.sleep(4); rows.append(("off_steady", resid("off_steady")))
setlight(on_value); t0 = time.time()
for dt in (0.0, 1.0, 4.0):
    while time.time() - t0 < dt: time.sleep(0.02)
    rows.append(("on_%.1fs" % (time.time() - t0), resid("on_%d" % int(dt * 10))))
print("== %s: light %s -> %s, quality %s, %s it/frame, warm %s" % (label, light, off_value, quality, iters, warm))
for tag, r in rows: print("  %-12s residual %s" % (tag, "n/a" if r is None else "%.5f" % r))
