# -*- coding: utf-8 -*-
"""Round 47 (W47, P4 part 1): LEAN check of the hero Box's soft shadow on the ground through the engine's cloud shadow map (Beer
shadow map, BSM) of the FogMS cloud host, in the owner's editor (owner's rule: status / log checks + ONE number; he judges the look
himself; target <= 20-30 min editor time; no screenshot series). FogMS_Weather_Design.md section 6 'W47', criteria:
  1. sun with Cast Cloud Shadows: Box 'Active', no 'not bound' in the status or the log;
  2. host status '[cloud shadow: extent .. km, res .., texel .. m, filter ..]'; a hint without the flag; after
     FogMS.CloudHost.SetupShadows texel <= 10 m and one log line with the previous values;
  3. Lumen Bounce Off + frozen sky (r.SkyLight.RealTimeReflectionCapture 0, round-21 method) + frozen density: the solver field with
     and without the flag within the noise floor (off / on / off, off vs off = floor; FogMS.DumpSpatial of the resident atlas,
     -BindlessAll session);
  4. for the owner's eyes only: one Lit frame pair (flag off / on) from a camera that sees the Box and its ground shadow;
  5. no Box renders through a host (the host temporarily hidden in the editor, the owner's host is never deleted): the plugin's
     cloud-shadow cvars come back (log line); with the host hidden the sky Volumetric Cloud renders again: a foreign cloud gives a
     status warning, not a refusal;
  6. the number: ProfileGPU x3 at the owner camera, 'VolumetricCloudShadow' (CloudShadow + filters), median.

Owner state (map saved 19:49, measure/owner_pre47.json): the Box is on Render Path Cloud Host but its density band dips below the
SkyAtmosphere ground (z 25.7 m), so the host cannot render it (froxel fallback). The checks lift the Box by LIFT_CM (host renders it)
and put it back exactly. The sun's flags / extent / resolution, the Box, the host visibility, the camera, the throttle, the view
mode override and r.SkyLight.RealTimeReflectionCapture are restored from the snapshot; restore prints its diff (must be {}).
Engine cloud / render-target / shadow-map cvars are the plugin's (set at game-setting priority, restored by it): never set here.

Subcommands (python d47_shadow.py <sub>; FOGMS_LOG = the running editor's log, required):
  snapshot [--force]  measure/owner_pre47.json (READ ONLY; written once: the restore target, also after a power cut)
  check               criteria 1, 2, 5 (+ the foreign-cloud warning)
  look                criterion 4: frame pair off / on -> results/diag47/ground_pair.png (+ single frames), no metrics
  cost                criterion 6
  field               criterion 3
  all                 check + look + cost + field, then restore and summary (one command)
  restore             restore the snapshot
  summary             print the verdicts from results/diag47/d47.json
Rules: the map is never saved; no tick callbacks (frames by r.DumpingMovie), no mark_render_state_dirty; throttle off during the run
and restored; ProfileGPU waits while the editor is minimized; a LogPython error flood stops the run; frames go to measure/diag47
(junction to D:/FogMS_ProbeFrames/diag47); results/diag47/d47.json written atomically after every step."""
import os, sys, json, time, re, math, glob, shutil
import numpy as np
import diag34lib as d

HERE = d.HERE
RES = os.path.join(HERE, "results", "diag47")
os.makedirs(RES, exist_ok=True)
OUT = os.path.join(RES, "d47.json")
OWNER = os.path.join(HERE, "measure", "owner_pre47.json")
FRAMES = os.path.join(HERE, "measure", "diag47")          # junction -> D:/FogMS_ProbeFrames/diag47
LOG = os.environ.get("FOGMS_LOG", "")
BOX_LABEL = "FogMS - Live Box"
SS = "D:/PersonalProjects/UE5/MimirHead_portfolio 5.7 5.8 - 3/Saved/Screenshots/WindowsEditor"
SETTLE = 3.0
LIFT_CM = 1000.0          # the owner's Box band dips 4 m below the ground: +10 m puts it inside a host layer
FROZEN_T = 100.0
# Game View (G) of the owner's perspective pane in his Main46 session (read 2026-09-26 20:50, before the round-47 relaunch; a new
# editor starts without it): restored to this at the end.
OWNER_GAME_VIEW_MAIN46 = True
SUN_PROPS = ["cast_cloud_shadows", "cloud_shadow_extent", "cloud_shadow_map_resolution_scale", "cloud_shadow_strength",
             "cloud_shadow_on_surface_strength", "cloud_shadow_on_atmosphere_strength", "cloud_shadow_ray_sample_count_scale"]
BOX_PROPS = ["lumen_bounce", "use_manual_animation_time", "manual_animation_time", "animation_time_offset", "sun_softness"]
SHADOW_CVARS = ["r.VolumetricCloud.ShadowMap.SpatialFiltering", "r.VolumetricCloud.ShadowMap.SnapLength",
                "r.VolumetricCloud.ShadowMap.SnapToPixelGrid"]
ENGINE_CVARS = SHADOW_CVARS + ["r.VolumetricCloud.ShadowMap", "r.VolumetricCloud.ShadowMap.MaxResolution",
                               "r.VolumetricRenderTarget.Mode", "r.VolumetricCloud.SampleMinCount", "r.VolumetricCloud.DistanceToSampleMaxCount",
                               "r.SkyLight.RealTimeReflectionCapture", "r.FogMS.Transport.SolveInterval", "r.FogMS.Transport.DirectSamples"]
