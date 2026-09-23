# -*- coding: utf-8 -*-
"""Run ProfileGPU through the UE-MCP bridge (unless --parse-only) and summarise the last profile block from the editor log.
Usage: python gpuprofile.py [label] [--parse-only]
"""
import sys, re, io, time, subprocess, os
import os
LOG = os.environ.get("FOGMS_LOG", r"D:/PersonalProjects/UE5/MimirHead_portfolio 5.7 5.8 - 3/Saved/Logs/MimirHead_portfolio.log")
HERE = os.path.dirname(os.path.abspath(__file__))
label = next((a for a in sys.argv[1:] if not a.startswith("--")), "profile")
if "--parse-only" not in sys.argv:
    subprocess.run([sys.executable, os.path.join(HERE, "uemcp.py"), "cmd", "r.ProfileGPU.ShowUI 0"], capture_output=True)
    subprocess.run([sys.executable, os.path.join(HERE, "uemcp.py"), "cmd", "ProfileGPU"], capture_output=True)
    time.sleep(7)
lines = io.open(LOG, encoding="utf-8", errors="replace").read().splitlines()
starts = [i for i, l in enumerate(lines) if "GPU Profile for Frame" in l and "Graphics pipeline" in l]
if not starts:
    print("no profile block"); sys.exit(1)
s = starts[-1]
block = []
for l in lines[s:s + 4000]:
    if "LogRHI" not in l: break
    block.append(l)
frame = next((re.search(r"Frame Time\s*:\s*([\d.]+)ms", l).group(1) for l in block if "Frame Time" in l), "?")
rows = []
for l in block:
    m = re.search(r".*[│┊]\s*([\d.]+) ms\s*┃\s*(\S.*?)\s*┃", l)
    if m:
        rows.append((float(m.group(1)), m.group(2)))
def total(pred):
    return sum(ms for ms, name in rows if pred(name))
print("== %s: GPU frame %s ms, %d events" % (label, frame, len(rows)))
groups = [
    ("FogMS transport/world (B2/B3)", lambda n: n.startswith("FogMS B2") or n.startswith("FogMS B3") or n.startswith("FogMS World")),
    ("FogMS other (shadow/spatial/SSFS/publish)", lambda n: n.startswith("FogMS") and not (n.startswith("FogMS B2") or n.startswith("FogMS B3") or n.startswith("FogMS World"))),
    ("Native VolumetricFog", lambda n: n.startswith("VolumetricFog") or n.startswith("LightScattering") or n.startswith("FinalIntegration") or n.startswith("InitializeVolumeAttributes") or n.startswith("ShadowedLights")),
    ("Lumen", lambda n: n.startswith("Lumen")),
    ("ShadowDepths/VSM", lambda n: n.startswith("ShadowDepths") or n.startswith("VirtualShadowMap") or n.startswith("Shadow")),
]
for name, pred in groups:
    print("  %-44s %8.2f ms" % (name, total(pred)))
print("  -- top FogMS events:")
for ms, name in sorted((r for r in rows if r[1].startswith("FogMS")), reverse=True)[:12]:
    print("     %7.3f ms  %s" % (ms, name))
print("  -- top 10 events overall:")
for ms, name in sorted(rows, reverse=True)[:10]:
    print("     %7.3f ms  %s" % (ms, name))
