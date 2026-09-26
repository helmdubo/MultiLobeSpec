# -*- coding: utf-8 -*-
"""Round 49 (W49, FogMS_Weather_Design.md section 6 'W49'): LEAN phase-2 check of the FogMS Weather sky dome in the owner's editor
(owner's rule: status / log checks + ONE GPU number + ONE ambient number; he judges the look himself; no screenshot series; target
<= 30 min editor time). Criteria (design 6 'W49'):
  1. the sky clouds line up with the ground shadows (the owner, by eye: nothing captured here);
  2. Overcast: the sky light of the world and the fog greys and darkens (the owner) - here: the Box status reads
     '[sky: SH (Real Time Capture, weather clouds)]' while the dome shows, and the AMBIENT NUMBER below;
  3. without the actor (and with Clear) the sky and the status are as before (no dome component shows, the Box's sky source is the
     snapshot's);
  4. the hero cloud of the host draws over the dome without seams / halos (the owner; the dome keeps the far depth, so the host composes
     over it exactly as over the atmosphere's own sky);
  5. the NUMBER: the dome pass (ProfileGPU event SkyPassParallel / SkyPass: the only draw of an Is Sky mesh in the main view) at the owner
     camera <= 1 ms at 1080p equivalent (x 1920*1080 / the traced view size of the profile); above it: the panorama-RT fallback is the
     next slice.
Plus: the dome is in the SkyLight's Real Time Capture (the ambient probe darkens with the dome on vs off under the same Overcast), the
log is clean (no LogPython errors, ensures, 'Failed to compile Material', LogMultiLobeSpec errors).

AMBIENT NUMBER (probe): a transient white sphere (/Engine/BasicShapes/Sphere + BasicShapeMaterial, Visible In Scene Capture Only, no
shadow) 3 m in front of the owner camera, seen by a transient SceneCapture2D (32^2 RGBA16F, Scene Color HDR, directional lights OFF, GI
method None: the sphere is lit by the SkyLight's capture only, plus local lights); its mean linear luminance for: A no weather, B Overcast
with the dome, C Overcast with r.FogMS.Weather.SkyDome 0 (weather shadows, no dome), D no weather again (noise floor). Reported: B/A (the
overcast ambient vs clear), B/C (the dome's share: < 1 = the capture holds the dome), D/A (floor).

Subcommands (python d49_sky.py <sub>; FOGMS_LOG = the running editor's log, required):
  snapshot [--force]  measure/owner_pre49.json (READ ONLY; written once: the restore target)
  matedit             matedit_weather.py (builds M_FogMS_WeatherSky; the W48 assets stay ALREADY_PATCHED)
  copyback            the saved M_FogMS_WeatherSky (+ any changed weather asset) from the project plugin into this worktree (files only)
  check               criteria 2 (status), 3, the capture probe, the log
  cost                criterion 5
  all                 check + cost, then restore and summary (one command)
  restore             restore the snapshot
  summary             verdicts from results/diag49/d49.json
Rules: the map is never saved; no tick callbacks, no mark_render_state_dirty; engine cvars are not set from the console (only the plugin's
r.FogMS.Weather.SkyDome, set back to its snapshot value); the owner's own FogMS Weather actors are disabled for the run (Enabled off) and
enabled again; the test weather actor, the probe sphere and the scene capture are deleted at the end; throttle off during the run and
restored; ShowFlag.OverrideDiffuseAndSpecular 0 (Lit) during the run and 2 (the viewport's own mode, VMI_Lit in the ini) after; ProfileGPU
waits while the editor is minimized; a LogPython error flood stops the run; the probe's render target is exported to measure/diag49
(junction to D:/FogMS_ProbeFrames/diag49); results/diag49/d49.json written atomically after every step."""
import os, sys, json, time, re, glob, shutil, hashlib, subprocess
import diag34lib as d
import cloudproto_session as S

