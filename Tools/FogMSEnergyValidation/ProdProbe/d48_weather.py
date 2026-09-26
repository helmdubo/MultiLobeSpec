# -*- coding: utf-8 -*-
"""Round 48 (W48, FogMS_Weather_Design.md section 6 'W48'): LEAN phase-2 check of the FogMS Weather actor v0 in the owner's editor
(owner's rule: status / log checks + ONE GPU number each for the visible pass and the shadow pass; he judges the look himself; no
screenshot series; target <= 30 min editor time). Criteria:
  1. preset change over 10 s: log 'weather: A -> B (10 s)', the actor status shows the progress, the weather map changes with time
     (RT_FogMS_WeatherMap redrawn during the transition: MapDrawCount + two texels read back; the wind displacement grows);
  2. Clear (all coverages 0): the host layer is not extended, the managed cvars unchanged, one frame pair (no actor / actor on Clear) at
     the owner camera within the noise floor of a repeat frame;
  3. Scattered / Broken / Overcast shadows on the ground (the owner, by eye: nothing captured here);
  4. deleting the actor: the host layer and the cvars come back (log lines);
  5. no LogPython errors, ensures, 'Failed to compile Material', LogMultiLobeSpec errors;
  6. the numbers: ProfileGPU x3 at the owner camera: VolumetricCloud (visible pass) and VolumetricCloudShadow (shadow pass) for
     no weather / Overcast extended / Overcast thin -> the visible-pass delta of the extended layer vs the +0.3 ms gate (decision 2:
     extended when <= 0.3 ms, else the thin layer).
The owner's Box must render through the host for 6 to mean anything: when it does not (its band below the ground, round 47), the
script lifts it by LIFT_CM for the run and puts it back exactly. The sun is set up with FogMS.Weather.SetupShadows for the run and
restored from the snapshot (Cast Cloud Shadows, extent, resolution, ray sample scale); the weather actor the script spawns is deleted.

Subcommands (python d48_weather.py <sub>; FOGMS_LOG = the running editor's log, required):
  snapshot [--force]  measure/owner_pre48.json (READ ONLY; written once: the restore target, also after a power cut)
  matedit             matedit_weather.py (textures, compose / sun materials, presets), then matedit_cloud.py (host material v3)
  copyback            the saved weather assets + M_FogMS_Cloud from the project plugin into this worktree (files only)
  check               criteria 1, 2, 4, 5
  cost                criterion 6
  all                 check + cost, then restore and summary (one command)
  restore             restore the snapshot
  summary             verdicts from results/diag48/d48.json
Rules: the map is never saved; no tick callbacks, no mark_render_state_dirty, no engine cvar set from the console (the plugin owns them;
a console value would lock them for the session); throttle off during the run and restored; ShowFlag.OverrideDiffuseAndSpecular 0
(Lit) during the run and 2 (the viewport's own mode, VMI_Lit in the ini) after; ProfileGPU waits while the editor is minimized; a
LogPython error flood stops the run; frames go to measure/diag48 (junction to D:/FogMS_ProbeFrames/diag48); results/diag48/d48.json
written atomically after every step."""
import os, sys, json, time, re, glob, shutil, hashlib
import numpy as np
import diag34lib as d
import cloudproto_session as S

HERE = d.HERE
RES = os.path.join(HERE, "results", "diag48")
os.makedirs(RES, exist_ok=True)
OUT = os.path.join(RES, "d48.json")
OWNER = os.path.join(HERE, "measure", "owner_pre48.json")
FRAMES = os.path.join(HERE, "measure", "diag48")          # junction -> D:/FogMS_ProbeFrames/diag48
LOG = os.environ.get("FOGMS_LOG", "")
S.LOG = LOG
PROJECT = "D:/PersonalProjects/UE5/MimirHead_portfolio 5.7 5.8 - 3"
WORKTREE = os.path.normpath(os.path.join(HERE, "..", "..", ".."))
SS = PROJECT + "/Saved/Screenshots/WindowsEditor"
BOX_LABEL = "FogMS - Live Box"
TEST_LABEL = "FogMS Weather (d48 test)"
WEATHER_DIR = "/MultiLobeSpec/FogMS/Weather"
SETTLE = 3.0
LIFT_CM = 1000.0
FROZEN_T = 100.0
GATE_MS = 0.3
SUN_PROPS = ["cast_cloud_shadows", "cloud_shadow_extent", "cloud_shadow_map_resolution_scale", "cloud_shadow_ray_sample_count_scale",
             "cloud_shadow_strength", "cloud_shadow_on_surface_strength", "cloud_shadow_on_atmosphere_strength"]
