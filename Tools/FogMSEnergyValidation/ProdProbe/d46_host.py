# -*- coding: utf-8 -*-
"""Round 46 (W46) phase 2: LEAN functional check of the cloud-host slice in the owner's editor (target <= 20 min editor time,
owner's rule 2026-09-26: no screenshot series; he judges the look by eye). Checks by status strings, engine/host readback and the
editor log, plus ONE GPU cost number of the host (defaults: render-target mode 3 near the camera, Host Prefilter 1).

What W46 changed (FogMS_UserGuide.md section 4 'Render Path', docs/history/FogMS_Prod_Report.md):
  1. solver: Sun Softness > 0 -> the sun at all 8 subcell points every solve (ProfileGPU pass name '... [sun 8 points]');
  2. Render Path class default Cloud Host; no host -> froxel fallback + status 'click Create Cloud Host';
  3. the host follows the Box: host MID every update, layer refitted to the density band +-10 m with 5 m hysteresis;
  4. host settings applied while a Box uses a host (game-setting priority, restored after): r.VolumetricRenderTarget.Mode 3 near /
     1 far (r.FogMS.CloudHost.NearDistanceKm 1), SampleMinCount 32 / 8, UpsamplingMode 2, ReprojectionBoxConstraint 1,
     MinimumDistanceKmToEnableReprojection 4, host View Sample Count Scale 8 (plugin cvars r.FogMS.CloudHost.*);
  5. M_FogMS_Cloud v2 (matedit_cloud.py): Nubis-style prefilter, Box property Host Prefilter (default 1).

Subcommands (python d46_host.py <sub>; FOGMS_LOG = the running editor's log, required):
  snapshot [--force]  measure/owner_pre46.json: camera, throttle, Box transform / Render Path / Sun Softness / Host Prefilter,
                      hosts, plugin + managed engine cvars (READ ONLY). Written once; the restore target (also after a power cut).
                      Run it in the round-46 session (Host Prefilter does not exist before the install), before any change.
  matedit             matedit_cloud.py in the editor (v1 -> v2 upgrade in place, rollback on error; ALREADY_PATCHED on repeat)
  copyback            copy the saved M_FogMS_Cloud / MI_FogMS_Cloud .uasset from the project plugin into this worktree (files only)
  check               A host bound (creates one with the Box button if the level has none; destroyed by restore)
                      B defaults applied: engine cvars vs r.FogMS.CloudHost.* (near/far from the status), host view scale,
                        host MID FogMS_CloudStep / FogMS_CloudPrefilter, log line 'FogMS cloud host settings'
                      C Sun Softness: ProfileGPU pass-2 name '[sun 8 points]' at the owner's Sun Softness (5 if it is 0),
                        '[sun 4 points, alternating]' at 0 (only with r.FogMS.Transport.DirectSamples 4)
                      D host follows the Box: +30 m up, +2 m (hysteresis: no refit), Z scale x1.5, back: status '[render: cloud
                        host]', host MID density > 0, MID box centre / extent follow, layer refits, log 'fitted to Box'
                      E near/far: camera 1.5 x NearDistanceKm away -> 'far settings' + FarRTMode; back -> near
                      F errors in the log since the start marker (LogPython errors, Ensure, 'Failed to compile Material',
                        LogMultiLobeSpec errors)
  cost                ProfileGPU x3 at the owner camera (host bound, defaults): VolumetricCloud trace + compose (+ reconstruct),
                      cloud shadow, FogMS, frame -> median
  all                 matedit + check + cost + restore (one command; ~10 min)
  restore             restore the snapshot (Box transform / Render Path / Sun Softness / Host Prefilter, a host this script
                      created is destroyed, camera, throttle, ShowFlag.OverrideDiffuseAndSpecular 2)

Project rules followed: the map is never saved; snapshot before any change, restore after, the restore prints its diff; the
editor stays LIT during the run (ShowFlag.OverrideDiffuseAndSpecular 0, then 2 = the viewport's own mode, which the ini keeps at
VMI_Lit; never Detail Lighting); background throttle off during the run; NO engine cvar is set by console (the plugin sets the
cloud / render-target cvars at game-setting priority; a console value would lock them for the session, the round-45 lesson);
no frame dumps (so no measure/diag46 junction and no world time dilation are needed); no tick callbacks and no
mark_render_state_dirty (every change is a property set; the Box's own tick does the rest); a LogPython error flood stops the run;
results/diag46/d46.json written atomically after every step (keys present are kept; 'check' and 'cost' re-run as a whole)."""
import os, sys, json, time, re, math, shutil, hashlib
import diag34lib as d
import cloudproto_session as S