HERE = d.HERE
RES = os.path.join(HERE, "results", "diag49")
os.makedirs(RES, exist_ok=True)
OUT = os.path.join(RES, "d49.json")
OWNER = os.path.join(HERE, "measure", "owner_pre49.json")
FRAMES = os.path.join(HERE, "measure", "diag49")          # junction -> D:/FogMS_ProbeFrames/diag49
FRAMES_TARGET = "D:/FogMS_ProbeFrames/diag49"
LOG = os.environ.get("FOGMS_LOG", "")
S.LOG = LOG
PROJECT = "D:/PersonalProjects/UE5/MimirHead_portfolio 5.7 5.8 - 3"
WORKTREE = os.path.normpath(os.path.join(HERE, "..", "..", ".."))
BOX_LABEL = "FogMS - Live Box"
TEST_LABEL = "FogMS Weather (d49 test)"
PROBE_LABEL = "FogMS d49 ambient probe"
CAPTURE_LABEL = "FogMS d49 ambient capture"
WEATHER_DIR = "/MultiLobeSpec/FogMS/Weather"
SKY_MATERIAL = WEATHER_DIR + "/M_FogMS_WeatherSky"
SETTLE = 3.0
CAPTURE_SETTLE = 4.0       # the time-sliced Real Time Capture (12 steps, ~16 frames) + convolution, with margin
GATE_MS = 1.0
SUN_PROPS = ["cast_cloud_shadows", "cloud_shadow_extent", "cloud_shadow_map_resolution_scale", "cloud_shadow_ray_sample_count_scale"]
CVARS = ["r.FogMS.Weather.SkyDome", "r.FogMS.World.SkySource", "r.SkyLight.RealTimeReflectionCapture", "r.SkyLight.RealTimeReflectionCapture.TimeSlice"]
_state = {"flood0": None}

FIND = ("import unreal, json\n"
        "_A=unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()\n"
        "A={a.get_actor_label():a for a in _A}\n"
        "box=A.get(%r)\n"
        "weathers=[a for a in _A if a.get_class().get_name()=='FogMSWeather']\n"
        "test=A.get(%r)\n"
        "probe=A.get(%r)\n"
        "capture=A.get(%r)\n"
        "_suns=[]\n"
        "for a in _A:\n"
        "    if isinstance(a, unreal.DirectionalLight):\n"
        "        c=a.get_component_by_class(unreal.DirectionalLightComponent)\n"
        "        if c and c.get_editor_property('atmosphere_sun_light') and c.get_editor_property('atmosphere_sun_light_index')==0: _suns.append((a,c))\n"
        "sunA,sun=(_suns[0] if _suns else (None,None))\n"
        "skylights=[a for a in _A if isinstance(a, unreal.SkyLight)]\n"
        "_skym=unreal.load_asset(%r)\n"
        "def _base(m):\n"
        "    while m is not None and isinstance(m, unreal.MaterialInstance): m=m.get_editor_property('parent')\n"
        "    return m\n"
        "def domes():\n"
        "    out=[]\n"
        "    for a in _A:\n"
        "        for c in a.get_components_by_class(unreal.StaticMeshComponent):\n"
        "            ms=[c.get_material(i) for i in range(max(c.get_num_materials(),1))]\n"
        "            if _skym is not None and any(_base(m)==_skym for m in ms if m is not None): out.append((a.get_actor_label(), bool(c.is_visible())))\n"
        "    return out\n") % (BOX_LABEL, TEST_LABEL, PROBE_LABEL, CAPTURE_LABEL, SKY_MATERIAL)


# ------------------------------------------------------------------ plumbing (d48_weather.py pattern)
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
        import ctypes
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
    m = "D49_%s_%d" % (tag, int(time.time() * 1000))
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


def ensure_frames_dir():
    """measure/diag49 = a junction to D:/FogMS_ProbeFrames/diag49 (E: is nearly full; the repo keeps only results/diag49)."""
    if not os.path.isdir(FRAMES_TARGET):
        os.makedirs(FRAMES_TARGET, exist_ok=True)
    if not os.path.exists(FRAMES):
        subprocess.run(["cmd", "/c", "mklink", "/J", FRAMES.replace("/", "\\"), FRAMES_TARGET.replace("/", "\\")], capture_output=True)
    return os.path.isdir(FRAMES)


def sky_source(status):
    m = re.search(r"\[sky: ([^\]]*)", status or "")
    return m.group(1) if m else None