BOX_PROPS = ["use_manual_animation_time", "manual_animation_time", "animation_time_offset"]
MANAGED = ["r.VolumetricCloud.StepSizeOnZeroConservativeDensity", "r.VolumetricCloud.ShadowMap.SpatialFiltering",
           "r.VolumetricCloud.ShadowMap.SnapLength", "r.VolumetricCloud.ShadowMap.SnapToPixelGrid", "r.VolumetricRenderTarget.Mode",
           "r.VolumetricCloud.SampleMinCount", "r.VolumetricCloud.DistanceToSampleMaxCount"]
PLUGIN_CVARS = ["r.FogMS.Weather.SkipSteps", "r.FogMS.CloudHost.RTMode", "r.FogMS.CloudHost.NearDistanceKm"]
_state = {"flood0": None}

FIND = ("import unreal, json\n"
        "_A=unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()\n"
        "A={a.get_actor_label():a for a in _A}\n"
        "box=A.get(%r)\n"
        "_cm=unreal.load_asset('/MultiLobeSpec/FogMS/M_FogMS_Cloud')\n"
        "def _base(m):\n"
        "    while m is not None and isinstance(m, unreal.MaterialInstance): m=m.get_editor_property('parent')\n"
        "    return m\n"
        "def _cc(a):\n"
        "    return a.get_component_by_class(unreal.VolumetricCloudComponent) if a.get_class().get_name()=='VolumetricCloud' else None\n"
        "hosts=[a for a in _A if _cc(a) is not None and _cm is not None and _base(_cc(a).get_editor_property('material'))==_cm]\n"
        "host=hosts[0] if hosts else None\n"
        "hc=_cc(host) if host else None\n"
        "weathers=[a for a in _A if a.get_class().get_name()=='FogMSWeather']\n"
        "test=A.get(%r)\n"
        "_suns=[]\n"
        "for a in _A:\n"
        "    if isinstance(a, unreal.DirectionalLight):\n"
        "        c=a.get_component_by_class(unreal.DirectionalLightComponent)\n"
        "        if c and c.get_editor_property('atmosphere_sun_light') and c.get_editor_property('atmosphere_sun_light_index')==0: _suns.append((a,c))\n"
        "sunA,sun=(_suns[0] if _suns else (None,None))\n") % (BOX_LABEL, TEST_LABEL)


# ------------------------------------------------------------------ plumbing
def _atomic_dump(obj, path):
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        json.dump(obj, f, indent=1, default=str); f.flush(); os.fsync(f.fileno())
    os.replace(tmp, path)


def _load(path):
    return json.load(open(path)) if os.path.isfile(path) else {}


def last_json(out, tag):
    for line in reversed(out.splitlines()):
        if line.startswith(tag + " "):
            return json.loads(line[len(tag) + 1:])
    raise RuntimeError("no %s line in editor output: %s" % (tag, out[-800:]))


def pyj(code, tag):
    return last_json(d.py(code), tag)


def log_text():
    with open(LOG, "rb") as f:
        return f.read().decode("utf-8", "replace")


def flood_check():
    n = log_text().count("LogPython: Error")
    if _state["flood0"] is None:
        _state["flood0"] = n
    if n - _state["flood0"] > 20:
        raise RuntimeError("LogPython: Error flood (%d new lines): stopping" % (n - _state["flood0"]))
    return n


def editor_minimized():
    try:
        import ctypes, subprocess
        out = subprocess.run(["powershell", "-NoProfile", "-Command",
                              "(Get-Process UnrealEditor -ErrorAction SilentlyContinue | Select-Object -First 1).MainWindowHandle"],
                             capture_output=True, text=True, timeout=30).stdout.strip()
        return bool(out.isdigit() and ctypes.windll.user32.IsIconic(int(out)))
    except Exception:
        return False


def wait_not_minimized(what, max_wait=900.0):
    t0 = time.time()
    while editor_minimized():
        if time.time() - t0 > max_wait:
            raise RuntimeError("%s: the editor stayed minimized for %.0f s" % (what, max_wait))
        print("WAIT %s: the editor is minimized (%.0f s)" % (what, time.time() - t0), flush=True)
        time.sleep(20.0)


def marker(tag):
    m = "D48_%s_%d" % (tag, int(time.time() * 1000))
    d.py("import unreal\nunreal.log(%r)" % m)
    return m


def since(mark):
    d.cmd("FLUSHLOG"); time.sleep(0.5)
    text = log_text()
    return text.rsplit(mark, 1)[1] if mark in text else ""


def step(res, key, value):
    res[key] = value
    _atomic_dump(res, OUT)
    print("STEP %-10s %s" % (key, json.dumps(value, default=str)[:900]), flush=True)


def enum_name(value):
    m = re.search(r"\.(\w+):", str(value))
    return m.group(1) if m else str(value)


