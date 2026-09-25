# -*- coding: utf-8 -*-
"""Round 38b: tremble at the cloud boundary up close (owner report: vibration along the volume boundary, stronger when
approaching; Depth Prefilter masks it partly). Camera 'base' (d35_dolly.CAMS): ~40 m under the stratus base looking up at
the cloud edges. Per variant: W (approach at 20 cm/frame), D (strafe) and 0 (static camera: pure temporal flicker, no
motion confound), 48 frames each. Methodology: sky Volumetric Cloud temporarily hidden (its wind motion is not ours),
r.SkyLight.RealTimeReflectionCapture.TimeSlice 0 (full sky capture every frame), background CPU throttle off; key
variants repeated for the noise floor.
RESUMABLE (power cuts): every run is written atomically to results/diag35/b_dolly.json and runs already present are
skipped. The restore target is the owner snapshot measure/owner_pre38b.json taken right after the editor launch, never
the possibly half-modified current state. Summary: results/diag35/r38b_summary.txt.
Usage: python d38_edge.py"""
import os, json
import diag34lib as d
import diag35lib as L
import d35_dolly as B

B.V.update({
    "animoff": ({}, "b.set_editor_property('animate_density', False)", None),
    "lobe0ss0": ({}, "b.set_editor_property('ms_contribution', 0.0); b.set_editor_property('sun_softness', 0.0)", None),
    "quiet": ({}, "b.set_editor_property('animate_density', False); b.set_editor_property('ms_contribution', 0.0); "
                  "b.set_editor_property('sun_softness', 0.0)", None),
    "animoff_si1": ({"r.FogMS.Transport.SolveInterval": "1"}, "b.set_editor_property('animate_density', False)", None),
})
PLAN = [("boxoff", 2), ("cur", 2), ("animoff", 2), ("si1", 1), ("lobe0", 1), ("ss0", 1), ("lobe0ss0", 1), ("quiet", 1),
        ("animoff_si1", 1), ("hmss16", 1), ("tr0", 1), ("pf1", 1), ("jit0", 1), ("gpx8", 1), ("det0", 1)]
DIRS = ("W", "D", "0")
OWNER = os.path.join(d.HERE, "measure", "owner_pre38b.json")
CLOUD = ("import unreal\nA=unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()\n"
         "vc=[a for a in A if a.get_class().get_name()=='VolumetricCloud']\n")

def key(variant, D, r):
    return "b_base_%s_%s_s20_r38b_%d" % (variant, D, r)

def done():
    p = os.path.join(L.RES, "b_dolly.json")
    return json.load(open(p)) if os.path.isfile(p) else {}

def summary():
    res = done(); lines = ["variant  rep  |  W d2b wob  |  D d2b wob  |  static d2b wob"]
    for variant, reps in PLAN:
        for r in range(reps):
            row = ["%-12s" % variant, str(r)]
            for D in DIRS:
                v = res.get(key(variant, D, r), {})
                row.append("%.3f %.3f" % (v.get("d2_blur", float("nan")), v.get("wob_med", float("nan"))))
            lines.append("  ".join(row))
    open(os.path.join(L.RES, "r38b_summary.txt"), "w").write("\n".join(lines) + "\n")
    print("\n".join(lines), flush=True)

def main():
    owner = json.load(open(OWNER))
    L.restore(owner)  # start from the owner's state even if a previous run was cut mid-variant
    L.set_throttle(False)
    d.cmd("r.SkyLight.RealTimeReflectionCapture.TimeSlice 0")
    d.py(CLOUD + "[a.set_is_temporarily_hidden_in_editor(True) for a in vc]\nprint('hidden')")
    try:
        for variant, reps in PLAN:
            for r in range(reps):
                have = done(); missing = tuple(D for D in DIRS if key(variant, D, r) not in have)
                if missing: B.run(variant, missing, 20.0, 48, "base", "_r38b_%d" % r)
    finally:
        d.cmd("r.SkyLight.RealTimeReflectionCapture.TimeSlice 1")
        d.py(CLOUD + "[a.set_is_temporarily_hidden_in_editor(False) for a in vc]\nprint('shown')")
        L.restore(owner)
        now = L.snapshot()
        print("RESTORE box diff", {k: (owner["box"][k], now["box"].get(k)) for k in owner["box"] if owner["box"][k] != now["box"].get(k)},
              "camera", now["camera"], flush=True)
    summary()

if __name__ == "__main__":
    main()