# ------------------------------------------------------------------ state (read only)
def state():
    code = FIND + (
        "les=unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem)\n"
        "l,r=les.get_level_viewport_camera_info()\n"
        "perf=unreal.get_default_object(unreal.load_class(None, '/Script/UnrealEd.EditorPerformanceSettings'))\n"
        "def _v(x):\n"
        "    return str(x) if not isinstance(x, (bool, int, float)) else x\n"
        "out={'camera':[[l.x,l.y,l.z],[r.pitch,r.yaw,r.roll]], 'throttle': bool(perf.get_editor_property('bThrottleCPUWhenNotForeground')),\n"
        " 'weathers':[{'label':w.get_actor_label(),'enabled':bool(w.get_editor_property('enabled')),\n"
        "              'dome_active':bool(w.get_editor_property('sky_dome_active')),'status':str(w.get_editor_property('weather_status'))[-700:]} for w in weathers],\n"
        " 'domes':domes(), 'sky_material': _skym is not None}\n"
        "if box:\n"
        "    loc=box.get_actor_location()\n"
        "    out['box']={'location':[loc.x,loc.y,loc.z],'status':str(box.get_editor_property('spatial_status'))}\n"
        "if sun:\n"
        "    out['sun']={'label':sunA.get_actor_label(),'props':{p:_v(sun.get_editor_property(p)) for p in %r}}\n"
        "out['skylights']=[{'label':s.get_actor_label(),'rtc':bool(s.get_component_by_class(unreal.SkyLightComponent).get_editor_property('real_time_capture'))} for s in skylights]\n"
        "out['cvars']={n: unreal.SystemLibrary.get_console_variable_float_value(n) for n in %r}\n"
        "print('D49STATE '+json.dumps(out))") % (SUN_PROPS, CVARS)
    return pyj(code, "D49STATE")


def snapshot(force=False):
    if os.path.isfile(OWNER) and not force:
        return json.load(open(OWNER))
    s = state()
    s["note"] = "round 49 snapshot, read before any change of the run"
    s["time"] = time.strftime("%Y-%m-%d %H:%M:%S")
    _atomic_dump(s, OWNER)
    return s


# ------------------------------------------------------------------ changes (all undone by restore)
def set_throttle(flag):
    return d.py("import unreal\nperf=unreal.get_default_object(unreal.load_class(None, '/Script/UnrealEd.EditorPerformanceSettings'))\n"
                "perf.set_editor_property('bThrottleCPUWhenNotForeground', %s)\nprint('THROTTLE', perf.get_editor_property('bThrottleCPUWhenNotForeground'))" % bool(flag))


def set_owner_weathers(enabled_by_label):
    """The owner's FogMS Weather actors (not the test actor) get Enabled = the given value (restore: the snapshot's)."""
    code = FIND + "".join("w=A.get(%r)\nif w is not None: w.set_editor_property('enabled', %s)\n" % (label, bool(on))
                          for label, on in enabled_by_label.items()) + "print('OWNER_WEATHERS', [(w.get_actor_label(), w.get_editor_property('enabled')) for w in weathers])"
    return d.py(code)


def spawn_weather(preset):
    """The test actor at the Box's XY (the weather domain centre) on a plugin preset; it never creates a cloud host (the dome needs none)."""
    code = FIND + (
        "sub=unreal.get_editor_subsystem(unreal.EditorActorSubsystem)\n"
        "if test is None:\n"
        "    p=box.get_actor_location() if box else unreal.Vector(0,0,0)\n"
        "    test=sub.spawn_actor_from_class(unreal.FogMSWeather, unreal.Vector(p.x, p.y, 0.0))\n"
        "    test.set_actor_label(%r)\n"
        "    test.set_editor_property('create_cloud_host', False)\n"
        "test.set_editor_property('weather_state', unreal.load_asset(%r))\n"
        "print('SPAWNED', test.get_actor_label(), test.get_editor_property('weather_state'))") % (
        TEST_LABEL, "%s/DA_FogMS_Weather_%s" % (WEATHER_DIR, preset))
    return d.py(code)