# ------------------------------------------------------------------ state (read only)
def state():
    code = FIND + (
        "les=unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem)\n"
        "l,r=les.get_level_viewport_camera_info()\n"
        "perf=unreal.get_default_object(unreal.load_class(None, '/Script/UnrealEd.EditorPerformanceSettings'))\n"
        "lvs=unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)\n"
        "def _v(x):\n"
        "    return str(x) if not isinstance(x, (bool, int, float)) else x\n"
        "out={'camera':[[l.x,l.y,l.z],[r.pitch,r.yaw,r.roll]], 'throttle': bool(perf.get_editor_property('bThrottleCPUWhenNotForeground')),\n"
        " 'game_view': lvs.editor_get_game_view(), 'weathers':[w.get_actor_label() for w in weathers]}\n"
        "if box:\n"
        "    loc=box.get_actor_location()\n"
        "    out['box']={'location':[loc.x,loc.y,loc.z],'render_path':str(box.get_editor_property('render_path')),\n"
        "      'props':{p:_v(box.get_editor_property(p)) for p in %r},'status':str(box.get_editor_property('spatial_status'))}\n"
        "if host:\n"
        "    m=hc.get_editor_property('material')\n"
        "    def _s(n):\n"
        "        try: return m.get_scalar_parameter_value(n) if isinstance(m, unreal.MaterialInstanceDynamic) else None\n"
        "        except Exception: return None\n"
        "    out['host']={'label':host.get_actor_label(),'hidden':bool(host.is_temporarily_hidden_in_editor()),\n"
        "      'layer_km':[hc.get_editor_property('layer_bottom_altitude'),hc.get_editor_property('layer_height')],\n"
        "      'start_km':hc.get_editor_property('tracing_start_distance_from_camera'),'max_km':hc.get_editor_property('tracing_max_distance'),\n"
        "      'density':_s('FogMS_Density'),'weather_on':_s('FogMS_WeatherOn'),'weather_thin':_s('FogMS_WeatherThin'),'skip_margin':_s('FogMS_CloudSkipMargin')}\n"
        "out['hosts']=[h.get_actor_label() for h in hosts]\n"
        "if test:\n"
        "    cv=test.get_editor_property('current_values'); wo=test.get_editor_property('wind_offset')\n"
        "    out['test']={'status':str(test.get_editor_property('weather_status')),'draws':test.get_editor_property('map_draw_count'),\n"
        "      'wind':[wo.x,wo.y],'coverage':cv.get_editor_property('coverage'),'deck':cv.get_editor_property('deck_coverage'),\n"
        "      'shadow_layer':str(test.get_editor_property('shadow_layer')),\n"
        "      'created_host':(lambda h: h.get_actor_label() if h else None)(test.get_editor_property('created_cloud_host'))}\n"
        "if sun:\n"
        "    out['sun']={'label':sunA.get_actor_label(),'props':{p:_v(sun.get_editor_property(p)) for p in %r}}\n"
        "out['cvars']={n: unreal.SystemLibrary.get_console_variable_float_value(n) for n in %r}\n"
        "print('D48STATE '+json.dumps(out))") % (BOX_PROPS, SUN_PROPS, MANAGED + PLUGIN_CVARS)
    return pyj(code, "D48STATE")


def snapshot(force=False):
    if os.path.isfile(OWNER) and not force:
        return json.load(open(OWNER))
    s = state()
    s["note"] = "round 48 snapshot, read before any change in the freshly launched Main48 session"
    s["time"] = time.strftime("%Y-%m-%d %H:%M:%S")
    _atomic_dump(s, OWNER)
    return s


def rendered(st):
    return "[render: cloud host]" in (st.get("box") or {}).get("status", "") and (((st.get("host") or {}).get("density")) or 0.0) > 0.0


# ------------------------------------------------------------------ changes (all undone by restore)
def set_throttle(flag):
    return d.py("import unreal\nperf=unreal.get_default_object(unreal.load_class(None, '/Script/UnrealEd.EditorPerformanceSettings'))\n"
                "perf.set_editor_property('bThrottleCPUWhenNotForeground', %s)\nprint('THROTTLE', perf.get_editor_property('bThrottleCPUWhenNotForeground'))" % bool(flag))


def set_sun(**props):
    code = FIND + "".join("sun.set_editor_property(%r, %r)\n" % (k, v) for k, v in props.items())
    code += "print('SUN', %s)" % ", ".join("sun.get_editor_property(%r)" % k for k in props)
    return d.py(code)


def set_box(**props):
    code = FIND + "".join("box.set_editor_property(%r, %r)\n" % (k, v) for k, v in props.items()) + "box.update_density()\nprint('SETBOX ok')"
    return d.py(code)


def place_box(loc):
    return d.py(FIND + "box.set_actor_location(unreal.Vector(%r, %r, %r), False, True)\nprint('PLACED')" % tuple(loc))


