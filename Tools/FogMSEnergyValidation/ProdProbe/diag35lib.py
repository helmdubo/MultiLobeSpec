# -*- coding: utf-8 -*-
"""Round-35 session helpers on top of diag34lib (same frame capture: r.DumpingMovie + r.BufferVisualizationDumpFrames,
consecutive real viewport frames, works in FourPanes). Frames go to measure/diag35/<name>/f###.png, results to
results/diag35. Adds: full editor-state snapshot/restore (camera, background-throttle flag, cvars, Live Box authored
properties incl. density/threshold/softness/detail/feather, Height Fog volumetric properties) and cvar readback.
Usage: python diag35lib.py save <file> | restore <file> | show"""
import sys, os, json, time
import diag34lib as d

HERE = d.HERE
d.M = os.path.join(HERE, "measure", "diag35")
M = d.M
RES = os.path.join(HERE, "results", "diag35")
os.makedirs(M, exist_ok=True); os.makedirs(RES, exist_ok=True)

CVARS = ["r.VolumetricFog.GridSizeZ", "r.VolumetricFog.GridPixelSize", "r.VolumetricFog.DepthDistributionScale",
         "r.VolumetricFog.HistoryWeight", "r.VolumetricFog.Jitter", "r.VolumetricFog.HistoryMissSupersampleCount",
         "r.VolumetricFog.TemporalReprojection", "r.VolumetricFog.InjectRaytracedLights",
         "r.Lumen.TranslucencyVolume.Enable", "r.Lumen.TranslucencyVolume.SpatialFilter",
         "r.Lumen.TranslucencyVolume.Temporal.Jitter", "r.Lumen.TranslucencyVolume.GridPixelSize",
         "r.FogMS.Transport.DirectSamples", "r.FogMS.Transport.SolveInterval", "r.FogMS.Transport.AsyncCompute",
         "r.FogMS.ViewIntegration", "r.FogMS.MaxBoxesPerFrame", "r.SkyLight.RealTimeReflectionCapture",
         "r.FogMS.Enable", "r.FogMS.BoxMode", "r.FogMS.Steps", "r.FogMS.SSFS", "r.Fog.ScreenSpaceScattering",
         "r.BufferVisualizationDumpFrames", "r.DumpingMovie"]
BOXPROPS = ["enabled", "scattering_mode", "transport_preset", "angular_quality", "transport_iterations", "transport_tolerance",
            "emissive_injection", "hybrid_single_scattering", "debug_field_only", "use_manual_animation_time",
            "manual_animation_time", "animate_density", "density", "threshold", "softness", "detail_strength", "detail_scale",
            "detail_second_octave", "density_edge_feather", "erosion_strength", "erosion_depth", "height_profile",
            "height_profile_preset", "height_bottom", "height_top", "bottom_softness", "top_softness", "anvil_strength",
            # W36/W37 look properties: without them a probe that changes one leaves it changed after 'restore'.
            "depth_prefilter", "ms_contribution", "phase_g", "ms_occlusion", "ms_back_floor", "sun_softness", "ms_eccentricity"]
ENUMS = {"scattering_mode": "FogMSScatteringMode", "transport_preset": "FogMSTransportPreset",
         "angular_quality": "FogMSAngularQuality", "height_profile_preset": "FogMSHeightProfilePreset"}
HFPROPS = ["volumetric_fog_distance", "volumetric_fog_start_distance", "volumetric_fog_near_fade_in_distance",
           "volumetric_fog_scattering_distribution", "volumetric_fog_extinction_scale"]
HF = "hf=A['FogMS - Height Fog'].get_component_by_class(unreal.ExponentialHeightFogComponent)"
PERF = "perf=unreal.get_default_object(unreal.load_class(None, '/Script/UnrealEd.EditorPerformanceSettings'))"

def getcv(names):
    code = "import unreal, json\nprint(json.dumps({n: unreal.SystemLibrary.get_console_variable_float_value(n) for n in %r}))" % (list(names),)
    return json.loads(d.py(code).splitlines()[-1])

def setcv(dct):
    for k, v in dct.items(): d.cmd("%s %s" % (k, v))

def snapshot():
    code = ("import unreal, json\n" + d.ACT + "\n" + HF + "\n" + PERF + "\n" + d.BOX + "\n"
            "les=unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem)\nl,r=les.get_level_viewport_camera_info()\n"
            "def cv(v):\n    return int(v.value) if hasattr(v,'value') else (bool(v) if isinstance(v,bool) else (float(v) if isinstance(v,float) else (int(v) if isinstance(v,int) else str(v))))\n"
            "s={'camera':[[l.x,l.y,l.z],[r.pitch,r.yaw,r.roll]], 'throttle': bool(perf.get_editor_property('bThrottleCPUWhenNotForeground')),\n"
            "   'box': {p: cv(b.get_editor_property(p)) for p in %r}, 'hf': {p: cv(hf.get_editor_property(p)) for p in %r},\n"
            "   'status': str(b.get_editor_property('spatial_status'))}\nprint(json.dumps(s))" % (BOXPROPS, HFPROPS))
    s = json.loads(d.py(code).splitlines()[-1])
    s["cvars"] = getcv(CVARS)
    return s

def set_throttle(flag):
    return d.py("import unreal\n" + PERF + "\nperf.set_editor_property('bThrottleCPUWhenNotForeground', %s)\n"
                "print('throttle', perf.get_editor_property('bThrottleCPUWhenNotForeground'))" % bool(flag))

