# -*- coding: utf-8 -*-
"""Delivery A/B on one camera, frozen density, Production: injection+hybrid / injection full field / overlay (needs
-BindlessAll) / direct-only (r.FogMS.World.Indirect 0) / native only (Scattering Mode Off) / repeat. Per config: status,
the MID's FogMS_InjectionMode read back, HighResShot. Prints pairwise screenshot metrics over the Box crop.
Usage: FOGMS_LOG=<editor log> python delivab.py   (editor free, OnePane)"""
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
def shot(name, wait=9.0):
    time.sleep(wait)
    rx = r"High resolution screenshot saved as (.*?\.png)"
    before = len(re.findall(rx, open(LOG, encoding="utf-8", errors="replace").read())) if LOG and os.path.isfile(LOG) else 0
    cmd("HighResShot 1600x900")
    for _ in range(20):
        time.sleep(1)
        m = re.findall(rx, open(LOG, encoding="utf-8", errors="replace").read()) if LOG and os.path.isfile(LOG) else []
        if len(m) > before:
            src = m[-1].strip()
            if os.path.isfile(src):
                dst = os.path.join(M, "deliv_%s.png" % name); shutil.copy(src, dst); print("SHOT", name); return dst
    print("SHOT", name, "FAILED"); return None
PROBE = ("mode=None\n"
         "for c in b.get_components_by_class(unreal.PrimitiveComponent):\n"
         "    m=c.get_material(0) if c.get_num_materials()>0 else None\n"
         "    if isinstance(m, unreal.MaterialInstanceDynamic):\n"
         "        try: mode=m.get_scalar_parameter_value('FogMS_InjectionMode')\n"
         "        except Exception as e: mode='ERR %s' % e\n"
         "print('MID FogMS_InjectionMode =', mode, '| status:', b.get_editor_property('spatial_status'))")
def setp(assign, label):
    py("import unreal\n" + BOX + "\n" + assign + "\nb.update_density()")
    time.sleep(3)
    print("SET %s | %s" % (label, py("import unreal\n" + BOX + "\n" + PROBE)[:330]))
BASE = ("b.set_editor_property('scattering_mode', unreal.FogMSScatteringMode.ANGULAR_TRANSPORT); b.set_editor_property('transport_preset', unreal.FogMSTransportPreset.PRODUCTION)\n"
        "b.set_editor_property('manual_animation_time', 100.0); b.set_editor_property('use_manual_animation_time', True)\n"
        "b.set_editor_property('erosion_strength', 0.0); b.set_editor_property('height_profile', False); b.set_editor_property('height_profile_preset', unreal.FogMSHeightProfilePreset.NONE)\n")
SERIES = [
    ("A_inj_hybrid",  BASE + "b.set_editor_property('emissive_injection', True); b.set_editor_property('hybrid_single_scattering', True)", None),
    ("B_inj_full",    BASE + "b.set_editor_property('emissive_injection', True); b.set_editor_property('hybrid_single_scattering', False)", None),
    ("C_overlay",     BASE + "b.set_editor_property('emissive_injection', False); b.set_editor_property('hybrid_single_scattering', False)", None),
    ("D_direct_only", BASE + "b.set_editor_property('emissive_injection', True); b.set_editor_property('hybrid_single_scattering', True)", "r.FogMS.World.Indirect 0"),
    ("E_native_only", BASE.replace("ANGULAR_TRANSPORT", "OFF") + "b.set_editor_property('emissive_injection', False)", None),
    ("F_inj_hybrid_again", BASE + "b.set_editor_property('emissive_injection', True); b.set_editor_property('hybrid_single_scattering', True)", None),
]
snap = os.path.join(M, "delivab_state.json")
print(subprocess.run([sys.executable, os.path.join(HERE, "boxstate.py"), "save", snap], capture_output=True, text=True).stdout.strip()[:120])
CLOUDS = ("eas=unreal.get_editor_subsystem(unreal.EditorActorSubsystem)\n"
          "L=[a for a in eas.get_all_level_actors() if isinstance(a, unreal.VolumetricCloud)]\n")
print("CLOUDS |", py("import unreal\n" + CLOUDS + "st=[(a.get_actor_label(), a.is_temporarily_hidden_in_editor()) for a in L]\nfor a in L: a.set_is_temporarily_hidden_in_editor(True)\nprint('hidden', st)"))
cmd("r.SkyLight.RealTimeReflectionCapture 0"); cmd("r.FogMS.World.Indirect 1"); time.sleep(8)
for name, assign, cvar in SERIES:
    if cvar: cmd(cvar)
    setp(assign, name)
    shot(name)
    if cvar: cmd("r.FogMS.World.Indirect 1")
cmd("r.SkyLight.RealTimeReflectionCapture 1")
print(subprocess.run([sys.executable, os.path.join(HERE, "boxstate.py"), "restore", snap], capture_output=True, text=True).stdout.strip()[:160])
try:
    from PIL import Image; import numpy as np
    def load(n): return np.asarray(Image.open(os.path.join(M, "deliv_%s.png" % n)).convert('RGB'), dtype=np.float64)[140:700, 430:1340]
    R = load("A_inj_hybrid")
    for n in ["F_inj_hybrid_again", "B_inj_full", "C_overlay", "D_direct_only", "E_native_only"]:
        try:
            X = load(n); mse = ((X - R) ** 2).mean()
            print("%-20s vs A: PSNR %5.1f dB | mean ratio %.4f | mean abs diff %.2f/255" % (n, 10 * np.log10(255 ** 2 / max(mse, 1e-9)), X.mean() / R.mean(), np.abs(X - R).mean()))
        except Exception as e:
            print(n, "ERR", e)
except Exception as e:
    print("metrics ERR", e)