def spawn_weather(preset, layer="EXTENDED"):
    """The test actor at the Box's XY (the weather domain centre), on a plugin preset asset."""
    code = FIND + (
        "sub=unreal.get_editor_subsystem(unreal.EditorActorSubsystem)\n"
        "if test is None:\n"
        "    p=box.get_actor_location() if box else unreal.Vector(0,0,0)\n"
        "    test=sub.spawn_actor_from_class(unreal.FogMSWeather, unreal.Vector(p.x, p.y, 0.0))\n"
        "    test.set_actor_label(%r)\n"
        "test.set_editor_property('shadow_layer', unreal.FogMSWeatherShadowLayer.%s)\n"
        "test.set_editor_property('weather_state', unreal.load_asset(%r))\n"
        "print('SPAWNED', test.get_actor_label(), test.get_editor_property('weather_state'))") % (
        TEST_LABEL, layer, "%s/DA_FogMS_Weather_%s" % (WEATHER_DIR, preset))
    return d.py(code)


def set_layer(layer):
    return d.py(FIND + "test.set_editor_property('shadow_layer', unreal.FogMSWeatherShadowLayer.%s)\nprint('LAYER', test.get_editor_property('shadow_layer'))" % layer)


def delete_weather():
    return d.py(FIND + "n=0\nfor w in [a for a in weathers if a.get_actor_label()==%r]:\n"
                "    unreal.get_editor_subsystem(unreal.EditorActorSubsystem).destroy_actor(w); n+=1\nprint('DELETED', n)" % TEST_LABEL)


def read_map_texels():
    """Two texels of the test actor's RT_FogMS_WeatherMap (a GPU readback; the script only)."""
    code = FIND + ("rt=test.get_editor_property('weather_map')\n"
                   "w=unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()\n"
                   "px=[unreal.RenderingLibrary.read_render_target_raw_pixel(w, rt, x, y, False) for (x,y) in ((100,100),(300,220))]\n"
                   "print('D48TEX '+json.dumps([[c.r,c.g,c.b,c.a] for c in px]))")
    return pyj(code, "D48TEX")


def begin():
    if not LOG or not os.path.isfile(LOG):
        raise SystemExit("Set FOGMS_LOG to the running editor's log file.")
    wait_not_minimized("begin", 300.0)
    flood_check()
    set_throttle(False)
    d.cmd("ShowFlag.OverrideDiffuseAndSpecular 0")


def ensure_rendered(owner):
    """The Box through the host (lift it when its band dips below the ground). Returns the state."""
    st = state()
    if not rendered(st) and owner.get("box"):
        loc = list(owner["box"]["location"]); loc[2] += LIFT_CM
        place_box(loc); time.sleep(SETTLE); st = state()
        st["lifted"] = True
    return st


def restore(owner):
    delete_weather()
    time.sleep(SETTLE)
    now = state()
    sp = {k: v for k, v in (owner.get("sun") or {}).get("props", {}).items() if (now.get("sun") or {}).get("props", {}).get(k) != v}
    if sp:
        print(set_sun(**sp), flush=True)
    if owner.get("box"):
        b = owner["box"]
        props = {k: b["props"][k] for k in BOX_PROPS if k in b["props"] and now["box"]["props"].get(k) != b["props"][k]}
        if props:
            print(set_box(**props), flush=True)
        if max(abs(x - y) for x, y in zip(now["box"]["location"], b["location"])) > 1e-3:
            print(place_box(b["location"]), flush=True)
    d.set_cam(*owner["camera"])
    set_throttle(owner["throttle"])
    d.cmd("ShowFlag.OverrideDiffuseAndSpecular 2")
    time.sleep(SETTLE + 2.0)
    after = state()
    diff = {}
    for k, v in (owner.get("sun") or {}).get("props", {}).items():
        if (after.get("sun") or {}).get("props", {}).get(k) != v:
            diff["sun." + k] = (v, after["sun"]["props"].get(k))
    if owner.get("box"):
        for k in BOX_PROPS:
            v, g = owner["box"]["props"].get(k), after["box"]["props"].get(k)
            if (abs(v - g) > 1e-6) if isinstance(v, float) and isinstance(g, float) else v != g:
                diff["box." + k] = (v, g)
        if max(abs(x - y) for x, y in zip(after["box"]["location"], owner["box"]["location"])) > 1e-3:
            diff["box.location"] = (owner["box"]["location"], after["box"]["location"])
    if after.get("weathers") != owner.get("weathers"):
        diff["weathers"] = (owner.get("weathers"), after.get("weathers"))
    if after.get("hosts") != owner.get("hosts"):
        diff["hosts"] = (owner.get("hosts"), after.get("hosts"))
    oh, ah = owner.get("host") or {}, after.get("host") or {}
    for k in ("layer_km", "start_km"):
        if oh.get(k) is not None and ah.get(k) is not None and json.dumps(oh[k]) != json.dumps(ah[k]):
            diff["host." + k] = (oh[k], ah[k])
    cam = max(abs(x - y) for x, y in zip(after["camera"][0] + after["camera"][1], owner["camera"][0] + owner["camera"][1]))
    if cam > 1e-2:
        diff["camera"] = cam
    if after.get("throttle") != owner.get("throttle"):
        diff["throttle"] = (owner.get("throttle"), after.get("throttle"))
    eng = {k: (v, after["cvars"].get(k)) for k, v in owner["cvars"].items() if abs(v - after["cvars"].get(k, 1e30)) > 1e-6}
    print("RESTORE diff", diff, flush=True)
    print("RESTORE cvars differing from the snapshot (plugin-managed, informational):", eng, flush=True)
    return {"diff": diff, "cvars": eng}


