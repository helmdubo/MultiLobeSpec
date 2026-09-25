# -*- coding: utf-8 -*-
"""Round 38b follow-up: static-camera flicker vs Edge Flow Speed (owner: 500 cm/s, wind 0) and mitigations, camera 'base'.
Resumable like d38_edge.py (skips runs present in results/diag35/b_dolly.json); restores from measure/owner_pre38b.json."""
import os, json
import diag34lib as d
import diag35lib as L
import d35_dolly as B
import d38_edge as E

B.V.update({
    "ef200": ({}, "b.set_editor_property('edge_flow_speed', 200.0)", None),
    "ef100": ({}, "b.set_editor_property('edge_flow_speed', 100.0)", None),
    "ef500_pf1_si1": ({"r.FogMS.Transport.SolveInterval": "1"}, "b.set_editor_property('depth_prefilter', 1.0)", None),
    "ds8_si1": ({"r.FogMS.Transport.DirectSamples": "8", "r.FogMS.Transport.SolveInterval": "1"}, None, None),
    "hw0.7": ({"r.VolumetricFog.HistoryWeight": "0.7"}, None, None),
    "hw0.7_pf1": ({"r.VolumetricFog.HistoryWeight": "0.7"}, "b.set_editor_property('depth_prefilter', 1.0)", None),
    "ef200_pf05": ({}, "b.set_editor_property('edge_flow_speed', 200.0); b.set_editor_property('depth_prefilter', 0.5)", None),
})
PLAN = [("cur", 2), ("ef200", 2), ("ef100", 2), ("ef500_pf1_si1", 2), ("ef200_pf05", 2), ("ds8", 2), ("ds8_si1", 2), ("hw0.8", 2), ("hw0.7", 2), ("hw0.7_pf1", 2)]

def main():
    owner = json.load(open(E.OWNER)); L.restore(owner); L.set_throttle(False)
    d.cmd("r.SkyLight.RealTimeReflectionCapture.TimeSlice 0")
    d.py(E.CLOUD + "[a.set_is_temporarily_hidden_in_editor(True) for a in vc]\nprint('hidden')")
    try:
        for variant, reps in PLAN:
            for r in range(reps):
                k = "b_base_%s_0_s20_r38c_%d" % (variant, r)
                if k not in E.done(): B.run(variant, ("0",), 20.0, 48, "base", "_r38c_%d" % r)
    finally:
        d.cmd("r.SkyLight.RealTimeReflectionCapture.TimeSlice 1")
        d.py(E.CLOUD + "[a.set_is_temporarily_hidden_in_editor(False) for a in vc]\nprint('shown')")
        d.setbox("b.set_editor_property('edge_flow_speed', 500.0)"); L.restore(owner); now = L.snapshot()
        print("RESTORE box diff", {k: (owner["box"][k], now["box"].get(k)) for k in owner["box"] if owner["box"][k] != now["box"].get(k)}, flush=True)
    res = E.done()
    for variant, reps in PLAN:
        vals = [res.get("b_base_%s_0_s20_r38c_%d" % (variant, r), {}) for r in range(reps)]
        print("%-16s static d2b %s  wob %s" % (variant, " / ".join("%.3f" % v.get("d2_blur", float("nan")) for v in vals),
                                              " / ".join("%.3f" % v.get("wob_med", float("nan")) for v in vals)), flush=True)

if __name__ == "__main__":
    main()