PLUGIN_CVARS = ["r.FogMS.CloudHost.ShadowSpatialFiltering", "r.FogMS.CloudHost.ShadowSnapFraction", "r.FogMS.CloudHost.RTMode",
                "r.FogMS.CloudHost.NearDistanceKm"]
_state = {"flood0": None}

FIND = ("import unreal, json\n"
        "_A=unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()\n"
        "A={a.get_actor_label():a for a in _A}\n"
        "box=A[%r]\n"
        "_cm=unreal.load_asset('/MultiLobeSpec/FogMS/M_FogMS_Cloud')\n"
        "def _base(m):\n"
        "    while m is not None and isinstance(m, unreal.MaterialInstance): m=m.get_editor_property('parent')\n"
        "    return m\n"
        "def _cc(a):\n"
        "    return a.get_component_by_class(unreal.VolumetricCloudComponent) if a.get_class().get_name()=='VolumetricCloud' else None\n"
        "clouds=[a for a in _A if _cc(a) is not None]\n"
        "hosts=[a for a in clouds if _cm is not None and _base(_cc(a).get_editor_property('material'))==_cm]\n"
        "others=[a for a in clouds if a not in hosts]\n"
        "host=hosts[0] if hosts else None\n"
        "_suns=[]\n"
        "for a in _A:\n"
        "    if isinstance(a, unreal.DirectionalLight):\n"
        "        c=a.get_component_by_class(unreal.DirectionalLightComponent)\n"
        "        if c and c.get_editor_property('atmosphere_sun_light') and c.get_editor_property('atmosphere_sun_light_index')==0: _suns.append((a,c))\n"
        "sunA,sun=(_suns[0] if _suns else (None,None))\n") % BOX_LABEL


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
    """True when the Unreal Editor main window is minimized (its viewports render nothing). Never restore/focus it from here."""
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
    m = "D47_%s_%d" % (tag, int(time.time() * 1000))
    d.py("import unreal\nunreal.log(%r)" % m)
    return m


def since(mark):
    d.cmd("FLUSHLOG"); time.sleep(0.5)
    text = log_text()
    return text.rsplit(mark, 1)[1] if mark in text else ""


def step(res, key, value):
    res[key] = value
    _atomic_dump(res, OUT)
    print("STEP %-12s %s" % (key, json.dumps(value, default=str)[:900]), flush=True)


def enum_name(value):
    """'<FogMSRenderPath.CLOUD_HOST: 1>' (str of a UE Python enum) -> 'CLOUD_HOST'."""
    m = re.search(r"\.(\w+):", str(value))
    return m.group(1) if m else str(value)


# ------------------------------------------------------------------ state readback (read only)
def state():
    code = FIND + (
        "les=unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem)\n"
        "l,r=les.get_level_viewport_camera_info()\n"
        "lvs=unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)\n"
        "perf=unreal.get_default_object(unreal.load_class(None, '/Script/UnrealEd.EditorPerformanceSettings'))\n"
        "def _v(x):\n"
        "    return str(x) if not isinstance(x, (bool, int, float)) else x\n"
        "loc=box.get_actor_location(); ext=box.get_editor_property('box_component').get_scaled_box_extent()\n"
        "out={'camera':[[l.x,l.y,l.z],[r.pitch,r.yaw,r.roll]], 'throttle': bool(perf.get_editor_property('bThrottleCPUWhenNotForeground')),\n"
        " 'game_view': lvs.editor_get_game_view(),\n"
        " 'box':{'location':[loc.x,loc.y,loc.z],'extent':[ext.x,ext.y,ext.z],'render_path':str(box.get_editor_property('render_path')),\n"
        "  'props':{p:_v(box.get_editor_property(p)) for p in %r},'status':str(box.get_editor_property('spatial_status'))},\n"
        " 'hosts':[{'label':h.get_actor_label(),'hidden':bool(h.is_temporarily_hidden_in_editor())} for h in hosts],\n"
        " 'others':[{'label':h.get_actor_label(),'hidden':bool(h.is_temporarily_hidden_in_editor()),\n"
        "   'material':(lambda m: m.get_name() if m else None)(_cc(h).get_editor_property('material'))} for h in others]}\n"
        "if sun:\n"
        "    f=sunA.get_actor_forward_vector()\n"
        "    out['sun']={'label':sunA.get_actor_label(),'forward':[f.x,f.y,f.z],'props':{p:_v(sun.get_editor_property(p)) for p in %r}}\n"
        "if host:\n"
        "    hc=_cc(host); m=hc.get_editor_property('material')\n"
        "    out['host']={'label':host.get_actor_label(),'layer_km':[hc.get_editor_property('layer_bottom_altitude'),hc.get_editor_property('layer_height')],\n"
        "      'density': m.get_scalar_parameter_value('FogMS_Density') if isinstance(m, unreal.MaterialInstanceDynamic) else None}\n"
        "sl=[a for a in _A if isinstance(a, unreal.SkyLight)]\n"
        "out['skylight_rtc']=[bool(a.get_component_by_class(unreal.SkyLightComponent).get_editor_property('real_time_capture')) for a in sl]\n"
        "out['cvars']={n: unreal.SystemLibrary.get_console_variable_float_value(n) for n in %r}\n"
        "print('D47STATE '+json.dumps(out))") % (BOX_PROPS, SUN_PROPS, ENGINE_CVARS + PLUGIN_CVARS)
    return pyj(code, "D47STATE")


