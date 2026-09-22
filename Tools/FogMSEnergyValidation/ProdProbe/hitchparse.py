# -*- coding: utf-8 -*-
"""Summarise stat dumphitches entries after a UTC timestamp (yyyy.MM.dd-HH.mm.ss) from the editor log."""
import re, sys, io
import os
LOG = os.environ.get("FOGMS_LOG", r"D:/PersonalProjects/UE5/MimirHead_portfolio 5.7 5.8 - 3/Saved/Logs/MimirHead_portfolio.log")
after = sys.argv[1]; label = sys.argv[2] if len(sys.argv) > 2 else ""
lines = io.open(LOG, encoding="utf-8", errors="replace").read().splitlines()
strip = lambda l: re.sub(r"^\[[^\]]*\]\[ *\d+\]LogStats: ", "", l)
idx = [i for i, l in enumerate(lines) if "Thread Hitch" in l]
keys = [("QueryCursor", "QueryCursor"), ("GPUIdleFlush", "SubmitAndBlockUntilGPUIdle_Flush"),
        ("OcclusionFence", "OcclusionSubmittedFence Wait"), ("ViewVisibility", "View Visibility"),
        ("ReadSurface", "ReadSurfaceCommand"), ("SlateTick", "Total Slate Tick Time")]
print("== %s (hitches after %s UTC)" % (label, after))
for n, h in enumerate(idx):
    ts = re.search(r"\[(\d{4}\.\d\d\.\d\d-\d\d\.\d\d\.\d\d)", lines[h]).group(1)
    if ts < after: continue
    total = re.search(r"(\d+\.\d)ms", lines[h]).group(1)
    end = idx[n + 1] if n + 1 < len(idx) else min(h + 4000, len(lines))
    block = [strip(l) for l in lines[h:end]]
    found = {}
    for k, pat in keys:
        for l in block:
            m = re.match(r"\s*(\d+\.\d+)ms .*-\s+{0}".format(re.escape(pat)), l)
            if m and (k not in found or float(m.group(1)) > found[k]): found[k] = float(m.group(1))
    print("  %s  total %6s ms | " % (ts, total) + "  ".join("%s=%.0f" % (k, found[k]) for k, _ in keys if k in found))