HERE = d.HERE
RES = os.path.join(HERE, "results", "diag46")
os.makedirs(RES, exist_ok=True)
OUT = os.path.join(RES, "d46.json")
OWNER = os.path.join(HERE, "measure", "owner_pre46.json")
LOG = os.environ.get("FOGMS_LOG", "")
S.LOG = LOG
BOX_LABEL = "FogMS - Live Box"
HOST_LABEL = "FogMS Cloud Host"
PROJECT = "D:/PersonalProjects/UE5/MimirHead_portfolio 5.7 5.8 - 3"
WORKTREE = os.path.normpath(os.path.join(HERE, "..", "..", ".."))
ENGINE_CVARS = ["r.VolumetricRenderTarget.Mode", "r.VolumetricRenderTarget.UpsamplingMode",
                "r.VolumetricRenderTarget.ReprojectionBoxConstraint", "r.VolumetricRenderTarget.MinimumDistanceKmToEnableReprojection",
                "r.VolumetricCloud.SampleMinCount", "r.VolumetricCloud.DistanceToSampleMaxCount", "r.VolumetricCloud.ViewRaySampleMaxCount",
                "r.FogMS.Transport.DirectSamples", "r.FogMS.Transport.SolveInterval"]
PLUGIN_CVARS = ["r.FogMS.CloudHost.StepSettings", "r.FogMS.CloudHost.FitLayer", "r.FogMS.CloudHost.ViewSampleScale",
                "r.FogMS.CloudHost.RTMode", "r.FogMS.CloudHost.FarRTMode", "r.FogMS.CloudHost.SampleMinCount",
                "r.FogMS.CloudHost.FarSampleMinCount", "r.FogMS.CloudHost.NearDistanceKm", "r.FogMS.CloudHost.UpsamplingMode",
                "r.FogMS.CloudHost.ReprojectionBoxConstraint", "r.FogMS.CloudHost.ReprojectionMinKm"]
SETTLE = 2.5          # seconds for the Box tick / subsystem tick / render to pick a change up
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
        "hosts=[a for a in _A if _cc(a) is not None and _cm is not None and _base(_cc(a).get_editor_property('material'))==_cm]\n"
        "host=hosts[0] if hosts else None\n"
        "hc=_cc(host) if host else None\n") % BOX_LABEL


def _atomic_dump(obj, path):
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        json.dump(obj, f, indent=1); f.flush(); os.fsync(f.fileno())
    os.replace(tmp, path)


def _load(path):
    return json.load(open(path)) if os.path.isfile(path) else {}


def pyj(code, tag):
    return S.last_json(d.py(code), tag)


def rp_name(value):
    """'<FogMSRenderPath.CLOUD_HOST: 1>' (str of the UE Python enum) -> 'CLOUD_HOST'."""
    m = re.search(r"FogMSRenderPath\.(\w+)", str(value))
    return m.group(1) if m else str(value)


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


def marker(tag):
    m = "D46_%s_%d" % (tag, int(time.time() * 1000))
    d.py("import unreal\nunreal.log(%r)" % m)
    return m


def since(mark):
    text = log_text()
    return text.rsplit(mark, 1)[1] if mark in text else ""