def snapshot(force=False):
    if os.path.isfile(OWNER) and not force:
        return json.load(open(OWNER))
    s = state()
    s["game_view_main46"] = OWNER_GAME_VIEW_MAIN46
    s["note"] = "round 47 snapshot, read before any change in the freshly launched Main47 session (the owner's map as saved 19:49)"
    s["time"] = time.strftime("%Y-%m-%d %H:%M:%S")
    _atomic_dump(s, OWNER)
    return s


# ------------------------------------------------------------------ changes (all undone by restore)
def set_throttle(flag):
    return d.py("import unreal\nperf=unreal.get_default_object(unreal.load_class(None, '/Script/UnrealEd.EditorPerformanceSettings'))\n"
                "perf.set_editor_property('bThrottleCPUWhenNotForeground', %s)\nprint('THROTTLE', perf.get_editor_property('bThrottleCPUWhenNotForeground'))" % bool(flag))


def set_sun(**props):
    code = FIND + "".join("sun.set_editor_property(%r, %r)\n" % (k, v) for k, v in props.items())
    code += "print('SUN', %s)" % ", ".join("sun.get_editor_property(%r)" % k for k in props)
    return d.py(code)


def set_box(**props):
    code = FIND
    for k, v in props.items():
        if k == "render_path":
            code += "box.set_editor_property('render_path', unreal.FogMSRenderPath.%s)\n" % v
        elif k == "lumen_bounce":
            code += "box.set_editor_property('lumen_bounce', unreal.FogMSLumenBounce.%s)\n" % v
        else:
            code += "box.set_editor_property(%r, %r)\n" % (k, v)
    code += "box.update_density()\nprint('SETBOX ok')"
    return d.py(code)


def place_box(loc):
    return d.py(FIND + "box.set_actor_location(unreal.Vector(%r, %r, %r), False, True)\nprint('PLACED')" % tuple(loc))


def set_host_hidden(hidden):
    return d.py(FIND + "[h.set_is_temporarily_hidden_in_editor(%s) for h in hosts]\nprint('HOSTS HIDDEN', [h.is_temporarily_hidden_in_editor() for h in hosts])" % bool(hidden))


def set_game_view(on):
    return d.py("import unreal\nles=unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)\nles.editor_set_game_view(%s)\n"
                "print('GAMEVIEW', les.editor_get_game_view())" % bool(on))


def begin():
    if not LOG or not os.path.isfile(LOG):
        raise SystemExit("Set FOGMS_LOG to the running editor's log file.")
    wait_not_minimized("begin", 300.0)
    flood_check()
    set_throttle(False)
    d.cmd("ShowFlag.OverrideDiffuseAndSpecular 0")        # Lit albedo during the run (the viewport ini keeps VMI_Lit)


def lift(owner, on):
    """The Box up by LIFT_CM (the host renders it) or back to the snapshot location."""
    loc = list(owner["box"]["location"])
    if on:
        loc[2] += LIFT_CM
    place_box(loc)
    time.sleep(SETTLE)


def restore(owner):
    """Everything this script may change back to the snapshot; prints the diff (must be {})."""
    b = owner["box"]
    set_host_hidden(False)
    now = state()
    sp = {k: v for k, v in owner.get("sun", {}).get("props", {}).items() if now.get("sun", {}).get("props", {}).get(k) != v}
    if sp:
        print(set_sun(**sp), flush=True)
    props = {}
    for k in BOX_PROPS:
        want, got = b["props"].get(k), now["box"]["props"].get(k)
        if want is None or want == got:
            continue
        if k == "lumen_bounce":
            props[k] = enum_name(want)
        elif isinstance(want, float) and isinstance(got, float) and abs(want - got) <= 1e-9:
            continue
        else:
            props[k] = want
    if enum_name(now["box"]["render_path"]) != enum_name(b["render_path"]):
        props["render_path"] = enum_name(b["render_path"])
    if props:
        print(set_box(**props), flush=True)
    if max(abs(x - y) for x, y in zip(now["box"]["location"], b["location"])) > 1e-3:
        print(place_box(b["location"]), flush=True)
    rtc = owner["cvars"].get("r.SkyLight.RealTimeReflectionCapture")
    if rtc is not None and abs(now["cvars"]["r.SkyLight.RealTimeReflectionCapture"] - rtc) > 1e-6:
        d.cmd("r.SkyLight.RealTimeReflectionCapture %d" % int(rtc))
    d.set_cam(*owner["camera"])
    set_game_view(owner.get("game_view_main46", owner.get("game_view", False)))
    set_throttle(owner["throttle"])
    d.cmd("ShowFlag.OverrideDiffuseAndSpecular 2")        # the viewport's own mode (VMI_Lit in the ini; never Detail Lighting)
    time.sleep(SETTLE + 2.0)
    after = state()
    diff = {}
    for k, v in owner.get("sun", {}).get("props", {}).items():
        if after.get("sun", {}).get("props", {}).get(k) != v:
            diff["sun." + k] = (v, after["sun"]["props"].get(k))
    for k in BOX_PROPS:
        v, g = b["props"].get(k), after["box"]["props"].get(k)
        if isinstance(v, float) and isinstance(g, float):
            if abs(v - g) > 1e-6:
                diff["box." + k] = (v, g)
        elif v != g:
            diff["box." + k] = (v, g)
    if after["box"]["render_path"] != b["render_path"]:
        diff["box.render_path"] = (b["render_path"], after["box"]["render_path"])
    if max(abs(x - y) for x, y in zip(after["box"]["location"], b["location"])) > 1e-3:
        diff["box.location"] = (b["location"], after["box"]["location"])
    if [h["hidden"] for h in after["hosts"]] != [h["hidden"] for h in owner["hosts"]]:
        diff["hosts"] = (owner["hosts"], after["hosts"])
    if [h["hidden"] for h in after["others"]] != [h["hidden"] for h in owner["others"]]:
        diff["others"] = (owner["others"], after["others"])
    cam = max(abs(x - y) for x, y in zip(after["camera"][0] + after["camera"][1], owner["camera"][0] + owner["camera"][1]))
    if cam > 1e-2:
        diff["camera"] = cam
    if after.get("throttle") != owner.get("throttle"):
        diff["throttle"] = (owner.get("throttle"), after.get("throttle"))
    want_gv = owner.get("game_view_main46", owner.get("game_view"))
    if after.get("game_view") != want_gv:
        diff["game_view"] = (want_gv, after.get("game_view"))
    for k in ["r.SkyLight.RealTimeReflectionCapture"] + PLUGIN_CVARS:
        if abs(owner["cvars"].get(k, 0) - after["cvars"].get(k, 0)) > 1e-6:
            diff[k] = (owner["cvars"].get(k), after["cvars"].get(k))
    # Engine cloud / shadow-map cvars are the plugin's (restored by it once no Box renders through a host or the sun stops casting
    # cloud shadows): reported, never set here.
    eng = {k: (v, after["cvars"].get(k)) for k, v in owner["cvars"].items()
           if k in ENGINE_CVARS and k != "r.SkyLight.RealTimeReflectionCapture" and abs(v - after["cvars"].get(k, 1e30)) > 1e-6}
    print("RESTORE diff", diff, flush=True)
    print("RESTORE engine cvars differing from the snapshot (plugin-managed, informational):", eng, flush=True)
    return {"diff": diff, "engine_cvars": eng, "status": after["box"]["status"][-500:]}