# ------------------------------------------------------------------ frames (criterion 2 pair)
def _movie_frames():
    return set(glob.glob(os.path.join(SS, "MovieFrame[0-9][0-9][0-9][0-9][0-9].png")))


def grab(name, n=6, timeout=120.0):
    out = os.path.join(FRAMES, name)
    shutil.rmtree(out, ignore_errors=True); os.makedirs(out)
    before = _movie_frames()
    d.cmd("r.BufferVisualizationOverviewTargets FogMSNoTarget")
    d.cmd("r.BufferVisualizationDumpFrames 1")
    d.cmd("r.DumpingMovie %d" % n)
    t0 = time.time()
    while time.time() - t0 < timeout and len(_movie_frames() - before) < n:
        time.sleep(0.5)
    time.sleep(1.0)
    d.cmd("r.BufferVisualizationDumpFrames 0"); d.cmd("r.DumpingMovie 0"); d.restore_overview()
    new = sorted(_movie_frames() - before)
    for i, f in enumerate(new):
        shutil.move(f, os.path.join(out, "f%03d.png" % i))
    for f in glob.glob(os.path.join(SS, "MovieFrame*_*.png")):
        try: os.remove(f)
        except Exception: pass
    return out, len(new)


def mean_frame(folder, last=4):
    from PIL import Image
    fs = sorted(glob.glob(os.path.join(folder, "f*.png")))[-last:]
    if not fs:
        raise RuntimeError("no frames in %s (editor minimized?)" % folder)
    return np.mean([np.asarray(Image.open(f).convert("RGB"), dtype=np.float64) for f in fs], axis=0)


def mae(a, b):
    return float(np.abs(a - b).mean())