# ------------------------------------------------------------------ state readback (read only)
def state():
    code = FIND + (
        "les=unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem)\n"
        "l,r=les.get_level_viewport_camera_info()\n"
        "perf=unreal.get_default_object(unreal.load_class(None, '/Script/UnrealEd.EditorPerformanceSettings'))\n"
        "loc=box.get_actor_location(); rot=box.get_actor_rotation(); sc=box.get_actor_scale3d()\n"
        "ext=box.get_editor_property('box_component').get_scaled_box_extent()\n"
        "out={'camera':[[l.x,l.y,l.z],[r.pitch,r.yaw,r.roll]], 'throttle': bool(perf.get_editor_property('bThrottleCPUWhenNotForeground')),\n"
        " 'box':{'location':[loc.x,loc.y,loc.z],'rotation':[rot.pitch,rot.yaw,rot.roll],'scale':[sc.x,sc.y,sc.z],'extent':[ext.x,ext.y,ext.z],\n"
        "  'render_path':str(box.get_editor_property('render_path')),'sun_softness':box.get_editor_property('sun_softness'),\n"
        "  'host_prefilter':box.get_editor_property('host_prefilter'),'status':str(box.get_editor_property('spatial_status'))},\n"
        " 'hosts':[h.get_actor_label() for h in hosts], 'host_material_found': _cm is not None}\n"
        "if hc:\n"
        "    m=hc.get_editor_property('material')\n"
        "    out['host']={'label':host.get_actor_label(),'hidden':bool(host.is_temporarily_hidden_in_editor()),\n"
        "      'layer_km':[hc.get_editor_property('layer_bottom_altitude'),hc.get_editor_property('layer_height')],\n"
        "      'trace_km':hc.get_editor_property('tracing_max_distance'),'view_scale':hc.get_editor_property('view_sample_count_scale'),\n"
        "      'material':m.get_name() if m else None,'mid':None}\n"
        "    if isinstance(m, unreal.MaterialInstanceDynamic):\n"
        "        _v=lambda p: (lambda c: [c.r,c.g,c.b,c.a])(m.get_vector_parameter_value(p))\n"
        "        out['host']['mid']={'scalars':{p: m.get_scalar_parameter_value(p) for p in ('FogMS_Density','FogMS_CloudStep','FogMS_CloudPrefilter','FogMS_InjectionMode')},\n"
        "          'vectors':{p: _v(p) for p in ('FogMS_CloudBoxCenter','FogMS_WorldExtent','FogMS_CloudWorldToLocal2','FogMS_PrefilterWavelengths')}}\n"
        "out['cvars']={n: unreal.SystemLibrary.get_console_variable_float_value(n) for n in %r}\n"
        "print('D46STATE '+json.dumps(out))") % (ENGINE_CVARS + PLUGIN_CVARS,)
    return pyj(code, "D46STATE")


def snapshot(force=False):
    if os.path.isfile(OWNER) and not force:
        return json.load(open(OWNER))
    s = state()
    s["note"] = "round 46 snapshot, read before any change in the session the owner left (camera = his)"
    s["time"] = time.strftime("%Y-%m-%d %H:%M:%S")
    _atomic_dump(s, OWNER)
    return s


# ------------------------------------------------------------------ changes (all undone by restore)
def set_throttle(flag):
    return d.py("import unreal\nperf=unreal.get_default_object(unreal.load_class(None, '/Script/UnrealEd.EditorPerformanceSettings'))\n"
                "perf.set_editor_property('bThrottleCPUWhenNotForeground', %s)\nprint('THROTTLE', perf.get_editor_property('bThrottleCPUWhenNotForeground'))" % bool(flag))


def set_box(**props):
    code = FIND
    for k, v in props.items():
        if k == "render_path":
            code += "box.set_editor_property('render_path', unreal.FogMSRenderPath.%s)\n" % v
        else:
            code += "box.set_editor_property(%r, %r)\n" % (k, v)
    code += "box.update_density()\nprint('SETBOX ok')"
    return d.py(code)


def place_box(loc=None, rot=None, scale=None):
    code = FIND
    if loc is not None:
        code += "box.set_actor_location(unreal.Vector(%r, %r, %r), False, True)\n" % tuple(loc)
    if rot is not None:
        code += "box.set_actor_rotation(unreal.Rotator(pitch=%r, yaw=%r, roll=%r), True)\n" % tuple(rot)
    if scale is not None:
        code += "box.set_actor_scale3d(unreal.Vector(%r, %r, %r))\n" % tuple(scale)
    code += "print('PLACED')"
    return d.py(code)


def create_host():
    return d.py(FIND + "box.create_cloud_host()\nprint('CREATE called')")


def destroy_script_host():
    """Destroys the host(s) labelled 'FogMS Cloud Host' that this script created (only when the snapshot had none)."""
    return d.py(FIND + "n=0\nfor h in [a for a in hosts if a.get_actor_label()==%r]:\n"
                "    unreal.get_editor_subsystem(unreal.EditorActorSubsystem).destroy_actor(h); n+=1\nprint('DESTROYED', n)" % HOST_LABEL)


def begin(owner):
    if not LOG or not os.path.isfile(LOG):
        raise SystemExit("Set FOGMS_LOG to the running editor's log file.")
    if editor_minimized():
        raise SystemExit("The Unreal Editor window is minimized: nothing would render. Nothing changed; retry when it is restored.")
    flood_check()
    set_throttle(False)
    d.cmd("ShowFlag.OverrideDiffuseAndSpecular 0")        # Lit albedo in every view during the run