def delete_labels(labels):
    return d.py(FIND + "n=0\nfor a in [a for a in _A if a.get_actor_label() in %r]:\n"
                "    unreal.get_editor_subsystem(unreal.EditorActorSubsystem).destroy_actor(a); n+=1\nprint('DELETED', n)" % (list(labels),))


def set_dome_cvar(value):
    d.cmd("r.FogMS.Weather.SkyDome %d" % int(value))


def begin():
    if not LOG or not os.path.isfile(LOG):
        raise SystemExit("Set FOGMS_LOG to the running editor's log file.")
    wait_not_minimized("begin", 300.0)
    flood_check()
    set_throttle(False)
    d.cmd("ShowFlag.OverrideDiffuseAndSpecular 0")


def restore(owner):
    delete_labels([TEST_LABEL, PROBE_LABEL, CAPTURE_LABEL])
    time.sleep(SETTLE)
    if abs(owner["cvars"].get("r.FogMS.Weather.SkyDome", 1.0) - state()["cvars"].get("r.FogMS.Weather.SkyDome", 1.0)) > 1e-6:
        set_dome_cvar(owner["cvars"]["r.FogMS.Weather.SkyDome"])
    set_owner_weathers({w["label"]: w["enabled"] for w in owner.get("weathers", [])})
    d.set_cam(*owner["camera"])
    set_throttle(owner["throttle"])
    d.cmd("ShowFlag.OverrideDiffuseAndSpecular 2")
    time.sleep(SETTLE + 2.0)
    after = state()
    diff = {}
    ow = {w["label"]: w["enabled"] for w in owner.get("weathers", [])}
    aw = {w["label"]: w["enabled"] for w in after.get("weathers", [])}
    if ow != aw:
        diff["weathers"] = (ow, aw)
    for k, v in (owner.get("sun") or {}).get("props", {}).items():
        if (after.get("sun") or {}).get("props", {}).get(k) != v:
            diff["sun." + k] = (v, after["sun"]["props"].get(k))
    if owner.get("skylights") != after.get("skylights"):
        diff["skylights"] = (owner.get("skylights"), after.get("skylights"))
    if sorted(map(tuple, owner.get("domes", []))) != sorted(map(tuple, after.get("domes", []))):
        diff["domes"] = (owner.get("domes"), after.get("domes"))
    cam = max(abs(x - y) for x, y in zip(after["camera"][0] + after["camera"][1], owner["camera"][0] + owner["camera"][1]))
    if cam > 1e-2:
        diff["camera"] = cam
    if after.get("throttle") != owner.get("throttle"):
        diff["throttle"] = (owner.get("throttle"), after.get("throttle"))
    eng = {k: (v, after["cvars"].get(k)) for k, v in owner["cvars"].items() if abs(v - after["cvars"].get(k, 1e30)) > 1e-6}
    if eng:
        diff["cvars"] = eng
    if owner.get("box") and after.get("box") and sky_source(owner["box"]["status"]) != sky_source(after["box"]["status"]):
        diff["box_sky_source"] = (sky_source(owner["box"]["status"]), sky_source(after["box"]["status"]))
    print("RESTORE diff", diff, flush=True)
    return {"diff": diff}