# ------------------------------------------------------------------ status parsing
SHADOW_RE = re.compile(r"\[cloud shadow: extent ([\d.]+) km, res (\d+), texel ([\d.]+) m, filter (-?\d+)([^\]]*)\]")


def shadow_part(status):
    m = SHADOW_RE.search(status)
    if m:
        return {"extent_km": float(m.group(1)), "res": int(m.group(2)), "texel_m": float(m.group(3)), "filter": int(m.group(4)),
                "rest": m.group(5), "warning": "WARNING texel" in m.group(5)}
    i = status.find("[cloud shadow:")
    return {"text": status[i:i + 300] if i >= 0 else None}


def rendered(st):
    return "[render: cloud host]" in st["box"]["status"] and ((st.get("host") or {}).get("density") or 0.0) > 0.0


def active(st):
    return st["box"]["status"].startswith("Active")


def box_min_side_m(st):
    e = st["box"]["extent"]
    return 2.0 * min(e[0], e[1]) / 100.0


# ------------------------------------------------------------------ criteria 1, 2, 5
def check(owner):
    res = _load(OUT)
    C = {"started": time.strftime("%Y-%m-%d %H:%M:%S")}
    res["check"] = C
    begin()
    mark = marker("CHECK")
    try:
        lift(owner, True)
        st = state()
        if enum_name(st["box"]["render_path"]) != "CLOUD_HOST":
            print(set_box(render_path="CLOUD_HOST"), flush=True); time.sleep(SETTLE); st = state()
        C["precondition"] = {"rendered": rendered(st), "status": st["box"]["status"][-600:], "host": st.get("host")}
        step(res, "check", C)
        if not rendered(st):
            raise RuntimeError("the lifted Box does not render through the host: %s" % st["box"]["status"][-400:])
        # 2a. hint without the flag (the owner's saved sun has it off; else switch it off for this read)
        if st["sun"]["props"]["cast_cloud_shadows"]:
            set_sun(cast_cloud_shadows=False); time.sleep(SETTLE); st = state()
        hint = "[cloud shadow: off: the sun" in st["box"]["status"]
        C["A_hint_flag_off"] = {"ok": hint, "cloud_shadow": shadow_part(st["box"]["status"]),
                                "shadow_cvars": {k: st["cvars"][k] for k in SHADOW_CVARS}}
        step(res, "check", C)
        # 1 + 2b. flag on with the owner's extent / resolution (engine defaults 150 km / x1)
        m1 = marker("FLAG_ON")
        set_sun(cast_cloud_shadows=True); time.sleep(SETTLE * 2); st = state()
        sp = shadow_part(st["box"]["status"])
        ext = float(st["sun"]["props"]["cloud_shadow_extent"])
        texel_expect = 2.0 * ext * 1000.0 / max(sp.get("res") or 1, 1)
        want_snap = min(20.0, round(0.25 * ext * 1000.0) / 1000.0)
        cv = {k: st["cvars"][k] for k in SHADOW_CVARS}
        lines = [l[:400] for l in since(m1).splitlines() if "FogMS cloud host" in l]
        C["B_flag_on"] = {"active": active(st), "not_bound_in_status": "not bound" in st["box"]["status"], "cloud_shadow": sp,
                          "texel_expected_m": round(texel_expect, 2), "box_min_side_m": round(box_min_side_m(st), 1),
                          "shadow_cvars": cv, "want": {"filter": 2, "snap_km": want_snap, "snap_to_pixel": 1}, "log": lines[-6:],
                          "status": st["box"]["status"][-700:]}
        C["B_flag_on"]["ok"] = (active(st) and "not bound" not in st["box"]["status"] and sp.get("texel_m") is not None
                                and abs(sp["texel_m"] - texel_expect) < 0.11 and sp["warning"] == (texel_expect > box_min_side_m(st))
                                and abs(cv["r.VolumetricCloud.ShadowMap.SpatialFiltering"] - 2) < 1e-6
                                and abs(cv["r.VolumetricCloud.ShadowMap.SnapLength"] - want_snap) < 1e-4
                                and abs(cv["r.VolumetricCloud.ShadowMap.SnapToPixelGrid"] - 1) < 1e-6)
        step(res, "check", C)
        # 2c. FogMS.CloudHost.SetupShadows: one log line with the previous values, texel <= 10 m, snap 1.25 km
        m2 = marker("SETUP")
        d.cmd("FogMS.CloudHost.SetupShadows"); time.sleep(SETTLE * 2); st = state()
        text = since(m2)
        setup = [l[:600] for l in text.splitlines() if "FogMS.CloudHost.SetupShadows:" in l]
        sp = shadow_part(st["box"]["status"])
        cv = {k: st["cvars"][k] for k in SHADOW_CVARS}
        C["C_setup"] = {"log": setup, "sun": st["sun"]["props"], "cloud_shadow": sp, "shadow_cvars": cv,
                        "settings_lines": [l[:400] for l in text.splitlines() if "FogMS cloud host settings" in l][-3:]}
        C["C_setup"]["ok"] = (len(setup) == 1 and "Cloud Shadow Extent %g -> 5 km" % ext in setup[0]
                              and abs(float(st["sun"]["props"]["cloud_shadow_extent"]) - 5.0) < 1e-6
                              and abs(float(st["sun"]["props"]["cloud_shadow_map_resolution_scale"]) - 2.0) < 1e-6
                              and sp.get("texel_m") is not None and sp["texel_m"] <= 10.0 and not sp["warning"]
                              and abs(cv["r.VolumetricCloud.ShadowMap.SnapLength"] - 1.25) < 1e-4)
        step(res, "check", C)
        # 5 + foreign. No Box renders through a host (host temporarily hidden): restore line with the shadow cvars; the sky cloud
        # renders again (foreign): warning in the Box status, Box still Active.
        m3 = marker("HOST_HIDDEN")
        set_host_hidden(True); time.sleep(SETTLE * 2); st = state()
        text = since(m3)
        restored = [l[:700] for l in text.splitlines() if "FogMS cloud host settings restored" in l]
        cv = {k: st["cvars"][k] for k in SHADOW_CVARS}
        foreign = "[sun cloud shadows: Volumetric Cloud '" in st["box"]["status"]
        C["D_no_host"] = {"restored_line": restored, "shadow_cvars": cv, "active": active(st), "foreign_warning": foreign,
                          "not_bound": "not bound" in st["box"]["status"], "others_visible": [o["label"] for o in st["others"] if not o["hidden"]],
                          "status": st["box"]["status"][-700:],
                          "worldsources_log": [l[:400] for l in text.splitlines() if "not a FogMS cloud host" in l][-2:]}
        pre = C["A_hint_flag_off"]["shadow_cvars"]
        C["D_no_host"]["ok"] = (bool(restored) and all(k in restored[-1] for k in SHADOW_CVARS)
                                and all(abs(cv[k] - pre[k]) < 1e-6 for k in SHADOW_CVARS) and active(st) and not C["D_no_host"]["not_bound"])
        C["D_no_host"]["foreign_ok"] = foreign or not C["D_no_host"]["others_visible"]
        step(res, "check", C)
        m4 = marker("HOST_SHOWN")
        set_host_hidden(False); time.sleep(SETTLE * 2); st = state()
        text = since(m4)
        C["E_host_back"] = {"rendered": rendered(st), "shadow_cvars": {k: st["cvars"][k] for k in SHADOW_CVARS},
                            "settings_lines": [l[:500] for l in text.splitlines() if "FogMS cloud host settings" in l][-2:]}
        C["E_host_back"]["ok"] = rendered(st) and abs(C["E_host_back"]["shadow_cvars"]["r.VolumetricCloud.ShadowMap.SnapLength"] - 1.25) < 1e-4
        step(res, "check", C)
    finally:
        text = since(mark)
        lines = text.splitlines()
        C["F_log"] = {"not_bound": [l[:300] for l in lines if "not bound" in l][:5],
                      "python_errors": text.count("LogPython: Error"),
                      "ensures": [l[:300] for l in lines if "Ensure condition failed" in l][:5],
                      "multilobespec_errors": [l[:300] for l in lines if "LogMultiLobeSpec: Error" in l][:10],
                      "shadow_note_lines": [l[:400] for l in lines if "[cloud shadow:" in l and "FogMS cloud host" in l][-8:]}
        C["F_log"]["ok"] = not (C["F_log"]["not_bound"] or C["F_log"]["python_errors"] or C["F_log"]["ensures"] or C["F_log"]["multilobespec_errors"])
        step(res, "check", C)
    return res