def restore(owner):
    """Everything this script may change back to the snapshot; prints the diff (must be {})."""
    b = owner["box"]
    now = state()
    if not owner.get("hosts"):
        print(destroy_script_host(), flush=True)
    props = {}
    if rp_name(now["box"]["render_path"]) != rp_name(b["render_path"]):
        props["render_path"] = rp_name(b["render_path"])
    for k in ("sun_softness", "host_prefilter"):
        if abs(float(now["box"][k]) - float(b[k])) > 1e-6:
            props[k] = float(b[k])
    if props:
        print(set_box(**props), flush=True)
    moved = max(abs(x - y) for x, y in zip(now["box"]["location"] + now["box"]["scale"] + now["box"]["rotation"],
                                              b["location"] + b["scale"] + b["rotation"]))
    if moved > 1e-4:
        print(place_box(loc=b["location"], rot=b["rotation"], scale=b["scale"]), flush=True)
    d.set_cam(*owner["camera"])
    set_throttle(owner["throttle"])
    d.cmd("ShowFlag.OverrideDiffuseAndSpecular 2")        # the viewport's own mode (VMI_Lit in the ini; never Detail Lighting)
    time.sleep(SETTLE)
    after = state()
    diff = {}
    for k in ("render_path", "sun_softness", "host_prefilter"):
        if after["box"][k] != b[k]:
            diff[k] = (b[k], after["box"][k])
    for k in ("location", "rotation", "scale"):
        if max(abs(x - y) for x, y in zip(after["box"][k], b[k])) > 1e-3:
            diff[k] = (b[k], after["box"][k])
    if sorted(after["hosts"]) != sorted(owner.get("hosts", [])):
        diff["hosts"] = (owner.get("hosts"), after["hosts"])
    cam = max(abs(x - y) for x, y in zip(after["camera"][0] + after["camera"][1], owner["camera"][0] + owner["camera"][1]))
    if cam > 1e-2:
        diff["camera"] = cam
    if after["throttle"] != owner["throttle"]:
        diff["throttle"] = (owner["throttle"], after["throttle"])
    cv = {k: (v, after["cvars"].get(k)) for k, v in owner["cvars"].items() if k.startswith("r.FogMS.") and abs(v - after["cvars"].get(k, 1e30)) > 1e-6}
    if cv:
        diff["plugin_cvars"] = cv
    # Engine cloud / render-target cvars are the plugin's business (restored by it when no Box uses a host): reported, never set here.
    eng = {k: (v, after["cvars"].get(k)) for k, v in owner["cvars"].items() if not k.startswith("r.FogMS.") and abs(v - after["cvars"].get(k, 1e30)) > 1e-6}
    print("RESTORE diff", diff, flush=True)
    print("RESTORE engine cvars differing from the snapshot (plugin-managed, informational):", eng, flush=True)
    print("RESTORE host layer: snapshot %s -> now %s (the plugin refits it to the Box band when r.FogMS.CloudHost.FitLayer is 1)"
          % ((owner.get("host") or {}).get("layer_km"), (after.get("host") or {}).get("layer_km")), flush=True)
    return {"diff": diff, "engine_cvars": eng, "status": after["box"]["status"][-400:]}


# ------------------------------------------------------------------ GPU profile (one frame, ProfileGPU, parsed from the log)
ROW = re.compile(r".*[\u2502\u250a]\s*([\d.]+) ms\s*\u2503\s*(\S.*?)\s*\u2503")


def profile(label):
    mark = marker("PROFILE_%s" % label)
    d.cmd("r.ProfileGPU.ShowUI 0"); d.cmd("ProfileGPU")
    time.sleep(6.0)
    d.cmd("FLUSHLOG"); time.sleep(1.0)
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
    pre = lambda prefix: round(sum(ms for ms, n in rows if n.startswith(prefix)), 3)
    r = {"frame_ms": frame, "trace": tot({"VolumetricCloud"}), "reconstruct": tot({"VolCloudReconstruction"}),
         "compose": tot({"VolCloudComposeOverScene"}), "cloud_shadow": tot({"VolumetricCloudShadow"}),
         "fog": tot({"ComputeVolumetricFog"}) or tot({"VolumetricFog"}), "fogms": pre("FogMS"), "events": len(rows),
         "cloud_rows": [[ms, n] for ms, n in rows if "Cloud" in n][:24],
         "pass2": [[ms, n] for ms, n in rows if "direct cell average" in n]}
    r["cloud_ms"] = round(r["trace"] + r["reconstruct"] + r["compose"], 3)
    return r


