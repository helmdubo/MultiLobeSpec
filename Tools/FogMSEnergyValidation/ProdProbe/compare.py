# -*- coding: utf-8 -*-
"""PSNR of each measure/<label>/shot.png against measure/<ref>/shot.png, plus metrics table."""
import sys, os, json, glob, numpy as np
from PIL import Image
S = os.path.dirname(os.path.abspath(__file__)); M = os.path.join(S, "measure"); ref = sys.argv[1] if len(sys.argv) > 1 else "ref_96_it64_cold"
R = np.asarray(Image.open(os.path.join(M, ref, "shot.png")).convert("RGB"), dtype=np.float64)
print("%-24s %8s %10s %10s %8s" % ("label", "PSNR dB", "resid", "transp ms", "frame"))
for d in sorted(glob.glob(os.path.join(M, "*"))):
    n = os.path.basename(d); p = os.path.join(d, "shot.png")
    if not os.path.exists(p): continue
    A = np.asarray(Image.open(p).convert("RGB"), dtype=np.float64)
    psnr = float("inf") if A.shape != R.shape else (99.0 if (mse := ((A - R) ** 2).mean()) == 0 else 10 * np.log10(255 ** 2 / mse))
    met = {}
    try: met = json.load(open(os.path.join(d, "dump.json")))
    except Exception: pass
    prof = open(os.path.join(d, "gpuprofile.txt"), encoding="utf-8").read() if os.path.exists(os.path.join(d, "gpuprofile.txt")) else ""
    tr = next((l.split()[-2] for l in prof.splitlines() if "transport/world" in l), "?")
    fr = next((l.split("GPU frame")[1].split("ms")[0].strip() for l in prof.splitlines() if "GPU frame" in l), "?")
    print("%-24s %8.2f %10s %10s %8s" % (n, psnr, met.get("maxRelativeCellResidual", "?"), tr, fr))
