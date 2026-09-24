# -*- coding: utf-8 -*-
"""How thick is the medium the solver actually sees, and how much multiple scattering it produces (Total/Primary), for the
authored Box and with the thinning parameters removed one by one. Frozen density, RTC off, clouds hidden. Dumps need
OnePane + -BindlessAll. Usage: FOGMS_LOG=<log> python mediumcheck.py"""
import sys, os, time, subprocess, json
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__)); M = os.path.join(HERE, "measure")
BOX = "b=[a for a in unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors() if a.get_actor_label()=='FogMS - Live Box'][0]"
def py(code):
    r = subprocess.run([sys.executable, os.path.join(HERE, "uemcp.py"), "py", code], capture_output=True, text=True, encoding="utf-8", errors="replace")
    try:
        j = json.loads(r.stdout); return ((j.get("result") or {}).get("output", "") or str(j.get("error", ""))).strip()
    except Exception:
        return r.stdout[-400:]
def cmd(c): subprocess.run([sys.executable, os.path.join(HERE, "uemcp.py"), "cmd", c], capture_output=True)
def stats(tag):
    d = os.path.join(M, "med_" + tag); os.makedirs(d, exist_ok=True)
    cmd("FogMS.DumpSpatial " + d.replace("\\", "/") + "/dump"); time.sleep(4)
    f = os.path.join(d, "dump.rgba32f")
    if not os.path.isfile(f):
        print("%-12s NO DUMP %s" % (tag, open(os.path.join(d, "dump.json")).read()[:120] if os.path.isfile(os.path.join(d, "dump.json")) else "")); return
    n = 32; a = np.fromfile(f, dtype=np.float32).reshape(-1, 4); sig = a[2 * n ** 3:3 * n ** 3, 3]; m = sig > 1e-7
    lum = lambda s: (0.2126 * s[:, 0] + 0.7152 * s[:, 1] + 0.0722 * s[:, 2]); T = lum(a[:n ** 3])[m]; P = lum(a[n ** 3:2 * n ** 3])[m]
    core = sig > 0.5 * sig.max()
    print("%-12s filled %5d (%.0f%%) core>50%%peak %4d | sigma/cm p50 %.5f p90 %.5f max %.5f | J p50 %.2f | MS gain Total/Primary %.2f" % (
        tag, m.sum(), 100.0 * m.sum() / n ** 3, core.sum(), *np.percentile(sig[m], [50, 90, 100]), np.percentile(T, 50), T.mean() / max(P.mean(), 1e-9)))
def run(tag, assign):
    py("import unreal\n" + BOX + "\nb.set_editor_property('manual_animation_time', 100.0); b.set_editor_property('use_manual_animation_time', True)\n" + assign + "\nb.update_density()")
    time.sleep(9); stats(tag)
snap = os.path.join(M, "medium_state.json")
print(subprocess.run([sys.executable, os.path.join(HERE, "boxstate.py"), "save", snap], capture_output=True, text=True).stdout.strip()[:100])
print("AUTHORED |", py("import unreal\n" + BOX + "\nprint({p: str(b.get_editor_property(p)) for p in ('density','threshold','softness','detail_strength','detail_scale','density_edge_feather','erosion_strength','height_profile','height_profile_preset','emissive_injection','hybrid_single_scattering')})"))
print("CLOUDS |", py("import unreal\neas=unreal.get_editor_subsystem(unreal.EditorActorSubsystem)\nL=[a for a in eas.get_all_level_actors() if isinstance(a, unreal.VolumetricCloud)]\nfor a in L: a.set_is_temporarily_hidden_in_editor(True)\nprint('hidden', len(L))"))
cmd("r.SkyLight.RealTimeReflectionCapture 0"); time.sleep(6)
OFF = "b.set_editor_property('height_profile', False); b.set_editor_property('height_profile_preset', unreal.FogMSHeightProfilePreset.NONE); b.set_editor_property('erosion_strength', 0.0)\n"
run("authored", "b.set_editor_property('emissive_injection', False)")  # overlay delivery: the dump needs the resident atlas
run("noprofile", OFF + "b.set_editor_property('emissive_injection', False)")
run("feather500", OFF + "b.set_editor_property('density_edge_feather', 500.0); b.set_editor_property('emissive_injection', False)")
run("thr045", OFF + "b.set_editor_property('density_edge_feather', 500.0); b.set_editor_property('threshold', 0.45); b.set_editor_property('emissive_injection', False)")
run("detail0", OFF + "b.set_editor_property('density_edge_feather', 500.0); b.set_editor_property('threshold', 0.45); b.set_editor_property('detail_strength', 0.0); b.set_editor_property('emissive_injection', False)")
run("dens05", OFF + "b.set_editor_property('density_edge_feather', 500.0); b.set_editor_property('threshold', 0.45); b.set_editor_property('detail_strength', 0.0); b.set_editor_property('density', 0.5); b.set_editor_property('emissive_injection', False)")
py("import unreal\n" + BOX + "\nb.set_editor_property('density_edge_feather', 6000.0); b.set_editor_property('threshold', 0.6); b.set_editor_property('detail_strength', 0.2); b.set_editor_property('density', 2.0); b.update_density()")
cmd("r.SkyLight.RealTimeReflectionCapture 1")
print(subprocess.run([sys.executable, os.path.join(HERE, "boxstate.py"), "restore", snap], capture_output=True, text=True).stdout.strip()[:140])
