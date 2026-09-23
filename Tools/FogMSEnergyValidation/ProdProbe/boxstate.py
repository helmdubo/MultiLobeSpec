# -*- coding: utf-8 -*-
"""Snapshot / restore the Live Box actor's test-relevant properties through the UE-MCP bridge, so measurement scripts
return the actor to whatever the owner authored (not to a hard-coded preset).
Usage: python boxstate.py save [file]   |   python boxstate.py restore [file]   (default file: measure/boxstate.json)"""
import sys, os, json, subprocess
HERE = os.path.dirname(os.path.abspath(__file__))
PROPS = ["scattering_mode", "transport_preset", "angular_quality", "transport_iterations", "transport_tolerance",
         "emissive_injection", "hybrid_single_scattering", "use_manual_animation_time", "manual_animation_time"]
ENUMS = {"scattering_mode": "FogMSScatteringMode", "transport_preset": "FogMSTransportPreset", "angular_quality": "FogMSAngularQuality"}
BOX = "b=[a for a in unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors() if a.get_actor_label()=='FogMS - Live Box'][0]"

def py(code):
    r = subprocess.run([sys.executable, os.path.join(HERE, "uemcp.py"), "py", code], capture_output=True, text=True, encoding="utf-8", errors="replace")
    try:
        j = json.loads(r.stdout)
        return ((j.get("result") or {}).get("output", "") or str(j.get("error", ""))).strip()
    except Exception:
        return r.stdout[-300:]

mode = sys.argv[1] if len(sys.argv) > 1 else "save"
path = sys.argv[2] if len(sys.argv) > 2 else os.path.join(HERE, "measure", "boxstate.json")
if mode == "save":
    code = "import unreal, json\n" + BOX + "\nd={}\n"
    for p in PROPS:
        code += "try:\n    v=b.get_editor_property('%s'); d['%s']=int(v.value) if hasattr(v,'value') else (bool(v) if isinstance(v,bool) else (float(v) if isinstance(v,float) else int(v)))\nexcept Exception: pass\n" % (p, p)
    code += "print(json.dumps(d))"
    out = py(code)
    state = json.loads(out.splitlines()[-1])
    os.makedirs(os.path.dirname(path), exist_ok=True)
    json.dump(state, open(path, "w"), indent=1)
    print("SAVED", path, state)
else:
    state = json.load(open(path))
    code = "import unreal\n" + BOX + "\n"
    # preset first so the tuple below survives when the preset is Custom; if the preset is not Custom, its tuple wins.
    for p in ["scattering_mode", "transport_preset", "angular_quality", "transport_iterations", "transport_tolerance",
              "emissive_injection", "hybrid_single_scattering", "manual_animation_time", "use_manual_animation_time"]:
        if p not in state: continue
        v = state[p]
        if p in ENUMS: val = "unreal.%s.cast(%d)" % (ENUMS[p], int(v))
        elif isinstance(v, bool): val = "True" if v else "False"
        else: val = repr(v)
        code += "try: b.set_editor_property('%s', %s)\nexcept Exception as e: print('SKIP %s', e)\n" % (p, val, p)
    code += "b.update_density(); print('RESTORED', {p: str(b.get_editor_property(p)) for p in ('transport_preset','angular_quality','transport_iterations','transport_tolerance','emissive_injection','use_manual_animation_time')})"
    print(py(code))