# ------------------------------------------------------------------ ambient probe
def probe_setup(owner):
    """The probe sphere 3 m in front of the owner camera and a scene capture at the camera looking at it (both transient test actors)."""
    (lx, ly, lz), (pitch, yaw, roll) = owner["camera"]
    code = FIND + (
        "sub=unreal.get_editor_subsystem(unreal.EditorActorSubsystem)\n"
        "rot=unreal.Rotator(pitch=%r, yaw=%r, roll=0.0)\n"
        "cam=unreal.Vector(%r, %r, %r)\n"
        "fwd=rot.get_forward_vector()\n"
        "if probe is None:\n"
        "    probe=sub.spawn_actor_from_class(unreal.StaticMeshActor, cam + fwd * 300.0, rot)\n"
        "    probe.set_actor_label(%r)\n"
        "    smc=probe.static_mesh_component\n"
        "    smc.set_mobility(unreal.ComponentMobility.MOVABLE)\n"
        "    smc.set_static_mesh(unreal.load_asset('/Engine/BasicShapes/Sphere'))\n"
        "    smc.set_material(0, unreal.load_asset('/Engine/BasicShapes/BasicShapeMaterial'))\n"
        "    smc.set_editor_property('visible_in_scene_capture_only', True)\n"
        "    smc.set_editor_property('cast_shadow', False)\n"
        "if capture is None:\n"
        "    capture=sub.spawn_actor_from_class(unreal.SceneCapture2D, cam, rot)\n"
        "    capture.set_actor_label(%r)\n"
        "w=unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()\n"
        "rt=unreal.RenderingLibrary.create_render_target2d(w, 32, 32, unreal.TextureRenderTargetFormat.RTF_RGBA16F)\n"
        "cc=capture.capture_component2d\n"
        "cc.set_editor_property('texture_target', rt)\n"
        "cc.set_editor_property('capture_source', unreal.SceneCaptureSource.SCS_SCENE_COLOR_HDR)\n"
        "cc.set_editor_property('capture_every_frame', False)\n"
        "cc.set_editor_property('capture_on_movement', False)\n"
        "cc.set_editor_property('fov_angle', 12.0)\n"
        "cc.set_editor_property('show_flag_settings', [unreal.EngineShowFlagsSetting(show_flag_name='DirectionalLights', enabled=False)])\n"
        "pps=cc.get_editor_property('post_process_settings')\n"
        "pps.set_editor_property('override_dynamic_global_illumination_method', True)\n"
        "pps.set_editor_property('dynamic_global_illumination_method', unreal.DynamicGlobalIlluminationMethod.NONE)\n"
        "cc.set_editor_property('post_process_settings', pps)\n"
        "cc.set_editor_property('post_process_blend_weight', 1.0)\n"
        "print('PROBE', probe.get_actor_label(), capture.get_actor_label())") % (pitch, yaw, lx, ly, lz, PROBE_LABEL, CAPTURE_LABEL)
    return d.py(code)


def probe_read(tag):
    """One capture of the probe: mean linear luminance of the central 12 x 12 texels (the sphere fills the 12-degree view)."""
    out_dir = FRAMES if ensure_frames_dir() else RES
    code = FIND + (
        "w=unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()\n"
        "cc=capture.capture_component2d\n"
        "rt=cc.get_editor_property('texture_target')\n"
        "cc.capture_scene()\n"
        "px=[unreal.RenderingLibrary.read_render_target_raw_pixel(w, rt, x, y, False) for y in range(10, 22) for x in range(10, 22)]\n"
        "lum=[0.2126*c.r+0.7152*c.g+0.0722*c.b for c in px]\n"
        "try:\n"
        "    unreal.RenderingLibrary.export_render_target(w, rt, %r, %r)\n"
        "except Exception as e:\n"
        "    print('EXPORT failed', e)\n"
        "print('D49PROBE '+json.dumps({'mean': sum(lum)/len(lum), 'min': min(lum), 'max': max(lum), 'n': len(lum)}))") % (
        out_dir.replace("\\", "/"), "probe_%s" % tag)
    return pyj(code, "D49PROBE")


def probe(res, C, owner):
    """The ambient number: A no weather, B Overcast + dome, C Overcast without the dome (r.FogMS.Weather.SkyDome 0), D no weather again."""
    P = {}
    C["probe"] = P
    probe_setup(owner)
    delete_labels([TEST_LABEL]); time.sleep(CAPTURE_SETTLE)
    P["A_none"] = probe_read("A_none")
    step(res, "check", C)
    spawn_weather("Overcast"); d.cmd("FogMS.Weather.Set Overcast 0"); time.sleep(CAPTURE_SETTLE)
    P["B_overcast_dome"] = probe_read("B_overcast_dome")
    set_dome_cvar(0); time.sleep(CAPTURE_SETTLE)
    P["C_overcast_nodome"] = probe_read("C_overcast_nodome")
    set_dome_cvar(1)
    delete_labels([TEST_LABEL]); time.sleep(CAPTURE_SETTLE)
    P["D_none"] = probe_read("D_none")
    a, b, c, dd = (P[k]["mean"] for k in ("A_none", "B_overcast_dome", "C_overcast_nodome", "D_none"))
    P["overcast_vs_clear"] = b / a if a > 0 else None
    P["dome_share"] = b / c if c > 0 else None
    P["floor"] = dd / a if a > 0 else None
    fl = abs(1.0 - (P["floor"] or 0.0))
    P["capture_holds_dome"] = P["dome_share"] is not None and P["dome_share"] < 1.0 - max(3.0 * fl, 0.03)
    step(res, "check", C)
    delete_labels([PROBE_LABEL, CAPTURE_LABEL])


