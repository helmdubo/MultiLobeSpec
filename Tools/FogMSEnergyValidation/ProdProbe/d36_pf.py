# -*- coding: utf-8 -*-
"""Round 36 look check of Depth Prefilter 0 / 1 / 2 (injection + hybrid), live-sun views as d35_look.py 'sun'.
Sheet results/diag36/pf_sheet.png, numbers results/diag36/pf.json. Restores the Box and the owner camera."""
import os, sys, json, time
import numpy as np
from PIL import Image, ImageDraw
sys.argv = [sys.argv[0], "sun"]
import d35_look as T
import diag34lib as d
import diag35lib as L
RES = os.path.join(d.HERE, "results", "diag36"); OUT = os.path.join(RES, "pf"); os.makedirs(OUT, exist_ok=True)
CFG = [("PF0", "b.set_editor_property('depth_prefilter', 0.0)\n"), ("PF1", "b.set_editor_property('depth_prefilter', 1.0)\n"),
       ("PF2", "b.set_editor_property('depth_prefilter', 2.0)\n")]
snap = L.snapshot(); L.set_throttle(False); res = {}
try:
    for name, a in CFG:
        d.setbox(L.box_assign(snap["box"])); d.setbox(a); time.sleep(6); res[name] = {}
        for vn, (loc, rot) in T.VIEWS.items():
            d.set_cam(loc, rot); time.sleep(5)
            img = T.frame(d.capture("pf_%s_%s" % (name, vn), n=4).split(os.sep)[-1])
            Image.fromarray(np.clip(img, 0, 255).astype(np.uint8)).save(os.path.join(OUT, "%s_%s.png" % (name, vn)))
            res[name][vn] = round(float(img.mean()), 2); print(name, vn, res[name][vn], flush=True)
finally:
    d.setbox(L.box_assign(snap["box"])); d.cmd("r.BufferVisualizationDumpFrames 0"); d.cmd("r.DumpingMovie 0"); d.restore_overview()
    d.set_cam(*T.OWNER)
json.dump(res, open(os.path.join(RES, "pf.json"), "w"), indent=1)
W, H, LAB = 480, 270, 80; V = list(T.VIEWS)
sheet = Image.new("RGB", (LAB + W * len(V), 22 + H * len(CFG)), (18, 18, 18)); dr = ImageDraw.Draw(sheet)
for j, vn in enumerate(V): dr.text((LAB + j * W + 6, 5), vn, fill=(230, 230, 230))
for i, (name, _) in enumerate(CFG):
    dr.text((6, 22 + i * H + 8), name, fill=(240, 220, 90))
    for j, vn in enumerate(V):
        sheet.paste(Image.open(os.path.join(OUT, "%s_%s.png" % (name, vn))).convert("RGB").resize((W, H)), (LAB + j * W, 22 + i * H))
sheet.save(os.path.join(RES, "pf_sheet.png")); print("SHEET done", flush=True)