# ------------------------------------------------------------------ criterion 4: frame pair for the owner (no metrics)
def _movie_frames():
    return set(glob.glob(os.path.join(SS, "MovieFrame[0-9][0-9][0-9][0-9][0-9].png")))


def grab(name, n=4, timeout=120.0):
    """n real viewport frames (r.DumpingMovie; only the realtime perspective pane dumps) into measure/diag47/<name>; no callback."""
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


GROUND_CAM = FIND + (
    "w=unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()\n"
    "c=box.get_actor_location(); f=sunA.get_actor_forward_vector()\n"
    "ign=[box]+hosts\n"
    "def _hit(a, b):\n"
    "    h=unreal.SystemLibrary.line_trace_single(w, a, b, unreal.TraceTypeQuery.TRACE_TYPE_QUERY1, True, ign, unreal.DrawDebugTrace.NONE, True)\n"
    "    if h is None: return None\n"
    "    t=h.to_tuple()\n"
    "    if not t[0]: return None\n"
    "    v=[x for x in t if isinstance(x, unreal.Vector)]\n"
    "    return v[0] if v else None\n"
    "g=_hit(c, c+f*2000000.0)\n"
    "out={'box':[c.x,c.y,c.z],'fwd':[f.x,f.y,f.z],'ground_hit': g is not None}\n"
    "if g is None:\n"
    "    t=c.z/max(-f.z,1e-3); g=c+f*t\n"
    "out['g']=[g.x,g.y,g.z]\n"
    "print('D47CAM '+json.dumps(out))")