# ------------------------------------------------------------------ criteria 2, 3, probe, log
def check(owner):
    res = _load(OUT)
    C = {"started": time.strftime("%Y-%m-%d %H:%M:%S")}
    res["check"] = C
    begin()
    mark = marker("CHECK")
    try:
        # the owner's weather actors off for the run (their dome hides), the viewport camera = the owner's
        set_owner_weathers({w["label"]: False for w in owner.get("weathers", []) if w["label"] != TEST_LABEL})
        d.set_cam(*owner["camera"]); time.sleep(CAPTURE_SETTLE)
        base = state()
        C["A_none"] = {"domes": base.get("domes"), "sky_source": sky_source((base.get("box") or {}).get("status")),
                       "sky_material": base.get("sky_material"), "skylights": base.get("skylights")}
        C["A_none"]["ok"] = not any(v for _, v in (base.get("domes") or [])) and "weather clouds" not in (C["A_none"]["sky_source"] or "")
        step(res, "check", C)
        # Overcast (immediate): the dome shows, the Box's sky source is the capture's SH
        m = marker("OVERCAST")
        spawn_weather("Overcast"); d.cmd("FogMS.Weather.Set Overcast 0"); time.sleep(CAPTURE_SETTLE)
        s = state()
        t = next((w for w in s.get("weathers", []) if w["label"] == TEST_LABEL), {})
        text = since(m)
        C["B_overcast"] = {"dome_active": t.get("dome_active"), "domes": s.get("domes"), "status": t.get("status", "")[-600:],
                           "sky_source": sky_source((s.get("box") or {}).get("status")),
                           "log": [l[:400] for l in text.splitlines() if "sky dome" in l or "FogMS Weather" in l][-8:]}
        B = C["B_overcast"]
        B["ok"] = (bool(B["dome_active"]) and any(lbl == TEST_LABEL and vis for lbl, vis in (B["domes"] or []))
                   and "sky dome on" in B["status"] and (B["sky_source"] or "").startswith("SH (Real Time Capture, weather clouds)")
                   and any("sky dome on" in l for l in B["log"]))
        step(res, "check", C)
        # Clear: the dome goes, the sky source is the snapshot's again
        m = marker("CLEAR")
        d.cmd("FogMS.Weather.Set Clear 0"); time.sleep(CAPTURE_SETTLE)
        s = state()
        t = next((w for w in s.get("weathers", []) if w["label"] == TEST_LABEL), {})
        text = since(m)
        C["C_clear"] = {"dome_active": t.get("dome_active"), "domes": s.get("domes"), "sky_source": sky_source((s.get("box") or {}).get("status")),
                        "log": [l[:400] for l in text.splitlines() if "sky dome" in l][-4:]}
        Cc = C["C_clear"]
        Cc["ok"] = (not Cc["dome_active"] and not any(v for _, v in (Cc["domes"] or []))
                    and Cc["sky_source"] == C["A_none"]["sky_source"] and any("sky dome off" in l for l in Cc["log"]))
        step(res, "check", C)
        # delete the actor: nothing left, the sky source as before
        m = marker("DELETE")
        delete_labels([TEST_LABEL]); time.sleep(CAPTURE_SETTLE)
        s = state()
        C["D_delete"] = {"weathers": [w["label"] for w in s.get("weathers", [])], "domes": s.get("domes"),
                         "sky_source": sky_source((s.get("box") or {}).get("status"))}
        Dd = C["D_delete"]
        Dd["ok"] = TEST_LABEL not in Dd["weathers"] and not any(v for _, v in (Dd["domes"] or [])) and Dd["sky_source"] == C["A_none"]["sky_source"]
        step(res, "check", C)
        # the ambient probe (also proves the capture holds the dome)
        probe(res, C, owner)
    finally:
        text = since(mark)
        lines = text.splitlines()
        C["F_log"] = {"python_errors": text.count("LogPython: Error"),
                      "ensures": [l[:300] for l in lines if "Ensure condition failed" in l][:5],
                      "material_errors": [l[:300] for l in lines if "Failed to compile Material" in l][:5],
                      "multilobespec_errors": [l[:300] for l in lines if "LogMultiLobeSpec: Error" in l][:10],
                      "warnings": [l[:300] for l in lines if "LogMultiLobeSpec: Warning" in l and ("Weather" in l or "sky" in l)][:10]}
        C["F_log"]["ok"] = not (C["F_log"]["python_errors"] or C["F_log"]["ensures"] or C["F_log"]["material_errors"] or C["F_log"]["multilobespec_errors"])
        step(res, "check", C)
    return res