def pass2_points(label, tries=4):
    """Pass-2 event names of one solve frame (SolveInterval N: a profiled frame may be a hold frame; retried)."""
    for t in range(tries):
        flood_check()
        p = profile("%s_%d" % (label, t))
        if "error" in p:
            return {"error": p["error"]}
        if p["pass2"]:
            names = sorted(set(n for _, n in p["pass2"]))
            return {"names": names, "ms": [ms for ms, _ in p["pass2"]], "try": t,
                    "eight": any("[sun 8 points]" in n for n in names), "four": any("[sun 4 points" in n for n in names)}
    return {"error": "no solve frame in %d profiles" % tries}


# ------------------------------------------------------------------ checks
def near_state(status):
    m = re.search(r"(near|far) settings \(camera ([\d.\-]+) km, near within ([\d.]+) km\)", status)
    if m:
        return m.group(1), float(m.group(2)), float(m.group(3))
    return ("near", None, 0.0) if "near settings always" in status else (None, None, None)


def expected_defaults(cv, where):
    near = where != "far"
    return {"r.VolumetricRenderTarget.Mode": cv["r.FogMS.CloudHost.RTMode" if near else "r.FogMS.CloudHost.FarRTMode"],
            "r.VolumetricCloud.SampleMinCount": cv["r.FogMS.CloudHost.SampleMinCount" if near else "r.FogMS.CloudHost.FarSampleMinCount"],
            "r.VolumetricRenderTarget.UpsamplingMode": cv["r.FogMS.CloudHost.UpsamplingMode"],
            "r.VolumetricRenderTarget.ReprojectionBoxConstraint": cv["r.FogMS.CloudHost.ReprojectionBoxConstraint"],
            "r.VolumetricRenderTarget.MinimumDistanceKmToEnableReprojection": cv["r.FogMS.CloudHost.ReprojectionMinKm"]}


def defaults_report(st):
    cv = st["cvars"]
    where, km, near_km = near_state(st["box"]["status"])
    want = expected_defaults(cv, where)
    if cv["r.FogMS.CloudHost.StepSettings"] >= 1 and st.get("host"):
        want["r.VolumetricCloud.DistanceToSampleMaxCount"] = max(st["host"]["trace_km"], 0.1)
    got = {k: cv.get(k) for k in want}
    bad = {k: (want[k], got[k]) for k in want if want[k] >= 0 and abs(want[k] - got[k]) > 1e-4}
    host = st.get("host") or {}
    mid = (host.get("mid") or {}).get("scalars", {})
    vss = cv["r.FogMS.CloudHost.ViewSampleScale"]
    return {"near_far": [where, km, near_km], "want": want, "got": got, "mismatch": bad,
            "host_view_scale": [vss, host.get("view_scale")], "host_view_scale_ok": vss <= 0 or abs(vss - (host.get("view_scale") or 0)) < 1e-4,
            "mid_step_cm": mid.get("FogMS_CloudStep"), "mid_prefilter": mid.get("FogMS_CloudPrefilter"),
            "box_host_prefilter": st["box"]["host_prefilter"],
            "prefilter_ok": abs((mid.get("FogMS_CloudPrefilter") or -1) - st["box"]["host_prefilter"]) < 1e-5,
            "ok": not bad and (vss <= 0 or abs(vss - (host.get("view_scale") or 0)) < 1e-4)}


def rendered(st):
    h = st.get("host") or {}
    dens = ((h.get("mid") or {}).get("scalars") or {}).get("FogMS_Density") or 0.0
    return "[render: cloud host]" in st["box"]["status"] and dens > 0.0


def layer(st):
    lk = (st.get("host") or {}).get("layer_km") or [None, None]
    return [lk[0], (lk[0] + lk[1]) if lk[0] is not None else None]


def center(st):
    return (((st.get("host") or {}).get("mid") or {}).get("vectors") or {}).get("FogMS_CloudBoxCenter")


def step(res, key, value):
    res[key] = value
    _atomic_dump(res, OUT)
    print("STEP %-14s %s" % (key, json.dumps(value)[:700]), flush=True)