# ------------------------------------------------------------------ criteria 1, 2, 4, 5
def check(owner):
    res = _load(OUT)
    C = {"started": time.strftime("%Y-%m-%d %H:%M:%S")}
    res["check"] = C
    begin()
    mark = marker("CHECK")
    try:
        st = ensure_rendered(owner)
        C["precondition"] = {"rendered": rendered(st), "lifted": st.get("lifted", False), "host": st.get("host"),
                             "status": (st.get("box") or {}).get("status", "")[-500:]}
        step(res, "check", C)
        # Density frozen for the frame pair (manual animation time), camera = the owner's.
        set_box(use_manual_animation_time=True, manual_animation_time=FROZEN_T)
        d.set_cam(*owner["camera"]); time.sleep(SETTLE + 2.0)
        base = state()
        wait_not_minimized("frames")
        fa, na = grab("clear_none_a")
        # 2. Clear: spawn the actor on Clear -> nothing of the host changes
        m2 = marker("CLEAR")
        spawn_weather("Clear"); time.sleep(SETTLE * 2)
        s2 = state()
        fb, nb = grab("clear_actor")
        text = since(m2)
        ca = ""
        C["A_clear"] = {"test": s2.get("test"), "host_before": base.get("host"), "host_after": s2.get("host"),
                        "cvars_changed": {k: (base["cvars"][k], s2["cvars"][k]) for k in MANAGED if abs(base["cvars"][k] - s2["cvars"][k]) > 1e-6},
                        "log": [l[:300] for l in text.splitlines() if "FogMS Weather" in l or "FogMS cloud host" in l][-8:]}
        # 1. preset change over 10 s: immediate Scattered, then Overcast over 10 s
        d.cmd("FogMS.Weather.Set Scattered 0"); time.sleep(SETTLE)
        m1 = marker("TRANSITION")
        s_start = state(); tex0 = read_map_texels()
        d.cmd("FogMS.Weather.Set Overcast 10")
        time.sleep(5.0)
        s_mid = state(); tex1 = read_map_texels()
        time.sleep(6.5)
        s_end = state(); tex2 = read_map_texels()
        text = since(m1)
        lines = [l[:400] for l in text.splitlines() if "weather:" in l and "FogMS Weather" in l]
        C["B_transition"] = {"log": lines, "status_mid": (s_mid.get("test") or {}).get("status", "")[:400],
                             "status_end": (s_end.get("test") or {}).get("status", "")[:400],
                             "draws": [(s.get("test") or {}).get("draws") for s in (s_start, s_mid, s_end)],
                             "coverage": [(s.get("test") or {}).get("coverage") for s in (s_start, s_mid, s_end)],
                             "deck": [(s.get("test") or {}).get("deck") for s in (s_start, s_mid, s_end)],
                             "wind_cm": [(s.get("test") or {}).get("wind") for s in (s_start, s_mid, s_end)],
                             "texels": [tex0, tex1, tex2],
                             "host_layer_km": [(s.get("host") or {}).get("layer_km") for s in (s_start, s_mid, s_end)],
                             "host_weather_on": (s_end.get("host") or {}).get("weather_on"),
                             "cvars_end": {k: s_end["cvars"][k] for k in MANAGED}}
        B = C["B_transition"]
        B["ok"] = (any("-> Overcast (10 s)" in l for l in lines) and "%" in B["status_mid"]
                   and (B["draws"][2] or 0) > (B["draws"][0] or 0) + 2 and (B["coverage"][0] or 0) < (B["coverage"][1] or 0) < (B["coverage"][2] or 0) + 1e-6
                   and tex0 != tex2 and B["wind_cm"][0] != B["wind_cm"][2])
        step(res, "check", C)
        # 4. delete the actor: the host layer and the cvars come back
        m4 = marker("DELETE")
        delete_weather(); time.sleep(SETTLE * 2)
        s4 = state()
        text = since(m4)
        C["C_delete"] = {"log": [l[:400] for l in text.splitlines() if "FogMS Weather" in l or "FogMS cloud host" in l][-10:],
                         "host_before": base.get("host"), "host_after": s4.get("host"), "hosts": s4.get("hosts"), "weathers": s4.get("weathers"),
                         "cvars_back": {k: (base["cvars"][k], s4["cvars"][k]) for k in MANAGED}}
        D = C["C_delete"]
        hb, ha = base.get("host") or {}, s4.get("host") or {}
        D["ok"] = (not s4.get("weathers") and hb.get("layer_km") is not None
                   and max(abs(x - y) for x, y in zip(hb["layer_km"], ha.get("layer_km") or [9e9, 9e9])) < 0.006
                   and all(abs(a - b) < 1e-6 for a, b in D["cvars_back"].values())
                   and any("stopped" in l for l in D["log"]))
        # 2 (frames): repeat without the actor = the noise floor
        fc, nc = grab("clear_none_b")
        A_, B_, C_ = mean_frame(fa), mean_frame(fb), mean_frame(fc)
        A = C["A_clear"]
        A.update({"frames": [na, nb, nc], "mae_clear_vs_none_a": mae(B_, A_), "mae_clear_vs_none_b": mae(B_, C_), "floor_none_a_vs_b": mae(C_, A_)})
        A["ok"] = ((A["host_before"] or {}).get("layer_km") == (A["host_after"] or {}).get("layer_km") and not A["cvars_changed"]
                   and (A["host_after"] or {}).get("weather_on") in (0.0, None)
                   and max(A["mae_clear_vs_none_a"], A["mae_clear_vs_none_b"]) <= max(2.0 * A["floor_none_a_vs_b"], A["floor_none_a_vs_b"] + 0.1))
        step(res, "check", C)
    finally:
        text = since(mark)
        lines = text.splitlines()
        C["F_log"] = {"python_errors": text.count("LogPython: Error"),
                      "ensures": [l[:300] for l in lines if "Ensure condition failed" in l][:5],
                      "material_errors": [l[:300] for l in lines if "Failed to compile Material" in l][:5],
                      "multilobespec_errors": [l[:300] for l in lines if "LogMultiLobeSpec: Error" in l][:10],
                      "warnings": [l[:300] for l in lines if "LogMultiLobeSpec: Warning" in l and "Weather" in l][:10]}
        C["F_log"]["ok"] = not (C["F_log"]["python_errors"] or C["F_log"]["ensures"] or C["F_log"]["material_errors"] or C["F_log"]["multilobespec_errors"])
        step(res, "check", C)
    return res


# ------------------------------------------------------------------ criterion 6
ROW = re.compile(r".*[\u2502\u250a]\s*([\d.]+) ms\s*\u2503\s*(\S.*?)\s*\u2503")


