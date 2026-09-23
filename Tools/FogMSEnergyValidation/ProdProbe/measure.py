# -*- coding: utf-8 -*-
"""Measure one FogMS configuration in the running editor via the UE-MCP bridge.
Usage: python measure.py <label> [mode=ANGULAR_TRANSPORT|TRANSPORT|OFF] [quality=BALANCED48|HIGH96|LOW16|MEDIUM24] [iterations=N] [cvar=...] ...
Steps: apply actor properties + cvars -> wait -> ProfileGPU (log) -> FogMS.DumpSpatial (json metrics) -> HighResShot.
Writes results under <scratch>/measure/<label>/ and prints a one-line summary.
"""
import sys, os, json, time, subprocess, glob, shutil
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "measure")
label = sys.argv[1]
kv = dict(a.split("=", 1) for a in sys.argv[2:] if "=" in a)
cvars = [a.split("=", 1)[1] for a in sys.argv[2:] if a.startswith("cvar=")]
d = os.path.join(OUT, label); os.makedirs(d, exist_ok=True)

def bridge(*args):
    r = subprocess.run([sys.executable, os.path.join(HERE, "uemcp.py"), *args], capture_output=True, text=True, encoding="utf-8", errors="replace")
    return r.stdout

py = ["import unreal",
      "actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()",
      "box = next(a for a in actors if a.get_actor_label() == 'FogMS - Live Box')"]
if "mode" in kv: py.append("box.set_editor_property('scattering_mode', unreal.FogMSScatteringMode.%s)" % kv["mode"])
if "quality" in kv: py.append("box.set_editor_property('angular_quality', unreal.FogMSAngularQuality.%s)" % kv["quality"])
if "iterations" in kv: py.append("box.set_editor_property('transport_iterations', %d)" % int(kv["iterations"]))
py.append("print('APPLIED', box.get_editor_property('scattering_mode'), box.get_editor_property('angular_quality'), box.get_editor_property('transport_iterations'))")
print(bridge("py", "\n".join(py)).count("APPLIED") and "actor props applied" or "actor props: check output")
for c in cvars:
    bridge("cmd", c)
time.sleep(float(kv.get("settle", 4)))

# GPU profile
prof = subprocess.run([sys.executable, os.path.join(HERE, "gpuprofile.py"), label], capture_output=True, text=True, encoding="utf-8", errors="replace").stdout
open(os.path.join(d, "gpuprofile.txt"), "w", encoding="utf-8").write(prof)
transport = next((l for l in prof.splitlines() if "transport/world" in l), "")
frame = next((l for l in prof.splitlines() if "GPU frame" in l), "")

# Transport metrics dump
prefix = os.path.join(d, "dump").replace("\\", "/")
bridge("cmd", "FogMS.DumpSpatial %s" % prefix)
time.sleep(3)
metrics = {}
if os.path.exists(prefix + ".json"):
    try: metrics = json.loads(open(prefix + ".json", encoding="utf-8").read())
    except Exception as e: metrics = {"error": str(e)}
    for f in glob.glob(prefix + ".rgba32f"):
        pass  # keep raw field for later offline comparison
keys = ["iterations", "directions", "maxRelativeCellResidual", "diffuseBalance", "minRadiance", "finite"]
summary = {k: metrics.get(k) for k in keys if k in metrics}
if "balance" in metrics: summary["balance"] = metrics["balance"]

# Screenshot
shots_before = set(glob.glob(r"D:/PersonalProjects/UE5/MimirHead_portfolio 5.7 5.8 - 3/Saved/Screenshots/WindowsEditor/*.png"))
bridge("cmd", "HighResShot 1600x900")
time.sleep(4)
new = set(glob.glob(r"D:/PersonalProjects/UE5/MimirHead_portfolio 5.7 5.8 - 3/Saved/Screenshots/WindowsEditor/*.png")) - shots_before
for s in new: shutil.copy(s, os.path.join(d, "shot.png"))

print("== %s | %s | %s | metrics %s | shot %s" % (label, frame.strip(), transport.strip(), json.dumps(summary), "yes" if new else "no"))