def check(owner):
    res = _load(OUT)
    res["check"] = {"started": time.strftime("%Y-%m-%d %H:%M:%S")}
    C = res["check"]
    begin(owner)
    mark = marker("CHECK")
    try:
        # A. a host bound to the Box (Render Path Cloud Host; a host created by the button if the level has none)
        st = state()
        if rp_name(st["box"]["render_path"]) != "CLOUD_HOST":
            print(set_box(render_path="CLOUD_HOST"), flush=True)
        if not st["hosts"]:
            print(create_host(), flush=True)
        time.sleep(SETTLE)
        st = state()
        step(res, "check", dict(C, A_bound={"rendered": rendered(st), "status": st["box"]["status"][-500:], "host": st.get("host")}))
        C = res["check"]
        if not rendered(st):
            raise RuntimeError("the Box does not render through the host: %s" % st["box"]["status"][-300:])
        # B. defaults applied
        C["B_defaults"] = defaults_report(st)
        step(res, "check", C)
        # C. Sun Softness -> 8 sun points (and 4 at 0 with DirectSamples 4)
        ss0 = float(st["box"]["sun_softness"])
        ss = ss0 if ss0 > 0 else 5.0
        if ss != ss0:
            print(set_box(sun_softness=ss), flush=True); time.sleep(SETTLE)
        c = {"sun_softness": ss, "soft": pass2_points("soft")}
        if int(st["cvars"]["r.FogMS.Transport.DirectSamples"]) == 4:
            print(set_box(sun_softness=0.0), flush=True); time.sleep(SETTLE)
            c["zero"] = pass2_points("zero")
        print(set_box(sun_softness=ss0), flush=True); time.sleep(SETTLE)
        c["ok"] = bool(c["soft"].get("eight")) and (("zero" not in c) or bool(c["zero"].get("four")))
        C["C_sun_points"] = c
        step(res, "check", C)
        # D. host follows the Box: move up 30 m, +2 m (hysteresis), Z scale x1.5, back
        b0 = st["box"]
        base = state()
        seq = []
        def probe(name, loc=None, scale=None):
            flood_check()
            place_box(loc=loc, scale=scale); time.sleep(SETTLE)
            s = state()
            seq.append({"name": name, "rendered": rendered(s), "layer_km": layer(s), "center": center(s),
                        "extent": (((s.get("host") or {}).get("mid") or {}).get("vectors") or {}).get("FogMS_WorldExtent"),
                        "status": s["box"]["status"][-260:]})
            print("FOLLOW %-10s rendered %s layer %s centre %s" % (name, seq[-1]["rendered"], seq[-1]["layer_km"], seq[-1]["center"]), flush=True)
            return s
        L0 = b0["location"]
        probe("up30", loc=[L0[0], L0[1], L0[2] + 3000.0])
        probe("up32", loc=[L0[0], L0[1], L0[2] + 3200.0])
        probe("scale_z", scale=[b0["scale"][0], b0["scale"][1], b0["scale"][2] * 1.5])
        probe("back", loc=L0, scale=b0["scale"])
        l0, c0 = layer(base), center(base)
        e = {s["name"]: s for s in seq}
        dz = lambda a, b: (a[2] - b[2]) if a and b else None
        f = {"base": {"layer_km": l0, "center": c0}, "seq": seq,
             "all_rendered": all(s["rendered"] for s in seq),
             "centre_follows_up30_cm": dz(e["up30"]["center"], c0),
             "layer_shift_up30_km": (e["up30"]["layer_km"][0] - l0[0]) if l0[0] is not None and e["up30"]["layer_km"][0] is not None else None,
             "hysteresis_no_refit_2m": e["up32"]["layer_km"] == e["up30"]["layer_km"],
             "layer_height_scale_km": [l0[1] - l0[0] if l0[0] is not None else None,
                                       (e["scale_z"]["layer_km"][1] - e["scale_z"]["layer_km"][0]) if e["scale_z"]["layer_km"][0] is not None else None],
             "back_layer_km": e["back"]["layer_km"]}
        f["ok"] = (f["all_rendered"] and f["centre_follows_up30_cm"] is not None and abs(f["centre_follows_up30_cm"] - 3000.0) < 1.0
                   and f["layer_shift_up30_km"] is not None and abs(f["layer_shift_up30_km"] - 0.030) < 0.006 and f["hysteresis_no_refit_2m"])
        C["D_follow"] = f
        step(res, "check", C)
        # E. near / far settings (camera only)
        cv = st["cvars"]
        near_km = float(cv["r.FogMS.CloudHost.NearDistanceKm"])
        if near_km > 0:
            cam = owner["camera"]
            bl = b0["location"]
            dx, dy = cam[0][0] - bl[0], cam[0][1] - bl[1]
            n = math.hypot(dx, dy) or 1.0
            far = near_km * 1.0e5 * 1.5 + max(b0["extent"])
            loc = [bl[0] + dx / n * far, bl[1] + dy / n * far, bl[2]]
            yaw = math.degrees(math.atan2(-dy, -dx))
            d.set_cam(loc, [0.0, yaw, 0.0]); time.sleep(SETTLE + 1.0)
            sf = state()
            d.set_cam(*owner["camera"]); time.sleep(SETTLE + 1.0)
            sn = state()
            # The engine cvars are the evidence (the Box status may keep an older runtime text while the Box is far off screen).
            g = {"far": {"near_far": near_state(sf["box"]["status"]), "mode": sf["cvars"]["r.VolumetricRenderTarget.Mode"],
                         "min_count": sf["cvars"]["r.VolumetricCloud.SampleMinCount"]},
                 "back": {"near_far": near_state(sn["box"]["status"]), "mode": sn["cvars"]["r.VolumetricRenderTarget.Mode"],
                          "min_count": sn["cvars"]["r.VolumetricCloud.SampleMinCount"]}}
            g["ok"] = (abs(g["far"]["mode"] - cv["r.FogMS.CloudHost.FarRTMode"]) < 1e-4
                       and abs(g["far"]["min_count"] - cv["r.FogMS.CloudHost.FarSampleMinCount"]) < 1e-4)
            C["E_near_far"] = g
        else:
            C["E_near_far"] = {"skipped": "r.FogMS.CloudHost.NearDistanceKm 0 (always near)"}
        step(res, "check", C)
    finally:
        d.cmd("FLUSHLOG"); time.sleep(1.0)
        text = since(mark)
        lines = text.splitlines()
        C["F_log"] = {"python_errors": text.count("LogPython: Error"),
                      "ensures": [l[:300] for l in lines if "Ensure condition failed" in l][:5],
                      "material_failed": [l[:300] for l in lines if "Failed to compile Material" in l and "FogMS" in l][:5],
                      "multilobespec_errors": [l[:300] for l in lines if "LogMultiLobeSpec: Error" in l][:10],
                      "host_lines": [l[:300] for l in lines if "FogMS cloud host" in l][-30:]}
        C["F_log"]["ok"] = not (C["F_log"]["python_errors"] or C["F_log"]["ensures"] or C["F_log"]["material_failed"]
                                or C["F_log"]["multilobespec_errors"])
        C["F_log"]["refit_lines"] = sum(1 for l in lines if "fitted to Box" in l)
        C["F_log"]["settings_lines"] = sum(1 for l in lines if "FogMS cloud host settings" in l)
        step(res, "check", C)
        res["restore_check"] = restore(owner)
        _atomic_dump(res, OUT)
    summary()
    return res