def box_assign(props):
    """Python assignment lines for the Live Box from a {prop: value} dict (enums by int)."""
    lines = []
    order = ["scattering_mode", "transport_preset", "angular_quality", "transport_iterations", "transport_tolerance",
             "height_profile_preset"] + [p for p in BOXPROPS if p not in ("scattering_mode", "transport_preset", "angular_quality",
                                                                             "transport_iterations", "transport_tolerance", "height_profile_preset")]
    for p in order:
        if p not in props: continue
        v = props[p]
        if p in ENUMS: val = "unreal.%s.cast(%d)" % (ENUMS[p], int(v))
        elif isinstance(v, bool): val = "True" if v else "False"
        else: val = repr(v)
        lines.append("try: b.set_editor_property('%s', %s)\nexcept Exception as e: print('SKIP %s', e)" % (p, val, p))
    return "\n".join(lines)

def set_hf(props):
    code = "import unreal\n" + d.ACT + "\n" + HF + "\n"
    for p, v in props.items(): code += "hf.set_editor_property(%r, %r)\n" % (p, v)
    code += "print({p: hf.get_editor_property(p) for p in %r})" % (list(props),)
    return d.py(code)

def restore(s, cam=True):
    out = []
    out.append(d.setbox(box_assign(s["box"])))
    out.append(set_hf(s["hf"]))
    skip = ("r.BufferVisualizationDumpFrames", "r.DumpingMovie")
    for k, v in s["cvars"].items():
        if k in skip: continue
        d.cmd("%s %s" % (k, ("%d" % v) if float(v).is_integer() else ("%g" % v)))
    d.cmd("r.BufferVisualizationDumpFrames 0"); d.cmd("r.DumpingMovie 0")
    d.restore_overview()
    if cam: out.append(d.set_cam(*s["camera"]))
    out.append(set_throttle(s["throttle"]))
    return out

if __name__ == "__main__":
    mode = sys.argv[1] if len(sys.argv) > 1 else "show"
    if mode == "save":
        s = snapshot(); json.dump(s, open(sys.argv[2], "w"), indent=1); print(json.dumps(s, indent=1))
    elif mode == "restore":
        s = json.load(open(sys.argv[2])); print("\n".join(str(x) for x in restore(s, cam="--nocam" not in sys.argv)))
    else:
        print(json.dumps(snapshot(), indent=1))

def capture_slide(name, n=40, start_step=20.0, dist_scale_step=0.0, pre=12, timeout=300):
    """Static camera, the froxel grid slides instead: every editor tick (= frame) the Height Fog's VolumetricFogStartDistance
    grows by start_step cm (UE froxel depth z(s) = N + (F - N)(2^(s/S) - 1)/(2^((G-1)/S) - 1) with N = max(near, start):
    at z << F every slice moves by ~start_step, as in a W dolly of that speed, while nothing else in the view moves), and/or
    VolumetricFogDistance is scaled by (1 + dist_scale_step) per tick. Frames as diag34lib.capture (r.DumpingMovie after
    'pre' ticks). The caller restores the Height Fog properties."""
    import glob as _g, shutil as _sh
    out = os.path.join(M, name)
    _sh.rmtree(out, ignore_errors=True); os.makedirs(out)
    before = d._frames()
    d.cmd("r.BufferVisualizationOverviewTargets FogMSNoTarget")
    d.cmd("r.BufferVisualizationDumpFrames 1")
    tj = os.path.join(out, "ticks.json").replace("\\", "/")
    code = ("import unreal, time, json\n" + d.ACT + "\n" + HF + "\n"
            "s0=hf.get_editor_property('volumetric_fog_start_distance'); f0=hf.get_editor_property('volumetric_fog_distance')\n"
            "st={'n':0,'t':[],'start':[],'dist':[]}\nN=%d\nPRE=%d\nSS=%r\nDS=%r\n"
            "def _tick(dt):\n"
            "    st['n']+=1; k=st['n']\n"
            "    sd=s0+SS*k; fd=f0*(1.0+DS)**k\n"
            "    try:\n"
            "        hf.set_volumetric_fog_start_distance(sd); hf.set_volumetric_fog_distance(fd)\n"
            "    except Exception as e:\n"
            "        unreal.unregister_slate_post_tick_callback(unreal._diag35_h); unreal.log_warning('diag35 tick stopped: ' + str(e)); return\n"
            "    st['t'].append(time.perf_counter()); st['start'].append(sd); st['dist'].append(fd)\n"
            "    if k==PRE:\n"
            "        unreal.SystemLibrary.execute_console_command(None, 'r.DumpingMovie %%d' %% N)\n"
            "    if k>=PRE+N+3:\n"
            "        unreal.unregister_slate_post_tick_callback(unreal._diag35_h)\n"
            "        open(r'%s','w').write(json.dumps(st))\n"
            "unreal._diag35_h=unreal.register_slate_post_tick_callback(_tick)\nprint('ARMED')" % (n, pre, float(start_step), float(dist_scale_step), tj))
    d.py(code)
    t0 = time.time()
    while time.time() - t0 < timeout:
        if len(d._frames() - before) >= n and os.path.isfile(tj): break
        time.sleep(1.0)
    time.sleep(1.0)
    d.cmd("r.BufferVisualizationDumpFrames 0"); d.cmd("r.DumpingMovie 0")
    new = sorted(d._frames() - before)
    for i, f in enumerate(new): _sh.move(f, os.path.join(out, "f%03d.png" % i))
    for f in _g.glob(os.path.join(d.SS, "MovieFrame*_*.png")):
        try: os.remove(f)
        except Exception: pass
    print("CAPTURE %s: %d frames%s" % (name, len(new), "" if len(new) == n else " (WANTED %d)" % n), flush=True)
    return out