def profile(label):
    mark = marker("PROFILE_%s" % label)
    d.cmd("r.ProfileGPU.ShowUI 0"); d.cmd("ProfileGPU")
    time.sleep(6.0)
    lines = since(mark).splitlines()
    starts = [i for i, l in enumerate(lines) if "GPU Profile for Frame" in l]
    if not starts:
        return {"error": "no profile block"}
    block = []
    for l in lines[starts[0]: starts[0] + 6000]:
        if "LogRHI" not in l:
            break
        block.append(l)
    frame = next((float(re.search(r"Frame Time\s*:\s*([\d.]+)ms", l).group(1)) for l in block if "Frame Time" in l), None)
    rows = [(float(m.group(1)), m.group(2)) for m in (ROW.search(l) for l in block) if m]
    if len(rows) < 100 or not any(n == "Scene" for _, n in rows):
        return {"error": "no scene passes in the profile (%d events): viewport not rendering, editor minimized?" % len(rows)}
    tot = lambda names: round(sum(ms for ms, n in rows if n in names), 3)
    return {"frame_ms": frame, "cloud_trace": tot({"VolumetricCloud"}), "shadow": tot({"VolumetricCloudShadow"}),
            "cloud_shadow": tot({"CloudShadow"}), "reconstruct": tot({"VolCloudReconstruction"}), "compose": tot({"VolCloudComposeOverScene"}),
            "events": len(rows)}