def ground_camera():
    """A side view of the Box and its ground shadow: the shadow centre G = where the ray from the Box centre along the sun's travel
    direction hits geometry; the camera looks at the midpoint of Box and G from the side (perpendicular to the sun's azimuth), far
    enough to hold both, and higher than the pair; sun from the side."""
    g = pyj(GROUND_CAM, "D47CAM")
    C, G, f = np.array(g["box"]), np.array(g["g"]), np.array(g["fwd"])
    M = 0.5 * (C + G)
    R = float(np.linalg.norm(G - C))
    fh = np.array([f[0], f[1], 0.0]); fh /= max(np.linalg.norm(fh), 1e-6)
    p = np.array([-fh[1], fh[0], 0.0])
    S = max(1.1 * R, 40000.0)
    cam = M + p * S + np.array([0.0, 0.0, 0.35 * S])
    dv = M - cam
    yaw = math.degrees(math.atan2(dv[1], dv[0]))
    pitch = math.degrees(math.atan2(dv[2], math.hypot(dv[0], dv[1])))
    return {"trace": g, "shadow_len_m": round(R / 100.0, 1), "cam": [cam.tolist(), [pitch, yaw, 0.0]]}


def look(owner):
    res = _load(OUT)
    L = {"started": time.strftime("%Y-%m-%d %H:%M:%S")}
    res["look"] = L
    begin()
    try:
        wait_not_minimized("look")
        lift(owner, True)
        st = state()
        if not st["sun"]["props"]["cast_cloud_shadows"] or abs(float(st["sun"]["props"]["cloud_shadow_extent"]) - 5.0) > 1e-6:
            d.cmd("FogMS.CloudHost.SetupShadows"); time.sleep(SETTLE)
        gv = set_game_view(True)                          # no editor primitives (Box bounds) in the owner's frames
        cam = ground_camera()
        L["camera"] = cam
        d.set_cam(*cam["cam"]); time.sleep(SETTLE + 2.0)
        imgs = {}
        from PIL import Image
        for tag, flag in (("on", True), ("off", False)):
            set_sun(cast_cloud_shadows=flag); time.sleep(SETTLE + 2.0)
            out, got = grab("ground_%s" % tag)
            fs = sorted(glob.glob(os.path.join(out, "f*.png")))
            if not fs:
                raise RuntimeError("no frames for %s (editor minimized?)" % tag)
            shutil.copy(fs[-1], os.path.join(RES, "ground_%s.png" % tag))
            imgs[tag] = Image.open(fs[-1]).convert("RGB")
            L[tag] = {"frames": got, "status": state()["box"]["status"][-300:]}
            step(res, "look", L)
        set_sun(cast_cloud_shadows=True)
        w, h = imgs["off"].size
        sc = min(1.0, 900.0 / w)
        a, b = (im.resize((int(w * sc), int(h * sc))) for im in (imgs["off"], imgs["on"]))
        from PIL import ImageDraw
        sheet = Image.new("RGB", (a.width * 2 + 8, a.height + 24), (18, 18, 18))
        sheet.paste(a, (0, 24)); sheet.paste(b, (a.width + 8, 24))
        dr = ImageDraw.Draw(sheet)
        dr.text((6, 5), "Cast Cloud Shadows OFF", fill=(230, 230, 230))
        dr.text((a.width + 14, 5), "Cast Cloud Shadows ON (FogMS.CloudHost.SetupShadows: 5 km, x2 = 9.8 m texel)", fill=(230, 230, 230))
        sheet.save(os.path.join(RES, "ground_pair.png"))
        L["sheet"] = "results/diag47/ground_pair.png"
        L["game_view_set"] = gv
        step(res, "look", L)
    finally:
        d.set_cam(*owner["camera"])
    return res


