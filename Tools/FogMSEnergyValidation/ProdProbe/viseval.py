# -*- coding: utf-8 -*-
"""Visual evaluation series for S1 (edge erosion) and S2 (height profiles), editor world, injection+hybrid as authored,
frozen density, the owner's current viewport camera. One HighResShot per configuration into measure/vis_<name>.png.
Usage: FOGMS_LOG=<editor log> python viseval.py     (editor must be free; OnePane layout for HighResShot)"""
import sys, os, time, subprocess, json, re, shutil
HERE = os.path.dirname(os.path.abspath(__file__))
M = os.path.join(HERE, "measure")
LOG = os.environ.get("FOGMS_LOG", "")
BOX = "b=[a for a in unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors() if a.get_actor_label()=='FogMS - Live Box'][0]"

def py(code):
    r = subprocess.run([sys.executable, os.path.join(HERE, "uemcp.py"), "py", code], capture_output=True, text=True, encoding="utf-8", errors="replace")
    try:
        j = json.loads(r.stdout)
        return ((j.get("result") or {}).get("output", "") or str(j.get("error", ""))).strip()
    except Exception:
        return r.stdout[-400:]

def cmd(c):
    subprocess.run([sys.executable, os.path.join(HERE, "uemcp.py"), "cmd", c], capture_output=True)

def shot(name, wait=7.0):
    time.sleep(wait)
    before = len(re.findall(r"High resolution screenshot saved as", open(LOG, encoding="utf-8", errors="replace").read())) if LOG and os.path.isfile(LOG) else 0
    cmd("HighResShot 1600x900")
    src = None
    for _ in range(20):
        time.sleep(1)
        if not LOG or not os.path.isfile(LOG):
            break
        m = re.findall(r"High resolution screenshot saved as (.*?\.png)", open(LOG, encoding="utf-8", errors="replace").read())
        if len(m) > before:
            src = m[-1].strip()
            break
    if src and os.path.isfile(src):
        os.makedirs(M, exist_ok=True)
        dst = os.path.join(M, "vis_%s.png" % name)
        shutil.copy(src, dst)
        print("SHOT", name, "->", dst)
        return dst
    print("SHOT", name, "FAILED (no screenshot in log)")
    return None

def setp(assign, label):
    out = py("import unreal\n" + BOX + "\n" + assign + "\nb.update_density()\nprint('SET %s |', b.get_editor_property('spatial_status'))" % label)
    print(out[:200])

RESET = ("b.set_editor_property('erosion_strength', 0.0); b.set_editor_property('erosion_depth', 0.15)\n"
         "b.set_editor_property('height_profile_preset', unreal.FogMSHeightProfilePreset.NONE); b.set_editor_property('height_profile', False)\n"
         "b.set_editor_property('height_bottom', 0.0); b.set_editor_property('height_top', 1.0); b.set_editor_property('bottom_softness', 0.05); b.set_editor_property('top_softness', 0.1); b.set_editor_property('anvil_strength', 0.0)")

SERIES = [
    ("00_base",            RESET),
    ("01_erosion_0.3",     RESET + "\nb.set_editor_property('erosion_strength', 0.3)"),
    ("02_erosion_0.6",     RESET + "\nb.set_editor_property('erosion_strength', 0.6)"),
    ("03_erosion_0.2",       RESET + "\nb.set_editor_property('erosion_strength', 0.2)"),
    ("04_erosion_0.3_depth_0.3", RESET + "\nb.set_editor_property('erosion_strength', 0.3); b.set_editor_property('erosion_depth', 0.3)"),
    ("10_stratus",         RESET + "\nb.set_editor_property('height_profile_preset', unreal.FogMSHeightProfilePreset.STRATUS)"),
    ("11_cumulus",         RESET + "\nb.set_editor_property('height_profile_preset', unreal.FogMSHeightProfilePreset.CUMULUS)"),
    ("12_cumulonimbus",    RESET + "\nb.set_editor_property('height_profile_preset', unreal.FogMSHeightProfilePreset.CUMULONIMBUS)"),
    ("13_valley_fog",      RESET + "\nb.set_editor_property('height_profile_preset', unreal.FogMSHeightProfilePreset.VALLEY_FOG)"),
    ("20_cumulus_erosion_0.4", RESET + "\nb.set_editor_property('height_profile_preset', unreal.FogMSHeightProfilePreset.CUMULUS); b.set_editor_property('erosion_strength', 0.4)"),
    ("99_base_again",      RESET),
]

snap = os.path.join(M, "viseval_state.json")
print(subprocess.run([sys.executable, os.path.join(HERE, "boxstate.py"), "save", snap], capture_output=True, text=True).stdout.strip()[:160])
print("CAM |", py("import unreal\nles=unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem)\nloc,rot=les.get_level_viewport_camera_info()\nprint(loc, rot)"))
setp("b.set_editor_property('manual_animation_time', 100.0); b.set_editor_property('use_manual_animation_time', True)\n"
     "b.set_editor_property('transport_preset', unreal.FogMSTransportPreset.PRODUCTION); b.set_editor_property('emissive_injection', True); b.set_editor_property('hybrid_single_scattering', True)", "frozen")
cmd("r.SkyLight.RealTimeReflectionCapture 0")
time.sleep(6)
for name, assign in SERIES:
    setp(assign, name)
    shot(name)
cmd("r.SkyLight.RealTimeReflectionCapture 1")
print(subprocess.run([sys.executable, os.path.join(HERE, "boxstate.py"), "restore", snap], capture_output=True, text=True).stdout.strip()[:200])