# ------------------------------------------------------------------ criterion 5
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
    for l in lines[starts[0]: starts[0] + 8000]:
        if "LogRHI" not in l:
            break
        block.append(l)
    frame = next((float(re.search(r"Frame Time\s*:\s*([\d.]+)ms", l).group(1)) for l in block if "Frame Time" in l), None)
    rows = [(float(m.group(1)), m.group(2)) for m in (ROW.search(l) for l in block) if m]
    if len(rows) < 100 or not any(n == "Scene" for _, n in rows):
        return {"error": "no scene passes in the profile (%d events): viewport not rendering, editor minimized?" % len(rows)}
    tot = lambda names: round(sum(ms for ms, n in rows if n in names), 3)
    # the traced view size: TSR 'W x H -> W2 x H2' (the render resolution before upscaling), else the tonemapper output
    size = None
    for _, n in rows:
        mt = re.search(r"TemporalSuperResolution.*?(\d+)x(\d+)\s*->\s*(\d+)x(\d+)", n)
        if mt:
            size = [int(mt.group(1)), int(mt.group(2))]; break
    if size is None:
        for _, n in rows:
            mt = re.search(r"Tonemap (\d+)x(\d+)", n)
            if mt:
                size = [int(mt.group(1)), int(mt.group(2))]; break
    return {"frame_ms": frame, "dome": tot({"SkyPassParallel", "SkyPass"}), "capture_sky": tot({"CaptureSkyMeshReflection"}),
            "cloud_trace": tot({"VolumetricCloud"}), "view_size": size, "events": len(rows)}


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
        print("COST %-10s dome %.3f capture %.3f frame %s size %s" % (label, p["dome"], p["capture_sky"], p["frame_ms"], p["view_size"]), flush=True)
    med = {k: round(sorted(r[k] for r in runs)[1], 3) for k in ("dome", "capture_sky", "cloud_trace")}
    fr = sorted(r["frame_ms"] for r in runs if r["frame_ms"] is not None)
    med["frame_ms"] = fr[len(fr) // 2] if fr else None
    med["view_size"] = runs[-1]["view_size"]
    return {"median": med, "runs": runs}


def cost(owner):
    res = _load(OUT)
    begin()
    K = {"started": time.strftime("%Y-%m-%d %H:%M:%S")}
    res["cost"] = K
    try:
        set_owner_weathers({w["label"]: False for w in owner.get("weathers", []) if w["label"] != TEST_LABEL})
        d.set_cam(*owner["camera"])
        delete_labels([TEST_LABEL]); time.sleep(SETTLE)
        K["none"] = profile3("none")
        step(res, "cost", K)
        spawn_weather("Scattered"); d.cmd("FogMS.Weather.Set Scattered 0"); time.sleep(SETTLE * 2)
        K["scattered"] = profile3("scattered")
        step(res, "cost", K)
        d.cmd("FogMS.Weather.Set Overcast 0"); time.sleep(SETTLE * 2)
        K["overcast"] = profile3("overcast")
        worst = max(K["scattered"]["median"]["dome"], K["overcast"]["median"]["dome"])
        size = K["overcast"]["median"]["view_size"] or K["scattered"]["median"]["view_size"]
        K["dome_ms"] = worst
        K["view_size"] = size
        K["dome_ms_1080p"] = round(worst * 1920.0 * 1080.0 / (size[0] * size[1]), 3) if size else None
        K["gate_ms"] = GATE_MS
        K["pass"] = K["dome_ms_1080p"] is not None and K["dome_ms_1080p"] <= GATE_MS
        K["next"] = None if K["pass"] else "panorama render target (a Sky View LUT-like 2D texture of the weather sky, updated at low rate)"
        step(res, "cost", K)
    finally:
        delete_labels([TEST_LABEL])
    return res


# ------------------------------------------------------------------ matedit / copyback
def matedit():
    res = _load(OUT)
    text = S.run_file("matedit_weather.py")
    lines = [l for l in text.splitlines() if l.split(" ")[0] in ("WEATHER_OK", "ALREADY_PATCHED", "TRACE", "NOT", "ROLLBACK", "REBUILD", "CREATED",
                                                                 "COMPILE", "REPORT", "NOTE")
             or l.startswith(("T_FogMS", "M_FogMS", "DA_FogMS", "  File", "RuntimeError"))]
    out = {"lines": lines[-40:], "ok": any(l.startswith(("WEATHER_OK", "ALREADY_PATCHED")) for l in lines)}
    print("\n".join(lines), flush=True)
    res["matedit"] = out
    _atomic_dump(res, OUT)
    return out


def copyback():
    out = {}
    src_root = os.path.join(PROJECT, "Plugins", "MultiLobeSpec", "Content", "FogMS", "Weather")
    h = lambda p: hashlib.sha256(open(p, "rb").read()).hexdigest()[:16] if os.path.isfile(p) else None
    for src in glob.glob(os.path.join(src_root, "*.uasset")):
        name = os.path.basename(src)
        dst = os.path.join(WORKTREE, "Content", "FogMS", "Weather", name)
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
    P = C.get("probe") or {}
    rows = [("matedit (M_FogMS_WeatherSky)", (res.get("matedit") or {}).get("ok")),
            ("1 sky clouds over their ground shadows (the owner, by eye)", "owner"),
            ("2 Overcast: dome on, Box '[sky: SH (Real Time Capture, weather clouds)]'", (C.get("B_overcast") or {}).get("ok")),
            ("3 no actor / Clear / deleted: no dome, sky source as before",
             None if "A_none" not in C else bool((C.get("A_none") or {}).get("ok") and (C.get("C_clear") or {}).get("ok") and (C.get("D_delete") or {}).get("ok"))),
            ("4 hero cloud over the dome without seams (the owner)", "owner"),
            ("5 dome pass <= %.1f ms at 1080p" % GATE_MS, K.get("pass")),
            ("   the capture holds the dome (probe: dome on < dome off)", P.get("capture_holds_dome")),
            ("   log clean", (C.get("F_log") or {}).get("ok")),
            ("   restore diff empty", not ((res.get("restore") or {}).get("diff")))]
    for name, ok in rows:
        print("%-72s %s" % (name, ok), flush=True)
    if P.get("overcast_vs_clear") is not None:
        print("ambient probe (sky light only, linear): overcast/clear %.3f (drop %.0f %%), dome on/off under the same overcast %.3f, floor %.3f" % (
            P["overcast_vs_clear"], 100.0 * (1.0 - P["overcast_vs_clear"]), P["dome_share"] or 0.0, P["floor"] or 0.0), flush=True)
    if "dome_ms" in K:
        print("dome pass (SkyPassParallel, median of 3): scattered %.3f / overcast %.3f ms at %s -> %.3f ms at 1080p (gate %.1f)%s; capture %.3f / %.3f ms" % (
            K["scattered"]["median"]["dome"], K["overcast"]["median"]["dome"], K["view_size"], K["dome_ms_1080p"] or -1, GATE_MS,
            "" if K.get("pass") else " -> next: " + (K.get("next") or ""), K["scattered"]["median"]["capture_sky"], K["overcast"]["median"]["capture_sky"]), flush=True)


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
