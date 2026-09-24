# -*- coding: utf-8 -*-
"""Is the injected field actually visible? Same camera, frozen density, clouds hidden, RTC off. Steps with a screenshot
after each: S1 as-is (injection+hybrid) / S2 injection off->on / S3 Scattering Mode Off->Transport (the E->F cycle of
delivab) / S4 hybrid off / S5 hybrid on / S6 Indirect 0 / S7 Indirect 1. Also FogMS.DumpSpatial at S1 and S3 (BindlessAll)
and fielddiff between them. Usage: FOGMS_LOG=<log> python fieldcheck.py"""
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
            dst = os.path.join(M, "fc_%s.png" % name); shutil.copy(m[-1].strip(), dst); print("SHOT", name); return dst
    print("SHOT", name, "FAILED"); return None
def status(tag): print(tag, "|", py("import unreal\n" + BOX + "\nprint(b.get_editor_property('spatial_status'))")[:200])
def dump(name):
    d = os.path.join(M, "fc_dump_" + name); os.makedirs(d, exist_ok=True)
    cmd("FogMS.DumpSpatial " + d.replace("\\", "/") + "/dump"); time.sleep(4)
    print("DUMP", name, os.path.isfile(os.path.join(d, "dump.rgba32f")))
def setp(assign):
    py("import unreal\n" + BOX + "\n" + assign + "\nb.update_density()")
snap = os.path.join(M, "fieldcheck_state.json")
print(subprocess.run([sys.executable, os.path.join(HERE, "boxstate.py"), "save", snap], capture_output=True, text=True).stdout.strip()[:120])
print("CLOUDS |", py("import unreal\neas=unreal.get_editor_subsystem(unreal.EditorActorSubsystem)\nL=[a for a in eas.get_all_level_actors() if isinstance(a, unreal.VolumetricCloud)]\nfor a in L: a.set_is_temporarily_hidden_in_editor(True)\nprint('hidden', len(L))"))
cmd("r.SkyLight.RealTimeReflectionCapture 0"); cmd("r.FogMS.World.Indirect 1")
setp("b.set_editor_property('scattering_mode', unreal.FogMSScatteringMode.ANGULAR_TRANSPORT); b.set_editor_property('transport_preset', unreal.FogMSTransportPreset.PRODUCTION)\n"
     "b.set_editor_property('manual_animation_time', 100.0); b.set_editor_property('use_manual_animation_time', True)\n"
     "b.set_editor_property('emissive_injection', True); b.set_editor_property('hybrid_single_scattering', True)")
time.sleep(8); status("S1 as-is"); shot("S1_asis"); dump("S1")
setp("b.set_editor_property('emissive_injection', False)"); time.sleep(4); status("S2a inj off"); shot("S2a_inj_off", 4)
setp("b.set_editor_property('emissive_injection', True)"); time.sleep(4); status("S2b inj on"); shot("S2b_inj_on")
setp("b.set_editor_property('scattering_mode', unreal.FogMSScatteringMode.OFF)"); time.sleep(4); status("S3a mode off"); shot("S3a_mode_off", 4)
setp("b.set_editor_property('scattering_mode', unreal.FogMSScatteringMode.ANGULAR_TRANSPORT); b.set_editor_property('emissive_injection', True); b.set_editor_property('hybrid_single_scattering', True)")
time.sleep(4); status("S3b mode on"); shot("S3b_mode_on"); dump("S3")
setp("b.set_editor_property('hybrid_single_scattering', False)"); time.sleep(4); status("S4 hybrid off"); shot("S4_hybrid_off")
setp("b.set_editor_property('hybrid_single_scattering', True)"); time.sleep(4); status("S5 hybrid on"); shot("S5_hybrid_on")
cmd("r.FogMS.World.Indirect 0"); time.sleep(4); status("S6 indirect 0"); shot("S6_indirect0")
cmd("r.FogMS.World.Indirect 1"); time.sleep(4); status("S7 indirect 1"); shot("S7_indirect1")
print("LOG plugin lines during the run:")
for l in [l for l in logtext().splitlines() if ("LogMultiLobeSpec" in l or "LogFogMSTransport" in l) and "define" not in l and "patched" not in l][-30:]:
    print("   ", l[30:230])
cmd("r.SkyLight.RealTimeReflectionCapture 1")
print(subprocess.run([sys.executable, os.path.join(HERE, "boxstate.py"), "restore", snap], capture_output=True, text=True).stdout.strip()[:160])
try:
    from PIL import Image; import numpy as np
    def load(n): return np.asarray(Image.open(os.path.join(M, "fc_%s.png" % n)).convert('RGB'), dtype=np.float64)[140:700, 430:1340]
    R = load("S1_asis")
    for n in ["S2a_inj_off", "S2b_inj_on", "S3a_mode_off", "S3b_mode_on", "S4_hybrid_off", "S5_hybrid_on", "S6_indirect0", "S7_indirect1"]:
        try:
            X = load(n); mse = ((X - R) ** 2).mean()
            print("%-14s vs S1: PSNR %5.1f dB | mean ratio %.4f | mean abs diff %.2f/255" % (n, 10 * np.log10(255 ** 2 / max(mse, 1e-9)), X.mean() / R.mean(), np.abs(X - R).mean()))
        except Exception as e:
            print(n, "ERR", e)
    a, b3 = os.path.join(M, "fc_dump_S1", "dump.rgba32f"), os.path.join(M, "fc_dump_S3", "dump.rgba32f")
    if os.path.isfile(a) and os.path.isfile(b3):
        print("FIELD S3 vs S1:", subprocess.run([sys.executable, os.path.join(HERE, "fielddiff.py"), b3, a], capture_output=True, text=True).stdout.strip()[:200])
except Exception as e:
    print("metrics ERR", e)