def cost(owner):
    res = _load(OUT)
    begin(owner)
    try:
        st = state()
        if rp_name(st["box"]["render_path"]) != "CLOUD_HOST":
            print(set_box(render_path="CLOUD_HOST"), flush=True)
        if not st["hosts"]:
            print(create_host(), flush=True)
        d.set_cam(*owner["camera"]); time.sleep(SETTLE + 2.0)
        st = state()
        runs = []
        for i in range(3):
            flood_check()
            if editor_minimized():
                raise SystemExit("editor minimized: profiling refused")
            p = profile("cost_%d" % i)
            if "error" in p:
                raise RuntimeError(p["error"])
            runs.append(p)
            print("COST %d cloud %.3f (trace %.3f rec %.3f comp %.3f) shadow %.3f fogms %.3f frame %s" % (
                i, p["cloud_ms"], p["trace"], p["reconstruct"], p["compose"], p["cloud_shadow"], p["fogms"], p["frame_ms"]), flush=True)
        med = {k: round(sorted(r[k] for r in runs)[1], 3) for k in ("cloud_ms", "trace", "reconstruct", "compose", "cloud_shadow", "fog", "fogms")}
        frames = sorted(r["frame_ms"] for r in runs if r["frame_ms"] is not None)
        med["frame_ms"] = frames[len(frames) // 2] if frames else None
        res["cost"] = {"median": med, "runs": runs, "status": st["box"]["status"][-500:],
                       "settings": {k: st["cvars"][k] for k in ENGINE_CVARS}, "host": st.get("host"),
                       "camera": "owner", "time": time.strftime("%Y-%m-%d %H:%M:%S")}
        _atomic_dump(res, OUT)
        print("COST median", json.dumps(med), flush=True)
    finally:
        res["restore_cost"] = restore(owner)
        _atomic_dump(res, OUT)
    return res


def matedit():
    out = S.run_file("matedit_cloud.py")
    lines = [l for l in out.splitlines() if l.split(" ")[0] in ("MATERIAL_OK", "ALREADY_PATCHED", "TRACE", "NOT", "ROLLBACK", "REBUILD",
                                                               "CREATED", "COMPILE", "LINKS", "REPORT", "NOTE")]
    res = _load(OUT)
    res["matedit"] = {"lines": lines[-20:], "ok": any(l.startswith(("MATERIAL_OK", "ALREADY_PATCHED")) for l in lines),
                      "time": time.strftime("%Y-%m-%d %H:%M:%S")}
    _atomic_dump(res, OUT)
    print("\n".join(lines), flush=True)
    return res["matedit"]


def copyback():
    """The saved host material and instance from the project's installed plugin into this worktree (for the commit)."""
    out = {}
    for name in ("M_FogMS_Cloud.uasset", "MI_FogMS_Cloud.uasset"):
        src = os.path.join(PROJECT, "Plugins", "MultiLobeSpec", "Content", "FogMS", name)
        dst = os.path.join(WORKTREE, "Content", "FogMS", name)
        if not os.path.isfile(src):
            out[name] = "missing in the project: %s" % src
            continue
        h = lambda p: hashlib.sha256(open(p, "rb").read()).hexdigest()[:16] if os.path.isfile(p) else None
        if h(src) == h(dst):
            out[name] = "identical"
            continue
        shutil.copy2(src, dst)
        out[name] = "copied %s -> %s (%s)" % (src, dst, h(dst))
    print(json.dumps(out, indent=1), flush=True)
    return out


def summary():
    res = _load(OUT)
    C = res.get("check", {})
    rows = [("A host bound, '[render: cloud host]'", (C.get("A_bound") or {}).get("rendered")),
            ("B defaults applied (engine cvars, host view scale)", (C.get("B_defaults") or {}).get("ok")),
            ("B host prefilter written (MID = Box)", (C.get("B_defaults") or {}).get("prefilter_ok")),
            ("C Sun Softness -> [sun 8 points]", (C.get("C_sun_points") or {}).get("ok")),
            ("D host follows move/scale, layer refit + hysteresis", (C.get("D_follow") or {}).get("ok")),
            ("E far settings beyond NearDistanceKm", (C.get("E_near_far") or {}).get("ok", "skipped")),
            ("F no errors in the log", (C.get("F_log") or {}).get("ok")),
            ("restore diff empty", not ((res.get("restore_check") or {}).get("diff")))]
    for name, ok in rows:
        print("%-55s %s" % (name, ok), flush=True)
    if res.get("matedit"):
        print("matedit: %s" % res["matedit"].get("lines", [])[-3:], flush=True)
    if res.get("cost"):
        print("cost (owner camera, median of 3): %s" % json.dumps(res["cost"]["median"]), flush=True)


def main(cmd):
    if cmd == "copyback":
        copyback(); return
    if cmd == "summary":
        summary(); return
    owner = snapshot(force="--force" in sys.argv and cmd == "snapshot")
    if cmd == "snapshot":
        print(json.dumps(owner, indent=1)[:3000]); return
    if cmd == "matedit":
        matedit(); return
    if cmd == "check":
        check(owner); return
    if cmd == "cost":
        cost(owner); summary(); return
    if cmd == "restore":
        restore(owner); return
    if cmd == "all":
        m = matedit()
        if not m["ok"]:
            raise SystemExit("matedit_cloud.py did not finish (see results/diag46/d46.json): nothing else run")
        check(owner)
        cost(owner)
        summary()
        return
    raise SystemExit("unknown subcommand " + cmd)


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "snapshot")