def profile3(label):
    runs = []
    for i in range(3):
        flood_check()
        wait_not_minimized("cost")
        p = profile("%s_%d" % (label, i))
        if "error" in p:
            p = profile("%s_%d_retry" % (label, i))
        if "error" in p:
            raise RuntimeError(p["error"])
        runs.append(p)
        print("COST %-14s trace %.3f shadow %.3f frame %s" % (label, p["cloud_trace"], p["shadow"], p["frame_ms"]), flush=True)
    med = {k: round(sorted(r[k] for r in runs)[1], 3) for k in ("cloud_trace", "shadow", "cloud_shadow", "reconstruct", "compose")}
    fr = sorted(r["frame_ms"] for r in runs if r["frame_ms"] is not None)
    med["frame_ms"] = fr[len(fr) // 2] if fr else None
    return {"median": med, "runs": runs}


def cost(owner):
    res = _load(OUT)
    begin()
    K = {"started": time.strftime("%Y-%m-%d %H:%M:%S")}
    res["cost"] = K
    try:
        st = ensure_rendered(owner)
        K["rendered"] = rendered(st)
        d.cmd("FogMS.Weather.SetupShadows"); time.sleep(SETTLE)
        d.set_cam(*owner["camera"]); time.sleep(SETTLE + 2.0)
        delete_weather(); time.sleep(SETTLE)
        K["none"] = profile3("none")
        step(res, "cost", K)
        spawn_weather("Overcast", "EXTENDED"); time.sleep(SETTLE * 3)
        s = state()
        K["extended_state"] = {"host": s.get("host"), "status": (s.get("test") or {}).get("status", "")[:500],
                               "skip": s["cvars"]["r.VolumetricCloud.StepSizeOnZeroConservativeDensity"]}
        K["extended"] = profile3("extended")
        step(res, "cost", K)
        # A larger empty-step skip for the extended layer (the plugin cvar; the Box's conservative margin follows it): does it pass the gate?
        d.cmd("r.FogMS.Weather.SkipSteps 32"); time.sleep(SETTLE * 2)
        s = state()
        K["extended32_state"] = {"skip": s["cvars"]["r.VolumetricCloud.StepSizeOnZeroConservativeDensity"], "margin": (s.get("host") or {}).get("skip_margin")}
        K["extended32"] = profile3("extended32")
        d.cmd("r.FogMS.Weather.SkipSteps 8"); time.sleep(SETTLE)
        step(res, "cost", K)
        set_layer("THIN"); time.sleep(SETTLE * 3)
        s = state()
        K["thin_state"] = {"host": s.get("host"), "status": (s.get("test") or {}).get("status", "")[:500],
                           "skip": s["cvars"]["r.VolumetricCloud.StepSizeOnZeroConservativeDensity"]}
        K["thin"] = profile3("thin")
        n, e, t, e32 = K["none"]["median"], K["extended"]["median"], K["thin"]["median"], K["extended32"]["median"]
        K["delta_visible_extended_ms"] = round(e["cloud_trace"] - n["cloud_trace"], 3)
        K["delta_visible_extended32_ms"] = round(e32["cloud_trace"] - n["cloud_trace"], 3)
        K["delta_visible_thin_ms"] = round(t["cloud_trace"] - n["cloud_trace"], 3)
        K["shadow_ms"] = {"none": n["shadow"], "extended": e["shadow"], "extended32": e32["shadow"], "thin": t["shadow"]}
        K["gate_ms"] = GATE_MS
        K["decision2"] = "extended" if min(K["delta_visible_extended_ms"], K["delta_visible_extended32_ms"]) <= GATE_MS else "thin"
        step(res, "cost", K)
    finally:
        delete_weather()
    return res


# ------------------------------------------------------------------ matedit / copyback
def matedit():
    res = _load(OUT)
    out = {}
    for name in ("matedit_weather.py", "matedit_cloud.py"):
        text = S.run_file(name)
        lines = [l for l in text.splitlines() if l.split(" ")[0] in ("WEATHER_OK", "MATERIAL_OK", "ALREADY_PATCHED", "TRACE", "NOT", "ROLLBACK",
                                                                    "REBUILD", "CREATED", "COMPILE", "LINKS", "REPORT", "NOTE")
                 or l.startswith(("T_FogMS", "M_FogMS", "DA_FogMS", "  File", "RuntimeError"))]
        out[name] = {"lines": lines[-30:], "ok": any(l.startswith(("WEATHER_OK", "MATERIAL_OK", "ALREADY_PATCHED")) for l in lines)}
        print("\n".join(lines), flush=True)
        if not out[name]["ok"]:
            break
    res["matedit"] = out
    _atomic_dump(res, OUT)
    return out


def copyback():
    out = {}
    src_root = os.path.join(PROJECT, "Plugins", "MultiLobeSpec", "Content", "FogMS")
    names = [os.path.join("Weather", os.path.basename(p)) for p in glob.glob(os.path.join(src_root, "Weather", "*.uasset"))]
    for name in names + ["M_FogMS_Cloud.uasset", "MI_FogMS_Cloud.uasset"]:
        src = os.path.join(src_root, name)
        dst = os.path.join(WORKTREE, "Content", "FogMS", name)
        h = lambda p: hashlib.sha256(open(p, "rb").read()).hexdigest()[:16] if os.path.isfile(p) else None
        if not os.path.isfile(src):
            out[name] = "missing in the project"
            continue
        if h(src) == h(dst):
            out[name] = "identical"
            continue
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copy2(src, dst)
        out[name] = "copied (%s, %d bytes)" % (h(dst), os.path.getsize(dst))
    print(json.dumps(out, indent=1), flush=True)
    return out


# ------------------------------------------------------------------ summary
def summary():
    res = _load(OUT)
    C, K = res.get("check", {}), res.get("cost", {})
    rows = [("matedit (weather assets + host material v3)", all(v.get("ok") for v in (res.get("matedit") or {"x": {}}).values())),
            ("1 preset change over 10 s: log, status, map redrawn, wind", (C.get("B_transition") or {}).get("ok")),
            ("2 Clear: layer / cvars unchanged, frame within the floor", (C.get("A_clear") or {}).get("ok")),
            ("3 shadows on the ground (the owner, by eye)", "owner"),
            ("4 actor deleted: layer and cvars back (log)", (C.get("C_delete") or {}).get("ok")),
            ("5 log clean", (C.get("F_log") or {}).get("ok")),
            ("6 visible-pass delta extended <= %.1f ms" % GATE_MS, None if "delta_visible_extended_ms" not in K else K["delta_visible_extended_ms"] <= GATE_MS),
            ("   restore diff empty", not ((res.get("restore") or {}).get("diff")))]
    for name, ok in rows:
        print("%-62s %s" % (name, ok), flush=True)
    A = C.get("A_clear") or {}
    if "mae_clear_vs_none_a" in A:
        print("clear pair: MAE %.3f / %.3f, floor %.3f" % (A["mae_clear_vs_none_a"], A["mae_clear_vs_none_b"], A["floor_none_a_vs_b"]), flush=True)
    if "delta_visible_extended_ms" in K:
        print("cost (owner camera, median of 3): visible trace none %.3f / extended %.3f / extended skip 32 %s / thin %.3f ms; shadow pass %s; decision 2 -> %s" % (
            K["none"]["median"]["cloud_trace"], K["extended"]["median"]["cloud_trace"],
            (K.get("extended32") or {}).get("median", {}).get("cloud_trace"), K["thin"]["median"]["cloud_trace"],
            json.dumps(K["shadow_ms"]), K["decision2"]), flush=True)


def main(cmd):
    if cmd == "summary":
        summary(); return
    if cmd == "copyback":
        copyback(); return
    owner = snapshot(force="--force" in sys.argv and cmd == "snapshot")
    if cmd == "snapshot":
        print(json.dumps(owner, indent=1)[:4000]); return
    if cmd == "matedit":
        matedit(); return
    if cmd == "restore":
        r = restore(owner); res = _load(OUT); res["restore"] = r; _atomic_dump(res, OUT); return
    runs = {"check": [check], "cost": [cost], "all": [check, cost]}.get(cmd)
    if runs is None:
        raise SystemExit("unknown subcommand " + cmd)
    try:
        for fn in runs:
            try:
                fn(owner)
            except Exception as e:
                res = _load(OUT); res.setdefault("errors", []).append("%s: %r" % (fn.__name__, e)); _atomic_dump(res, OUT)
                print("ERROR in %s: %r" % (fn.__name__, e), flush=True)
    finally:
        r = restore(owner)
        res = _load(OUT); res["restore"] = r; _atomic_dump(res, OUT)
        summary()


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "snapshot")