# ------------------------------------------------------------------ criterion 6: GPU cost of the cloud shadow map
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
    return {"frame_ms": frame, "shadow_scope": tot({"VolumetricCloudShadow"}), "cloud_shadow": tot({"CloudShadow"}),
            "spatial_filter": tot({"CloudDataSpatialFilter"}), "temporal_filter": tot({"CloudDataTemporalFilter"}),
            "sky_ao": tot({"CloudSkyAO"}), "cloud_trace": tot({"VolumetricCloud"}), "events": len(rows),
            "shadow_rows": [[ms, n] for ms, n in rows if "Shadow" in n and "Cloud" in n or n.startswith("CloudData") or n == "CloudSkyAO"][:12]}


def cost(owner):
    res = _load(OUT)
    begin()
    K = {"started": time.strftime("%Y-%m-%d %H:%M:%S")}
    try:
        lift(owner, True)
        st = state()
        if not st["sun"]["props"]["cast_cloud_shadows"] or abs(float(st["sun"]["props"]["cloud_shadow_extent"]) - 5.0) > 1e-6:
            d.cmd("FogMS.CloudHost.SetupShadows"); time.sleep(SETTLE)
        d.set_cam(*owner["camera"]); time.sleep(SETTLE + 2.0)
        st = state()
        runs = []
        for i in range(3):
            flood_check()
            wait_not_minimized("cost")
            p = profile("cost_%d" % i)
            if "error" in p:
                p = profile("cost_%d_retry" % i)
            if "error" in p:
                raise RuntimeError(p["error"])
            runs.append(p)
            print("COST %d shadow scope %.3f (CloudShadow %.3f, spatial %.3f, sky AO %.3f) cloud trace %.3f frame %s" % (
                i, p["shadow_scope"], p["cloud_shadow"], p["spatial_filter"], p["sky_ao"], p["cloud_trace"], p["frame_ms"]), flush=True)
        med = {k: round(sorted(r[k] for r in runs)[1], 3) for k in ("shadow_scope", "cloud_shadow", "spatial_filter", "temporal_filter", "sky_ao", "cloud_trace")}
        frames = sorted(r["frame_ms"] for r in runs if r["frame_ms"] is not None)
        med["frame_ms"] = frames[len(frames) // 2] if frames else None
        K.update({"median": med, "runs": runs, "cloud_shadow_status": shadow_part(st["box"]["status"]),
                  "sun": st["sun"]["props"], "shadow_cvars": {k: st["cvars"][k] for k in SHADOW_CVARS}, "camera": "owner",
                  "ok": med["shadow_scope"] <= 0.3})
        step(res, "cost", K)
    finally:
        pass
    return res


# ------------------------------------------------------------------ criterion 3: the solver field with / without the flag
def dump_field(name, tries=10):
    """FogMS.DumpSpatial of the packet Box's resident atlas (-BindlessAll). In FourPanes an ortho pane redraw can clear the 'last
    build valid' flag for a frame: retried."""
    prefix = os.path.join(FRAMES, "field", name).replace("\\", "/")
    os.makedirs(os.path.dirname(prefix), exist_ok=True)
    for t in range(tries):
        for ext in (".json", ".rgba32f"):
            if os.path.isfile(prefix + ext):
                os.remove(prefix + ext)
        d.cmd("FogMS.DumpSpatial %s" % prefix)
        t0 = time.time()
        while time.time() - t0 < 5.0 and not os.path.isfile(prefix + ".json"):
            time.sleep(0.25)
        time.sleep(0.5)
        try:
            meta = json.load(open(prefix + ".json", encoding="utf-8"))
        except Exception:
            meta = {"success": False, "reason": "no json"}
        if meta.get("success") and os.path.isfile(prefix + ".rgba32f") and meta.get("domain") == "transport":
            meta["try"] = t
            return prefix + ".rgba32f", meta
        time.sleep(0.7)
    return None, meta


def field_J(path):
    N = 32
    return np.fromfile(path, dtype="<f4").reshape(-1, 4)[:N ** 3, :3].astype(np.float64)


def field_diff(pa, pb):
    """As fielddiff.py: rel L2 of J (slab 0 rgb), luminance ratio, bright-half mean |dJ|/J."""
    a, b = field_J(pa), field_J(pb)
    la = 0.2126 * a[:, 0] + 0.7152 * a[:, 1] + 0.0722 * a[:, 2]
    lb = 0.2126 * b[:, 0] + 0.7152 * b[:, 1] + 0.0722 * b[:, 2]
    ok = np.isfinite(la) & np.isfinite(lb)
    rel = float(np.sqrt(((a - b)[ok] ** 2).sum() / max((b[ok] ** 2).sum(), 1e-30)))
    w = np.maximum(lb[ok], 0)
    bright = w > np.percentile(w, 50)
    return {"rel_l2": rel, "lum_ratio": float(la[ok].sum() / max(lb[ok].sum(), 1e-30)),
            "bright_rel": float((np.abs(la - lb)[ok][bright] / np.maximum(lb[ok][bright], 1e-9)).mean())}


def field(owner):
    res = _load(OUT)
    begin()
    Fd = {"started": time.strftime("%Y-%m-%d %H:%M:%S")}
    try:
        lift(owner, True)
        st = state()
        if abs(float(st["sun"]["props"]["cloud_shadow_extent"]) - 5.0) > 1e-6:
            d.cmd("FogMS.CloudHost.SetupShadows"); time.sleep(SETTLE)
        # Lumen Bounce Off, density frozen at t = 100 s, sky frozen (static capture instead of Real Time Capture)
        set_box(lumen_bounce="OFF", manual_animation_time=FROZEN_T, use_manual_animation_time=True)
        d.cmd("r.SkyLight.RealTimeReflectionCapture 0")
        time.sleep(12.0)
        dumps = {}
        for tag, flag in (("off_a", False), ("on", True), ("off_b", False)):
            flood_check()
            set_sun(cast_cloud_shadows=flag); time.sleep(8.0)
            s = state()
            path, meta = dump_field(tag)
            dumps[tag] = path
            Fd[tag] = {"flag": flag, "active": active(s), "not_bound": "not bound" in s["box"]["status"], "rendered_by_host": rendered(s),
                       "status": s["box"]["status"][:260], "sky": (re.search(r"\[sky: ([^\]]*\]?)", s["box"]["status"]) or [None, None])[1],
                       "dump": {k: meta.get(k) for k in ("success", "reason", "iterations", "maxRelativeCellResidual", "revision",
                                                         "renderFrame", "try")}}
            step(res, "field", Fd)
            if not path:
                raise RuntimeError("field dump %s failed: %s" % (tag, meta.get("reason")))
        floor = field_diff(dumps["off_b"], dumps["off_a"])
        e1, e2 = field_diff(dumps["on"], dumps["off_a"]), field_diff(dumps["on"], dumps["off_b"])
        Fd["floor_off_b_vs_off_a"] = floor
        Fd["on_vs_off_a"] = e1
        Fd["on_vs_off_b"] = e2
        # Within the floor: the flag's difference is no larger than the repeat's (2x the floor, at least 1e-4 absolute slack).
        lim = max(2.0 * floor["rel_l2"], floor["rel_l2"] + 1e-4)
        Fd["limit_rel_l2"] = lim
        Fd["ok"] = all(x["active"] and not x["not_bound"] for x in (Fd["off_a"], Fd["on"], Fd["off_b"])) and max(e1["rel_l2"], e2["rel_l2"]) <= lim
        step(res, "field", Fd)
    finally:
        b = owner["box"]["props"]
        set_box(lumen_bounce=enum_name(b["lumen_bounce"]), use_manual_animation_time=b["use_manual_animation_time"],
                manual_animation_time=b["manual_animation_time"])
        rtc = owner["cvars"].get("r.SkyLight.RealTimeReflectionCapture", 1.0)
        d.cmd("r.SkyLight.RealTimeReflectionCapture %d" % int(rtc))
        time.sleep(SETTLE)
    return res


# ------------------------------------------------------------------ summary
def summary():
    res = _load(OUT)
    C = res.get("check", {})
    Fd = res.get("field", {})
    K = res.get("cost", {})
    rows = [("1  flag on: Box Active, no 'not bound' (status + log)", (C.get("B_flag_on") or {}).get("active") and not (C.get("B_flag_on") or {}).get("not_bound_in_status") and not (C.get("F_log") or {}).get("not_bound")),
            ("2a hint without the flag", (C.get("A_hint_flag_off") or {}).get("ok")),
            ("2b status line + shadow cvars (filter 2, snap, pixel grid)", (C.get("B_flag_on") or {}).get("ok")),
            ("2c SetupShadows: log line, texel <= 10 m", (C.get("C_setup") or {}).get("ok")),
            ("3  field with / without the flag within the floor", Fd.get("ok")),
            ("5  no host: shadow cvars restored (log line)", (C.get("D_no_host") or {}).get("ok")),
            ("5b foreign cloud: warning, Box Active", (C.get("D_no_host") or {}).get("foreign_ok")),
            ("   host back: re-applied", (C.get("E_host_back") or {}).get("ok")),
            ("   log clean (no errors / ensures)", (C.get("F_log") or {}).get("ok")),
            ("6  CloudShadow cost <= 0.3 ms", K.get("ok")),
            ("   restore diff empty", not ((res.get("restore") or {}).get("diff")))]
    for name, ok in rows:
        print("%-62s %s" % (name, ok), flush=True)
    if Fd.get("floor_off_b_vs_off_a"):
        print("field: floor %s | on vs off_a %s | on vs off_b %s" % (json.dumps(Fd["floor_off_b_vs_off_a"]), json.dumps(Fd["on_vs_off_a"]),
                                                                      json.dumps(Fd["on_vs_off_b"])), flush=True)
    if K.get("median"):
        print("cost (owner camera, median of 3): %s" % json.dumps(K["median"]), flush=True)


def main(cmd):
    if cmd == "summary":
        summary(); return
    owner = snapshot(force="--force" in sys.argv and cmd == "snapshot")
    if cmd == "snapshot":
        print(json.dumps(owner, indent=1)[:4000]); return
    if cmd == "restore":
        r = restore(owner); res = _load(OUT); res["restore"] = r; _atomic_dump(res, OUT); return
    runs = {"check": [check], "look": [look], "cost": [cost], "field": [field], "all": [check, look, cost, field]}.get(cmd)
    if runs is None:
        raise SystemExit("unknown subcommand " + cmd)
    try:
        for fn in runs:
            try:
                fn(owner)
            except Exception as e:           # record, continue with the next criterion, restore at the end
                res = _load(OUT); res.setdefault("errors", []).append("%s: %r" % (fn.__name__, e)); _atomic_dump(res, OUT)
                print("ERROR in %s: %r" % (fn.__name__, e), flush=True)
    finally:
        r = restore(owner)
        res = _load(OUT); res["restore"] = r; _atomic_dump(res, OUT)
        summary()


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "snapshot")
