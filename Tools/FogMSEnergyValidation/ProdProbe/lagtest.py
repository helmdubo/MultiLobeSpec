# -*- coding: utf-8 -*-
import sys, os, time, glob, subprocess, shutil, numpy as np
from PIL import Image
S = os.path.dirname(os.path.abspath(__file__)); OUT = os.path.join(S, "measure", "lag_" + sys.argv[1]); os.makedirs(OUT, exist_ok=True)
quality, iters, warm = sys.argv[2], sys.argv[3], sys.argv[4]
SH = r"D:/PersonalProjects/UE5/MimirHead_portfolio 5.7 5.8 - 3/Saved/Screenshots/WindowsEditor"
def br(*a): return subprocess.run([sys.executable, os.path.join(S, "uemcp.py"), *a], capture_output=True, text=True, encoding="utf-8", errors="replace").stdout
def light(v): br("py", "import unreal\nfor a in unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors():\n    if a.get_actor_label()=='PointLight': a.get_component_by_class(unreal.LightComponent).set_intensity(%s)" % v)
br("py", "import unreal\nb=[a for a in unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors() if a.get_actor_label()=='FogMS - Live Box'][0]\nb.set_editor_property('angular_quality', unreal.FogMSAngularQuality.%s)\nb.set_editor_property('transport_iterations', %d)" % (quality, int(iters)))
br("cmd", "r.FogMS.Transport.WarmStart %s" % warm)
before = set(glob.glob(SH + "/*.png")); stamps = []
def shot(tag, t):
    br("cmd", "HighResShot 1600x900"); stamps.append((tag, t))
light(80000); time.sleep(6); shot("on_steady", -1)
t0 = time.time(); light(0)
for dt in (0.3, 0.6, 0.9, 1.2, 1.6, 2.0, 3.0):
    while time.time() - t0 < dt: time.sleep(0.02)
    shot("off_%.1f" % dt, dt)
time.sleep(5); shot("off_steady", 99)
light(80000); time.sleep(5); shot("on_again", -2)
time.sleep(3)
files = sorted(set(glob.glob(SH + "/*.png")) - before, key=os.path.getmtime)
print("shots requested %d, files %d" % (len(stamps), len(files)))
imgs = {}
for (tag, t), f in zip(stamps, files):
    shutil.copy(f, os.path.join(OUT, tag + ".png")); imgs[tag] = np.asarray(Image.open(f).convert("RGB"), dtype=np.float64)
ref = imgs.get("off_steady")
def psnr(a, b):
    m = ((a - b) ** 2).mean(); return 99.0 if m == 0 else 10 * np.log10(255 ** 2 / m)
print("== lag test %s: quality %s, iterations %s, warm %s (PSNR vs off_steady; higher = closer to converged 'light off')" % (sys.argv[1], quality, iters, warm))
for tag, t in stamps:
    if tag in imgs and ref is not None: print("  %-10s %6.2f dB" % (tag, psnr(imgs[tag], ref)))
