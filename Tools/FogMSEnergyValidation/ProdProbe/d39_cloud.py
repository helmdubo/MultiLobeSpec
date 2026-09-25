# -*- coding: utf-8 -*-
"""Round 39 / P1 series (FogMS_PerPixelClouds_Design.md sections 4-5): the owner's Live Box as froxels (round 38) vs as the
per-pixel cloud-host prototype (cloudproto_session.py: engine Volumetric Cloud + M_FogMS_CloudBox_P1, froxel copy weighted 0).
Every run: sky Volumetric Cloud hidden (one cloud component renders; also the round-38b methodology), SkyLight real-time capture
TimeSlice 0, background throttle off. Frames: measure/diag39 (junction to D:), results: results/diag39.

Subcommands (each restores the owner snapshot measure/owner_pre39.json at the end and prints 'RESTORE box diff {...}'):
  identity_pre | identity_post   criterion 1: owner view, Box animation frozen at t=100 s, 8 frames x2; run _pre BEFORE
  | identity_late                matedit_density.py adds FogMS_FroxelWeight and _post after it (weight 1): MAE / PSNR of the
                                 mean frames against the pre repeat (noise floor); _late = the post material again tens of
                                 minutes later: the time-separated floor for pre ~ post
  smoke                          prototype on, one froxel and one cloud frame at 'base', status, one GPU profile
  series | series_td | series_w0  criteria 4/5 (d35_dolly metrics, camera 'base', 20 cm/frame, 48 frames): PLAN x {0, W, D};
                                 plus 'inside' (W up into the cloud base: froxel / p1 / p1_m1); _td = world time dilation
                                 0.25 (Edge Flow ~20 cm per captured frame), _w0 = wind 0 (round-38b condition)
  inside_frz                     entering the cloud with the density frozen (same cloud for every variant)
  look                           criteria 3/6: views owner / base / against / front / mine (live sun), Box frozen, configs LOOK;
                                 ROI brightness vs W38, flicker, opacity from albedo-0 renders, IoU, rim/core
  cost                           criterion 7: ProfileGPU x3 per host variant at 'base' and 'owner' (frozen)
  best                           second repeat of the best cloud variant if it is not p1
  summary                        offline: r39_summary.txt, p1_sheet.png, look_sheet.png
  all                            series + look + cost + best + summary in one session
RESUMABLE (power cuts): every run is written atomically to results/diag39/b39.json (look.json, cost.json); present keys are
skipped. Usage: python d39_cloud.py <subcommand>"""
import os, sys, json, time, math, shutil, glob, re
import numpy as np
import diag34lib as d
import diag35lib as L
import d35_dolly as B
import d4_density as D4
import cloudproto_session as S

d.M = os.path.join(d.HERE, "measure", "diag39")          # after the imports: diag35lib sets diag35
os.makedirs(d.M, exist_ok=True)
RES = os.path.join(d.HERE, "results", "diag39"); os.makedirs(RES, exist_ok=True)
LOOKDIR = os.path.join(RES, "look"); os.makedirs(LOOKDIR, exist_ok=True)
RP, LP, CP = os.path.join(RES, "b39.json"), os.path.join(RES, "look.json"), os.path.join(RES, "cost.json")
N = 48
STEP = 20.0
FROZEN_T = 100.0
D37_MINE = ((-282.70751614825076, 8738.93414348489, 2438.480836716684), (4.075094774365425, 243.93856991827488, 0.0))
# 'inside': 50 m along the 'base' view (pitch 35): z 84-93 m over the 48 frames, up into the base of the stratus band (83-127 m).
_bf = B.basis(B.CAMS["base"][1])[0]
B.CAMS["inside"] = (tuple(B.CAMS["base"][0][i] + _bf[i] * 5000.0 for i in range(3)), B.CAMS["base"][1])
# variant: (mode, Box properties, host)
V = {"boxoff": ("boxoff", {}, None), "cur": ("froxel", {}, None), "animoff": ("froxel", {"animate_density": False}, None),
     "p1": ("cloud", {}, "p1"), "p1_animoff": ("cloud", {"animate_density": False}, "p1"), "p1_d1": ("cloud", {}, "p1_d1"),
     "p1_d5": ("cloud", {}, "p1_d5"), "p1_s4": ("cloud", {}, "p1_s4"), "p1_m1": ("cloud", {}, "p1_m1"),
     "p1_ef200": ("cloud", {"edge_flow_speed": 200.0}, "p1")}
PLAN = [("boxoff", 2), ("cur", 2), ("animoff", 2), ("p1", 2), ("p1_animoff", 1), ("p1_d1", 1), ("p1_d5", 1), ("p1_s4", 1),
        ("p1_m1", 1), ("p1_ef200", 1)]
DIRS = ("0", "W", "D")
INSIDE = [("cur", 0), ("p1", 0), ("p1_m1", 0)]
# Frame-rate-corrected animation ('_td'): PNG dumps run the editor at ~6.4 fps (0.155 s per frame, ticks.json), so the Box's
# world-time animation moves Edge Flow ~78 cm and wind ~31 cm per captured frame, four times the design's 20 cm/frame
# (500 cm/s at 25 fps). World time dilation TD during the '_td' runs advances the density ~0.155 * TD = 0.039 s per captured
# frame (Edge Flow ~19 cm, wind ~8 cm). Only game-time animation changes: jitter, blue noise and history are per frame.
TD = 0.25
V.update({"cur_td": ("froxel", {}, None, TD), "p1_td": ("cloud", {}, "p1", TD), "p1_d1_td": ("cloud", {}, "p1_d1", TD),
          "p1_m1_td": ("cloud", {}, "p1_m1", TD), "p1_d5_td": ("cloud", {}, "p1_d5", TD)})
PLAN_TD = [("cur_td", 2), ("p1_td", 2), ("p1_d1_td", 2), ("p1_m1_td", 1), ("p1_d5_td", 1)]
INSIDE_TD = [("cur_td", 0), ("p1_td", 0), ("p1_m1_td", 0)]
# Entering the cloud with the density frozen (manual time 100 s): the same cloud for every variant, only the camera moves,
# so the runs isolate reprojection when the camera enters the cloud (design risk 3: Mode 0 vs Mode 1).
_FRZ = {"use_manual_animation_time": True, "manual_animation_time": FROZEN_T}
V.update({"cur_frz": ("froxel", dict(_FRZ), None), "p1_frz": ("cloud", dict(_FRZ), "p1"), "p1_m1_frz": ("cloud", dict(_FRZ), "p1_m1"),
          "p1_d1_frz": ("cloud", dict(_FRZ), "p1_d1")})
INSIDE_FRZ = [("cur_frz", 0), ("p1_frz", 0), ("p1_m1_frz", 0), ("p1_d1_frz", 0)]
# Round-38b condition: wind 0, only Edge Flow 500 animates (the owner's Box now has Wind Speed 200 cm/s, which also drifts the
# whole field during and between runs). wind_speed is restored from owner_pre39.json extra.box_extra.
V.update({"cur_w0": ("froxel", {"wind_speed": 0.0}, None), "p1_w0": ("cloud", {"wind_speed": 0.0}, "p1"),
          "cur_w0_td": ("froxel", {"wind_speed": 0.0}, None, TD), "p1_w0_td": ("cloud", {"wind_speed": 0.0}, "p1", TD)})
PLAN_W0 = [("cur_w0", 2), ("p1_w0", 2), ("cur_w0_td", 1), ("p1_w0_td", 2)]
# Second repeats and the wind-0 pair of the steadiest animated host (D 5 km: step 6.5 m) found in the _td series.
V.update({"p1_d5_w0_td": ("cloud", {"wind_speed": 0.0}, "p1_d5", TD)})
PLAN_EXTRA = [("p1_d5_td", 2), ("cur_w0_td", 2), ("p1_d5_w0_td", 2)]
# look configs: (name, mode, Box MID vector overrides, cloud MID scalar overrides[, Box MID scalar overrides]).
# W38_nolobe / P1_nolobe / P1_iso_nolobe compare the renderers without the forward lobe (the Box's lobe reads the field's RGB,
# not its alpha: see matedit_density.py summary 'FORWARD FieldA source value type'); P1_iso = the cloud's sun single scattering
# with an isotropic phase, as the froxel fog here (Scattering Distribution 0).
LOOK = [("boxoff", "boxoff", {}, {}), ("W38", "froxel", {}, {}), ("W38_black", "froxel", {"FogMS_Albedo": (0, 0, 0, 1)}, {}),
        ("W38_nolobe", "froxel", {}, {}, {"FogMS_ForwardStrength": 0.0}),
        ("P1", "cloud", {}, {}), ("P1_black", "cloud", {}, {"__albedo0": 1, "P1_FieldGain": 0.0}),
        ("P1_nolobe", "cloud", {}, {"FogMS_ForwardStrength": 0.0}), ("P1_g2", "cloud", {}, {"P1_PhaseG2": -0.3, "P1_PhaseBlend": 0.2}),
        ("P1_iso", "cloud", {}, {"P1_PhaseG": 0.0}), ("P1_iso_nolobe", "cloud", {}, {"P1_PhaseG": 0.0, "FogMS_ForwardStrength": 0.0}),
        ("P1_nofield", "cloud", {}, {"P1_FieldGain": 0.0}), ("P1_rep", "cloud", {}, {}), ("W38_rep", "froxel", {}, {})]
LOOK_FRAMES = 6
LOOK_HOST = "p1"
_state = {"host": None, "flood0": None}


def _atomic_dump(obj, path):
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        json.dump(obj, f, indent=1); f.flush(); os.fsync(f.fileno())
    os.replace(tmp, path)


def _load(path):
    return json.load(open(path)) if os.path.isfile(path) else {}


def flood_check():
    n = S.python_errors()
    if _state["flood0"] is None:
        _state["flood0"] = n
    if n - _state["flood0"] > 20:
        raise RuntimeError("LogPython: Error flood (%d new lines): stopping" % (n - _state["flood0"]))
    return n


# ------------------------------------------------------------------ capture (safe tick callback)
def capture(name, n=N, move=None):
    """n consecutive real viewport frames into measure/diag39/<name>/f###.png (r.DumpingMovie; diag34lib.capture with a callback
    that unregisters itself on the first exception). move = (dx, dy, dz) cm per tick from the arming tick on."""
    out = os.path.join(d.M, name)
    shutil.rmtree(out, ignore_errors=True); os.makedirs(out)
    before = d._frames()
    d.cmd("r.BufferVisualizationOverviewTargets FogMSNoTarget")
    d.cmd("r.BufferVisualizationDumpFrames 1")
    tj = os.path.join(out, "ticks.json").replace("\\", "/")
    code = ("import unreal, time, json\nles=unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem)\n"
            "old=getattr(unreal,'_d39_h',None)\n"
            "if old is not None:\n"
            "    try: unreal.unregister_slate_post_tick_callback(old)\n"
            "    except Exception: pass\n"
            "st={'n':0,'t':[],'cam':[]}\nN=%d\nMV=%r\nPRE=%d\n"
            "def _d39_tick(dt):\n"
            "    try:\n"
            "        l,r=les.get_level_viewport_camera_info()\n"
            "        st['t'].append(time.perf_counter()); st['cam'].append((l.x,l.y,l.z))\n"
            "        if MV is not None:\n"
            "            les.set_level_viewport_camera_info(unreal.Vector(l.x+MV[0],l.y+MV[1],l.z+MV[2]), r)\n"
            "        st['n']+=1\n"
            "        if st['n']==PRE:\n"
            "            unreal.SystemLibrary.execute_console_command(None, 'r.DumpingMovie %%d' %% N)\n"
            "        if st['n']>=PRE+N+3:\n"
            "            unreal.unregister_slate_post_tick_callback(unreal._d39_h); unreal._d39_h=None\n"
            "            open(r'%s','w').write(json.dumps(st))\n"
            "    except Exception as e:\n"
            "        try: unreal.unregister_slate_post_tick_callback(unreal._d39_h)\n"
            "        except Exception: pass\n"
            "        unreal._d39_h=None\n"
            "        try: unreal.log_warning('d39 capture tick stopped: %%s' %% e)\n"
            "        except Exception: pass\n"
            "unreal._d39_h=unreal.register_slate_post_tick_callback(_d39_tick)\nprint('ARMED')" % (n, move, 12 if move else 1, tj))
    d.py(code)
    t0 = time.time()
    while time.time() - t0 < 60 + 2.0 * n:
        if len(d._frames() - before) >= n and os.path.isfile(tj):
            break
        time.sleep(1.0)
    time.sleep(1.0)
    d.cmd("r.BufferVisualizationDumpFrames 0"); d.cmd("r.DumpingMovie 0")
    new = sorted(d._frames() - before)
    for i, f in enumerate(new):
        shutil.move(f, os.path.join(out, "f%03d.png" % i))
    for f in glob.glob(os.path.join(d.SS, "MovieFrame*_*.png")):
        try: os.remove(f)
        except Exception: pass
    if len(new) != n:
        print("CAPTURE %s: %d frames (WANTED %d)" % (name, len(new), n), flush=True)
    return out, len(new)


def set_cam(loc, rot):
    return d.set_cam(loc, rot)


# ------------------------------------------------------------------ state
def box_assign(props):
    return "\n".join("b.set_editor_property(%r, %r)" % (k, v) for k, v in props.items())


def apply_variant(variant):
    mode, box, host = V[variant][:3]
    td = V[variant][3] if len(V[variant]) > 3 else None
    if host and _state["host"] != host:
        S.apply_host(host); _state["host"] = host
    if box:
        d.setbox(box_assign(box))
        S.sync()
    if td:
        S.set_time_dilation(td)
    return S.mode(mode)


def revert_variant(variant, owner):
    box = V[variant][1]
    if len(V[variant]) > 3 and V[variant][3]:
        S.set_time_dilation(owner["extra"].get("time_dilation", 1.0))
    if box:
        extra = owner.get("extra", {}).get("box_extra", {})
        d.setbox(box_assign({k: (owner["box"][k] if k in owner["box"] else extra[k]) for k in box}))
        S.sync()


def editor_minimized():
    """True when the Unreal Editor main window is minimized: its viewports then render nothing (0 dumped frames, empty GPU
    profiles). Round 39: the owner started a game mid-series and the editor was minimized. Never restore/focus it from here."""
    try:
        import ctypes, subprocess
        out = subprocess.run(["powershell", "-NoProfile", "-Command",
                              "(Get-Process UnrealEditor -ErrorAction SilentlyContinue | Select-Object -First 1).MainWindowHandle"],
                             capture_output=True, text=True, timeout=30).stdout.strip()
        return bool(out.isdigit() and ctypes.windll.user32.IsIconic(int(out)))
    except Exception:
        return False


def begin(owner, host="p1"):
    """Owner state first (a cut run may have left anything), then the prototype, froxel mode."""
    if editor_minimized():
        raise SystemExit("The Unreal Editor window is minimized: nothing would render. Nothing changed; retry when it is restored.")
    flood_check()
    S.restore(owner)
    r = S.start(host, froxel=True); _state["host"] = host
    print("BEGIN", json.dumps(r["host"]), json.dumps(r["state"]), flush=True)
    return r


def end(owner):
    return S.restore(owner)


def freeze(on, owner):
    if on:
        d.setbox("b.set_editor_property('manual_animation_time', %r)\nb.set_editor_property('use_manual_animation_time', True)" % FROZEN_T)
    else:
        d.setbox("b.set_editor_property('use_manual_animation_time', %s)\nb.set_editor_property('manual_animation_time', %r)"
                 % (owner["box"]["use_manual_animation_time"], owner["box"]["manual_animation_time"]))
    S.sync()


# ------------------------------------------------------------------ series (criteria 4, 5)
def key(variant, D, r, cam="base"):
    return "b39_%s_%s_%s_s20_%d" % (cam, variant, D, r)


def run(variant, rep, dirs, owner, cam="base"):
    res = _load(RP)
    missing = [D for D in dirs if key(variant, D, rep, cam) not in res]
    if not missing:
        return
    st = apply_variant(variant)
    time.sleep(6.0)
    loc, rot = B.CAMS[cam]
    fwd, right = B.basis(rot)
    vec = {"W": fwd, "D": right, "0": (0.0, 0.0, 0.0)}
    try:
        for D in missing:
            flood_check()
            u = vec[D]; mv = tuple(c * STEP for c in u)
            c0 = tuple(loc[i] - u[i] * STEP * (12 + N / 2.0) for i in range(3))
            set_cam(c0, rot); time.sleep(2.5)
            nm = key(variant, D, rep, cam)
            _, got = capture(nm, N, move=mv if D != "0" else None)
            if got < N:
                raise RuntimeError("%s: %d of %d frames (viewport not rendering: editor minimized?); run stopped, nothing recorded" % (nm, got, N))
            r = B.analyse(nm, None); r.update(B.wobble(nm, None))
            r.update({"frames_got": got, "status": d.status()[:200], "p1_state": S.set_state(), "host": _state["host"] if V[variant][0] == "cloud" else None})
            try:   # wall time per captured frame and the density animation time it advanced (x time dilation)
                t = np.diff(np.array(json.load(open(os.path.join(d.M, nm, "ticks.json")))["t"]))
                pre = 12 if D != "0" else 1
                dt = float(np.median(t[pre:pre + N])); td = V[variant][3] if len(V[variant]) > 3 else 1.0
                r.update({"dump_dt": round(dt, 4), "anim_s_per_frame": round(dt * (td or 1.0), 4)})
            except Exception as e:
                r["dump_dt_error"] = str(e)
            res = _load(RP); res[nm] = r; _atomic_dump(res, RP)
            print("%-30s wob %s/%s | d2b %.3f | d1 %.3f mean %.1f | %s" % (nm, r.get("wob_med"), r.get("wob_p75"), r["d2_blur"], r["d1"], r["mean"],
                                                                        r["status"][:60]), flush=True)
    finally:
        revert_variant(variant, owner)


def series(owner, td=False):
    for variant, reps in (PLAN_TD if td else PLAN):
        for rep in range(reps):
            run(variant, rep, DIRS, owner)
    for variant, rep in (INSIDE_TD if td else INSIDE):
        run(variant, rep, ("W",), owner, cam="inside")


def best_variant(td=False):
    """Lowest mean static wob_med among cloud host variants (the '_td' runs if td) whose cost (base view) is <= 3 ms, else the
    lowest overall. Returns the variant name (with '_td' if td)."""
    res, cost = _load(RP), _load(CP)
    cand = []
    for v in ("p1", "p1_d1", "p1_d5", "p1_s4", "p1_m1"):
        name = v + ("_td" if td else "")
        w = [res[k]["wob_med"] for k in res if k.startswith("b39_base_%s_0_s20_" % name) and "wob_med" in res[k]]
        if not w: continue
        ms = (cost.get(v, {}).get("base") or {}).get("cloud_ms_median")
        cand.append((ms is not None and ms <= 3.0, -np.mean(w), name))
    if not cand: return "p1_td" if td else "p1"
    return sorted(cand)[-1][2]


def best(owner, td=False):
    """Second repeat of the best cloud variant when its plan has one repeat only (criterion 4 asks for two)."""
    b = best_variant(td)
    reps = dict(PLAN_TD if td else PLAN).get(b, 1)
    if reps < 2:
        run(b, 1, DIRS, owner)
    return b


# ------------------------------------------------------------------ identity (criterion 1)
def identity(tag, owner):
    """Owner view, Box frozen; before the patch (tag pre) the material has no FogMS_FroxelWeight, after it (post) weight 1."""
    res = _load(RP)
    names = ["id_%s_a" % tag, "id_%s_b" % tag]
    if all(n in res for n in names):
        return
    if editor_minimized():
        raise SystemExit("The Unreal Editor window is minimized: nothing would render. Nothing changed; retry when it is restored.")
    flood_check()
    S.restore(owner)
    L.set_throttle(False); d.cmd("r.SkyLight.RealTimeReflectionCapture.TimeSlice 0")
    d.py(S.FIND + "[a.set_is_temporarily_hidden_in_editor(True) for a in sky]\nprint('sky hidden')")
    try:
        freeze(True, owner)
        set_cam(*owner["camera"]); time.sleep(10.0)
        for nm in names:
            _, got = capture(nm, 8)
            F = d.load(nm)[-4:].mean(axis=0)
            np.save(os.path.join(d.M, nm, "mean.npy"), F.astype(np.float32))
            res = _load(RP); res[nm] = {"frames_got": got, "weight": S.weight(), "status": d.status()[:200]}; _atomic_dump(res, RP)
            print("IDENTITY", nm, res[nm], flush=True)
            time.sleep(3.0)
    finally:
        freeze(False, owner)
        end(owner)


def identity_report():
    res = _load(RP); out = {}
    def mean(nm):
        p = os.path.join(d.M, nm, "mean.npy")
        return np.load(p).astype(np.float64) if os.path.isfile(p) else None
    def cmp(a, b):
        A, Bm = mean(a), mean(b)
        if A is None or Bm is None: return None
        diff = np.abs(A - Bm); mse = float((diff ** 2).mean())
        return {"mae": round(float(diff.mean()), 4), "p99": round(float(np.percentile(diff, 99)), 2),
                "psnr": round(10 * math.log10(255.0 ** 2 / max(mse, 1e-12)), 2)}
    # late = the same material as post (weight 1), captured tens of minutes later: the time-separated floor for pre ~ post
    for a, b in (("id_pre_a", "id_pre_b"), ("id_post_a", "id_post_b"), ("id_pre_a", "id_post_a"), ("id_pre_b", "id_post_b"),
                 ("id_pre_b", "id_post_a"), ("id_late_a", "id_late_b"), ("id_post_a", "id_late_a"), ("id_post_b", "id_late_b"),
                 ("id_pre_a", "id_late_a")):
        out["%s~%s" % (a, b)] = cmp(a, b)
    return out


# ------------------------------------------------------------------ look (criteria 3, 6)
def look_views(owner):
    VIEWS, travel, elev = sun_views()
    VIEWS = dict([("owner", (tuple(owner["camera"][0]), tuple(owner["camera"][1]))), ("base", B.CAMS["base"])] + list(VIEWS.items()))
    return VIEWS, travel, elev


def sun_views():
    out = d.py("import unreal\n"
               "s=[a for a in unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors() if isinstance(a,unreal.DirectionalLight)][0]\n"
               "v=s.get_actor_forward_vector(); print('FWD %r %r %r' % (v.x, v.y, v.z))")
    fx, fy, fz = [float(x) for x in out.split("FWD")[-1].split()[:3]]
    travel = math.degrees(math.atan2(fy, fx)); elev = math.degrees(math.asin(max(-1.0, min(1.0, -fz))))
    return {"against": D4.view_at(travel + 10.0, 3000.0, 4.0), "front": D4.view_at(travel + 180.0 + 10.0, 3500.0, -2.0),
            "mine": D37_MINE}, travel, elev


def apply_look(cfg, owner_albedo, owner_lobe=0.0):
    """1) Box MID: albedo and lobe strength of this config (else the owner's), 2) sync: the cloud MID copies the Box MID and
    takes this config's cloud overrides, 3) the cloud MID albedo (never the Box's zeroed one), 4) mode."""
    name, mode, box_vec, cloud_sc = cfg[:4]
    box_sc = cfg[4] if len(cfg) > 4 else {}
    alb = box_vec.get("FogMS_Albedo", owner_albedo)
    d.py(S.FIND + "bmid.set_vector_parameter_value('FogMS_Albedo', unreal.LinearColor(%r, %r, %r, %r))\n" % tuple(float(c) for c in alb)
         # the Box MID's lobe strength: the Box property (MS Contribution) unless this config overrides it
         + "bmid.set_scalar_parameter_value('FogMS_ForwardStrength', %r)\nprint('BOXLOOK ok')" % float(box_sc.get("FogMS_ForwardStrength", owner_lobe)))
    look = {k: v for k, v in cloud_sc.items() if not k.startswith("__")}
    if "FogMS_ForwardStrength" not in look:
        look["FogMS_ForwardStrength"] = float(owner_lobe)       # the cloud keeps the Box's lobe even after a W38_nolobe config
    S.sync(look)
    d.py(S.FIND + "vc=proto[0].get_component_by_class(unreal.VolumetricCloudComponent); cmid=vc.get_editor_property('material')\n"
         + "cmid.set_vector_parameter_value('FogMS_Albedo', unreal.LinearColor(%s))\nprint('CLOUDLOOK ok')"
         % ("0.0, 0.0, 0.0, 1.0" if "__albedo0" in cloud_sc else "%r, %r, %r, %r" % tuple(float(c) for c in owner_albedo)))
    return S.mode(mode)


def look(owner, host=LOOK_HOST, cfgs=None, prefix="", views=None):
    """Look captures (frozen Box). The main pass uses LOOK_HOST and every LOOK config; a second pass (look_d5) renders the P1
    configs with another host under a name prefix (files look/<prefix><config>_<view>.png)."""
    from PIL import Image
    res = _load(LP)
    VIEWS, travel, elev = look_views(owner)
    if views: VIEWS = {k: v for k, v in VIEWS.items() if k in views}
    S.apply_host(host); _state["host"] = host     # the series may have left another host (e.g. Mode 1)
    if not prefix:
        res.update({"views": {k: [list(v[0]), list(v[1])] for k, v in VIEWS.items()}, "sun": [travel, elev], "host": _state["host"]})
    owner_albedo = owner["extra"]["box_mid_vector"]["FogMS_Albedo"]
    owner_lobe = float(owner["box"]["ms_contribution"])
    freeze(True, owner); time.sleep(8.0)
    try:
        for vn, (loc, rot) in VIEWS.items():
            todo = [c for c in (cfgs or LOOK) if "%s%s|%s" % (prefix, c[0], vn) not in res.get("cap", {})]
            if not todo: continue
            set_cam(loc, rot); time.sleep(4.0)
            for cfg in todo:
                flood_check()
                apply_look(cfg, owner_albedo, owner_lobe); time.sleep(4.0)
                name = prefix + cfg[0]
                nm = "look_%s_%s" % (name, vn)
                _, got = capture(nm, LOOK_FRAMES)
                F = d.load(nm).astype(np.float32)
                img = F[-2:].mean(axis=0)
                Image.fromarray(np.clip(img, 0, 255).astype(np.uint8)).save(os.path.join(LOOKDIR, "%s_%s.png" % (name, vn)))
                L4 = F[-4:].mean(axis=3)
                flick = float(np.mean([np.abs(L4[i] - L4[i - 1]).mean() for i in range(1, len(L4))])) if len(L4) >= 2 else None
                res = _load(LP); res.setdefault("cap", {})["%s|%s" % (name, vn)] = {"frames_got": got, "flicker_abs": flick,
                                                                                  "status": d.status()[:160], "host": host}
                if not prefix:
                    res.update({"views": {k: [list(v[0]), list(v[1])] for k, v in VIEWS.items()}, "sun": [travel, elev], "host": _state["host"]})
                _atomic_dump(res, LP)
                print("LOOK", nm, got, "flicker %.3f" % (flick or -1), flush=True)
    finally:
        apply_look(("restore", "froxel", {}, {}), owner_albedo, owner_lobe)
        freeze(False, owner)
    look_metrics()


LOOK_D5 = [c for c in LOOK if c[0] in ("P1", "P1_black")]


def _lum(p):
    from PIL import Image
    return np.asarray(Image.open(p).convert("RGB"), dtype=np.float64).mean(axis=2)


def look_metrics():
    """ROI brightness vs W38, opacity (1 - black / boxoff), IoU of opacity > 0.5, rim/core of the cloud's own light (lit - black)."""
    res = _load(LP)
    if "views" not in res: return {}
    m = {}
    for vn in res["views"]:
        P = lambda c: os.path.join(LOOKDIR, "%s_%s.png" % (c, vn))
        if not all(os.path.isfile(P(c)) for c in ("boxoff", "W38", "P1")): continue
        off = _lum(P("boxoff")); lit = {c: _lum(P(c)) for c, *_ in LOOK if os.path.isfile(P(c))}
        for c in ("d5_P1", "d5_P1_black"):      # look_d5 pass (host D 5 km)
            if os.path.isfile(P(c)): lit[c] = _lum(P(c))
        valid = off > 12.0
        alpha = {}
        for c, blk in (("W38", "W38_black"), ("P1", "P1_black")):
            if blk in lit:
                a = np.clip(1.0 - lit[blk] / np.maximum(off, 1e-3), 0.0, 1.0); a[~valid] = np.nan; alpha[c] = a
        roi = (np.abs(lit["W38"] - off) > 4) | (np.abs(lit["P1"] - off) > 4)
        e = {"roi_frac": round(float(roi.mean()), 3)}
        for c in lit:
            if c in ("boxoff",) or c.endswith("_black"): continue
            e["mean_%s" % c] = round(float(lit[c][roi].mean()), 2) if roi.any() else None
        for c in lit:
            if e.get("mean_%s" % c) and e.get("mean_W38"):
                e["ratio_%s" % c] = round(e["mean_%s" % c] / e["mean_W38"], 3)
        if "W38" in alpha and "P1" in alpha:
            for th in (0.3, 0.5, 0.7):
                a, b = alpha["W38"] > th, alpha["P1"] > th
                u = (a | b).sum()
                e["iou_a%.1f" % th] = round(float((a & b).sum() / u), 3) if u else None
            ra, rb = np.abs(lit["W38"] - off) > 4, np.abs(lit["P1"] - off) > 4
            e["iou_roi"] = round(float((ra & rb).sum() / max((ra | rb).sum(), 1)), 3)
            if "d5_P1" in lit:
                rd = np.abs(lit["d5_P1"] - off) > 4
                e["iou_roi_d5"] = round(float((ra & rd).sum() / max((ra | rd).sum(), 1)), 3)
            for c, blk in (("W38", "W38_black"), ("W38_nolobe", "W38_black"), ("P1", "P1_black"), ("P1_g2", "P1_black"),
                           ("P1_nolobe", "P1_black"), ("P1_iso", "P1_black"), ("P1_iso_nolobe", "P1_black")):
                if c not in lit or blk not in lit: continue
                a = alpha["W38" if c.startswith("W38") else "P1"]
                own = lit[c] - lit[blk]                      # the cloud's own light (lit minus background seen through it)
                edge = (a >= 0.1) & (a <= 0.5); core = a >= 0.9
                if edge.sum() > 200 and core.sum() > 200:
                    e["rim_core_%s" % c] = round(float(own[edge].mean() / max(own[core].mean(), 1e-3)), 3)
                    e["px_edge_core_%s" % c] = [int(edge.sum()), int(core.sum())]
            e["alpha_mean"] = {c: round(float(np.nanmean(alpha[c])), 3) for c in alpha}
        # Rim / core on the SAME geometry for every render: masks from the cloud's opacity (P1_black is black in the core, a valid
        # opacity; the froxel albedo-0 render is not: see blackdiag.json), thin edge 0.1 <= a <= 0.5, core a >= 0.9. 'lit' ratio
        # uses the lit luminance; 'own' first removes the background seen through the edge, boxoff * (1 - a).
        if "P1" in alpha:
            a = alpha["P1"]; edge = (a >= 0.1) & (a <= 0.5); core = a >= 0.9
            if edge.sum() > 200 and core.sum() > 200:
                for c in ("W38", "W38_nolobe", "P1", "P1_nolobe", "P1_g2", "P1_iso", "P1_iso_nolobe", "P1_nofield", "d5_P1"):
                    if c not in lit: continue
                    own = lit[c] - off * (1.0 - np.nan_to_num(a))
                    e["rimgeo_lit_%s" % c] = round(float(lit[c][edge].mean() / max(lit[c][core].mean(), 1e-3)), 3)
                    e["rimgeo_own_%s" % c] = round(float(own[edge].mean() / max(own[core].mean(), 1e-3)), 3)
                e["rimgeo_px"] = [int(edge.sum()), int(core.sum())]
        cap = res.get("cap", {})
        e["flicker"] = {c: (round(cap["%s|%s" % (c, vn)]["flicker_abs"], 3) if cap.get("%s|%s" % (c, vn), {}).get("flicker_abs") is not None else None)
                        for c in ("W38", "P1", "P1_rep", "W38_rep")}
        m[vn] = e
    res["metrics"] = m; _atomic_dump(res, LP)
    return m


# ------------------------------------------------------------------ cost (criterion 7)
ROW = re.compile(r".*[│┊]\s*([\d.]+) ms\s*┃\s*(\S.*?)\s*┃")


def profile(label):
    marker = "D39_PROFILE_%s_%d" % (label, int(time.time() * 1000))
    d.py("import unreal\nunreal.log(%r)" % marker)
    d.cmd("r.ProfileGPU.ShowUI 0"); d.cmd("ProfileGPU")
    time.sleep(6.0)
    d.cmd("FLUSHLOG"); time.sleep(1.0)
    text = open(S.LOG, "rb").read().decode("utf-8", "replace")
    if marker not in text:
        return {"error": "marker not in log"}
    lines = text.rsplit(marker, 1)[1].splitlines()
    starts = [i for i, l in enumerate(lines) if "GPU Profile for Frame" in l]
    if not starts:
        return {"error": "no profile block"}
    block = []
    for l in lines[starts[0]: starts[0] + 6000]:
        if "LogRHI" not in l: break
        block.append(l)
    frame = next((float(re.search(r"Frame Time\s*:\s*([\d.]+)ms", l).group(1)) for l in block if "Frame Time" in l), None)
    rows = [(float(m.group(1)), m.group(2)) for m in (ROW.search(l) for l in block) if m]
    if len(rows) < 100 or not any(n == "Scene" for _, n in rows):
        return {"error": "no scene passes in the profile (%d events): viewport not rendering, editor minimized?" % len(rows)}
    def tot(names):
        return round(sum(ms for ms, n in rows if n in names), 3)
    def pre(prefix):
        return round(sum(ms for ms, n in rows if n.startswith(prefix)), 3)
    r = {"frame_ms": frame, "trace": tot({"VolumetricCloud"}), "reconstruct": tot({"VolCloudReconstruction"}),
         "compose": tot({"VolCloudComposeOverScene"}), "cloud_shadow": tot({"VolumetricCloudShadow"}),
         "cloudview": pre("CloudView"), "fog": tot({"ComputeVolumetricFog"}) or tot({"VolumetricFog"}), "fogms": pre("FogMS"),
         "events": len(rows)}
    r["cloud_ms"] = round(r["trace"] + r["reconstruct"] + r["compose"], 3)
    return r


def cost(owner):
    res = _load(CP)
    VIEWS = {"base": B.CAMS["base"], "owner": (tuple(owner["camera"][0]), tuple(owner["camera"][1]))}
    freeze(True, owner); time.sleep(6.0)
    try:
        for variant in ("cur", "boxoff") + tuple(S.HOST):
            if all(res.get(variant, {}).get(vn, {}).get("runs") for vn in VIEWS): continue
            flood_check()
            if variant in S.HOST:
                S.apply_host(variant); _state["host"] = variant; S.mode("cloud")
            else:
                S.mode("froxel" if variant == "cur" else "boxoff")
            for vn, (loc, rot) in VIEWS.items():
                set_cam(loc, rot); time.sleep(5.0)
                runs = [profile("%s_%s_%d" % (variant, vn, i)) for i in range(3)]
                ok = [r for r in runs if "error" not in r]
                med = lambda k: round(float(np.median([r[k] for r in ok])), 3) if ok else None
                res = _load(CP); res.setdefault(variant, {})[vn] = {"runs": runs, "cloud_ms_median": med("cloud_ms"),
                                                                     "fog_ms_median": med("fog"), "frame_ms_median": med("frame_ms"),
                                                                     "fogms_ms_median": med("fogms")}
                _atomic_dump(res, CP)
                print("COST %-8s %-6s cloud %s fog %s frame %s" % (variant, vn, med("cloud_ms"), med("fog"), med("frame_ms")), flush=True)
    finally:
        freeze(False, owner)


# ------------------------------------------------------------------ froxel albedo-0 diagnostic
BLACKDIAG = [("lit", {}, {}), ("alb0", {"FogMS_Albedo": (0, 0, 0, 1)}, {}),
             ("alb0_mode0", {"FogMS_Albedo": (0, 0, 0, 1)}, {"FogMS_InjectionMode": 0.0}),
             ("alb0_lobe0", {"FogMS_Albedo": (0, 0, 0, 1)}, {"FogMS_ForwardStrength": 0.0}),
             ("mode0", {}, {"FogMS_InjectionMode": 0.0}), ("weight0", {}, {"FogMS_FroxelWeight": 0.0})]


def blackdiag(owner):
    """Froxel mode, owner view, Box frozen: why the albedo-0 froxel render stays grey (look series W38_black). Each config sets
    Box MID parameters directly (vectors / scalars), 4 frames, mean luminance over the look ROI of the owner view."""
    from PIL import Image
    alb = owner["extra"]["box_mid_vector"]["FogMS_Albedo"]
    freeze(True, owner); S.mode("froxel"); set_cam(*owner["camera"]); time.sleep(8.0)
    off = _lum(os.path.join(LOOKDIR, "boxoff_owner.png")); lit = _lum(os.path.join(LOOKDIR, "W38_owner.png"))
    roi = np.abs(lit - off) > 4
    out = {}
    base_sc = {"FogMS_InjectionMode": None, "FogMS_ForwardStrength": None}
    got = last = None
    try:
        cur = S.last_json(d.py(S.FIND + "print('SC '+json.dumps({p: bmid.get_scalar_parameter_value(p) for p in ('FogMS_InjectionMode','FogMS_ForwardStrength')}))"), "SC")
        base_sc.update(cur)
        for name, vec, sc in BLACKDIAG:
            code = S.FIND + "bmid.set_vector_parameter_value('FogMS_Albedo', unreal.LinearColor(%r, %r, %r, %r))\n" % tuple(float(c) for c in vec.get("FogMS_Albedo", alb))
            for p in ("FogMS_InjectionMode", "FogMS_ForwardStrength"):
                code += "bmid.set_scalar_parameter_value(%r, %r)\n" % (p, float(sc.get(p, base_sc[p])))
            code += "st=getattr(unreal,'_p1_state',None)\nif st is not None: st['weight']=%r\n" % float(sc.get("FogMS_FroxelWeight", 1.0))
            code += "bmid.set_scalar_parameter_value('FogMS_FroxelWeight', %r)\nprint('SET ok')" % float(sc.get("FogMS_FroxelWeight", 1.0))
            d.py(code); time.sleep(5.0)
            nm = "blackdiag_%s" % name
            capture(nm, 4)
            F = d.load(nm).astype(np.float32)[-2:].mean(axis=0)
            Image.fromarray(np.clip(F, 0, 255).astype(np.uint8)).save(os.path.join(LOOKDIR, "blackdiag_%s.png" % name))
            Lm = F.mean(axis=2)
            out[name] = {"roi_mean": round(float(Lm[roi].mean()), 2), "roi_rgb": [round(float(F[..., c][roi].mean()), 1) for c in range(3)]}
            print("BLACKDIAG", name, out[name], flush=True)
    finally:
        code = S.FIND + "bmid.set_vector_parameter_value('FogMS_Albedo', unreal.LinearColor(%r, %r, %r, %r))\n" % tuple(float(c) for c in alb)
        for p in ("FogMS_InjectionMode", "FogMS_ForwardStrength"):
            if base_sc[p] is not None:
                code += "bmid.set_scalar_parameter_value(%r, %r)\n" % (p, float(base_sc[p]))
        code += "print('RESET ok')"
        d.py(code)
        freeze(False, owner)
    out["boxoff_roi_mean"] = round(float(off[roi].mean()), 2)
    _atomic_dump(out, os.path.join(RES, "blackdiag.json"))
    return out


# ------------------------------------------------------------------ smoke
def smoke(owner):
    from PIL import Image
    set_cam(*B.CAMS["base"]); time.sleep(4.0)
    out = {}
    for mode in ("froxel", "cloud", "boxoff"):
        S.mode(mode); time.sleep(5.0)
        nm = "smoke_%s" % mode
        _, got = capture(nm, 4)
        F = d.load(nm).astype(np.float32)
        Image.fromarray(np.clip(F[-1], 0, 255).astype(np.uint8)).save(os.path.join(RES, "smoke_%s.png" % mode))
        out[mode] = {"frames": got, "mean": round(float(F[-1].mean()), 2), "status": d.status()[:200], "state": S.set_state()}
        print("SMOKE", mode, out[mode], flush=True)
    S.mode("cloud"); time.sleep(3.0)
    out["profile_cloud"] = profile("smoke_cloud")
    print("SMOKE profile", out["profile_cloud"], flush=True)
    _atomic_dump(out, os.path.join(RES, "smoke.json"))
    return out


# ------------------------------------------------------------------ summary + sheets (offline)
def boxoff_mean_base():
    """Time-mean luminance of the static Box-off run at 'base' (sky + scene without the Box; the sky is static)."""
    p = os.path.join(d.M, "boxoff_mean_base.npy")
    if os.path.isfile(p): return np.load(p)
    b = key("boxoff", "0", 0)
    if not os.path.isdir(os.path.join(d.M, b)): return None
    m = d.load(b).mean(axis=3).mean(0).astype(np.float32)
    np.save(p, m)
    return m


def wobble_cloud(name, off, tile=64):
    """B.wobble on the cloud alone: this run's own cloud mask (|time-mean of the run - Box-off mean| > 8 levels; the cloud field
    drifts between runs, so coverage differs), tiles >= 80 % cloud. Also the cloud coverage of the frame."""
    from scipy.signal import savgol_filter
    F = d.load(name); Lm = F.mean(axis=3); T, H, W = Lm.shape
    mask = np.abs(Lm.mean(0) - off) > 8.0
    m = B.roi_mask(F) & mask
    res = []
    for y in range(0, H - tile + 1, tile):
        for x in range(0, W - tile + 1, tile):
            mm = m[y:y + tile, x:x + tile]
            if mm.mean() < 0.8: continue
            s = Lm[:, y:y + tile, x:x + tile][:, mm].mean(axis=1)
            r = (s - savgol_filter(s, 21, 2))[3:-3]
            res.append(float(np.sqrt((r ** 2).mean())))
    out = {"cloud_frac": round(float(mask.mean()), 3), "wob_cloud_tiles": len(res), "wob_cloud_med": None, "wob_cloud_p75": None}
    if res:
        out.update({"wob_cloud_med": round(float(np.median(res)), 3), "wob_cloud_p75": round(float(np.percentile(res, 75)), 3)})
    return out


def reanalyse_cloud():
    res = _load(RP); off = boxoff_mean_base()
    if off is None: return
    for k in list(res):
        if k.startswith("b39_base_") and "_0_s20_" in k and "wob_cloud_p75" not in res[k] and os.path.isdir(os.path.join(d.M, k)):
            res[k].update(wobble_cloud(k, off)); _atomic_dump(res, RP)


def fmt(v, f="%.3f"):
    return f % v if isinstance(v, (int, float)) else "  -  "


def summary():
    reanalyse_cloud()
    res, lk, cost_ = _load(RP), _load(LP), _load(CP)
    lines = ["Round 39 / P1: Live Box as froxels (round 38) vs cloud-host prototype (engine Volumetric Cloud). Camera 'base', 20 cm/frame, 48 frames.",
             "wob_med = median tile wobble over the frame (r38b metric), wob_cloud = median over this run's cloud tiles (cov = cloud share",
             "of the frame; the field drifts between runs), d2b = d2 of the 9x9-blurred frame. anim = density animation time per captured",
             "frame (s; live runs ~0.155 = the PNG-dump frame rate, _td runs x%.2f world time dilation)." % TD,
             "variant      rep |  static wob  wob_cloud   cov   d2b |  W wob   d2b  |  D wob   d2b  | anim  | host / status"]
    for variant, reps in PLAN + [(best_variant(), 2)] + PLAN_TD + [(best_variant(True), 2)] + PLAN_W0 + PLAN_EXTRA:
        for rep in range(reps):
            ks = {D: res.get(key(variant, D, rep)) for D in DIRS}
            if not any(ks.values()): continue
            s0, sw, sd = ks["0"] or {}, ks["W"] or {}, ks["D"] or {}
            line = "%-12s %d | %s %s %s %s | %s %s | %s %s | %s | %s" % (
                variant, rep, fmt(s0.get("wob_med")), fmt(s0.get("wob_cloud_med")), fmt(s0.get("cloud_frac"), "%.2f"), fmt(s0.get("d2_blur")), fmt(sw.get("wob_med")),
                fmt(sw.get("d2_blur")), fmt(sd.get("wob_med")), fmt(sd.get("d2_blur")), fmt(s0.get("anim_s_per_frame")),
                (s0.get("host") or "froxel") + " / " + (s0.get("status") or "")[:40])
            if line not in lines: lines.append(line)
    for tag, plan in (("live", INSIDE), ("td", INSIDE_TD), ("frozen density", INSIDE_FRZ)):
        lines.append("inside %s (W up into the cloud base, 50 m along 'base'): " % tag + ", ".join(
            "%s wob %s d2b %s" % (v, fmt((res.get(key(v, "W", r, "inside")) or {}).get("wob_med")), fmt((res.get(key(v, "W", r, "inside")) or {}).get("d2_blur")))
            for v, r in plan))
    idr = identity_report()
    lines.append("identity (criterion 1, owner view, frozen): " + json.dumps(idr))
    lines.append("cost (ms, probe window, median of 3; cloud = VolumetricCloud + VolCloudReconstruction + VolCloudComposeOverScene):")
    for v in ("cur", "boxoff") + tuple(S.HOST):
        c = cost_.get(v, {})
        lines.append("  %-8s base cloud %s fog %s frame %s | owner cloud %s fog %s frame %s" % (
            v, fmt((c.get("base") or {}).get("cloud_ms_median")), fmt((c.get("base") or {}).get("fog_ms_median")),
            fmt((c.get("base") or {}).get("frame_ms_median")), fmt((c.get("owner") or {}).get("cloud_ms_median")),
            fmt((c.get("owner") or {}).get("fog_ms_median")), fmt((c.get("owner") or {}).get("frame_ms_median"))))
    lines.append("look (frozen Box, host %s, sun travel yaw %s elev %s):" % (lk.get("host"), fmt((lk.get("sun") or [None])[0], "%.1f"),
                                                                              fmt((lk.get("sun") or [None, None])[1], "%.1f")))
    for vn, e in (lk.get("metrics") or {}).items():
        lines.append("  %-7s " % vn + json.dumps(e))
    lines.append("best cloud variant: %s" % best_variant())
    open(os.path.join(RES, "r39_summary.txt"), "w", encoding="utf-8").write("\n".join(lines) + "\n")
    print("\n".join(lines), flush=True)
    sheets(res, lk)


TABLE_ROWS = [("boxoff", "Box off (weight 0), floor"), ("cur", "froxels, live anim"), ("animoff", "froxels, anim off"),
              ("p1", "cloud p1, live anim"), ("p1_animoff", "cloud p1, anim off"), ("p1_d1", "cloud d1 (1.3 m), live"),
              ("p1_m1", "cloud Mode 1, live"), ("cur_td", "froxels, anim 20 cm/frame"), ("p1_td", "cloud p1, anim 20 cm/fr"),
              ("p1_d1_td", "cloud d1, anim 20 cm/fr"), ("p1_d5_td", "cloud d5 (6.5 m), 20 cm/fr"), ("p1_m1_td", "cloud Mode 1, 20 cm/fr"),
              ("cur_w0", "froxels, wind 0, live"), ("p1_w0", "cloud p1, wind 0, live"), ("cur_w0_td", "froxels, wind 0, 20 cm/fr"),
              ("p1_w0_td", "cloud p1, wind 0, 20 cm/fr"), ("p1_d5_w0_td", "cloud d5, wind 0, 20 cm/fr")]


def table_rows(res):
    """(label, reps, static wob, static wob_cloud, W wob, W d2b, D wob, D d2b), means over the repeats present."""
    rows = []
    for v, label in TABLE_ROWS:
        vals = {}
        reps = 0
        for rep in range(3):
            got = {D: res.get(key(v, D, rep)) for D in DIRS}
            if not any(got.values()): continue
            reps += 1
            for D, k in (("0", "wob_med"), ("0", "wob_cloud_med"), ("W", "wob_med"), ("W", "d2_blur"), ("D", "wob_med"), ("D", "d2_blur")):
                x = (got.get(D) or {}).get(k)
                if isinstance(x, (int, float)): vals.setdefault((D, k), []).append(x)
        if reps:
            m = lambda D, k: (sum(vals[(D, k)]) / len(vals[(D, k)])) if vals.get((D, k)) else None
            rows.append((label, reps, m("0", "wob_med"), m("0", "wob_cloud_med"), m("W", "wob_med"), m("W", "d2_blur"),
                         m("D", "wob_med"), m("D", "d2_blur")))
    return rows


def sheets(res, lk):
    from PIL import Image, ImageDraw
    # the four views the owner asked for ('mine' is mostly a pillar: metrics only)
    views = [v for v in ("owner", "base", "against", "front") if os.path.isfile(os.path.join(LOOKDIR, "W38_%s.png" % v))]
    if not views: return
    W, H, LAB = 480, 407, 210
    cols = [("W38", "froxels (round 38)"), ("P1", "cloud host P1 (D 2 km, S 8, Mode 0)"), ("P1_g2", "P1 + Phase G2 -0.3 / Blend 0.2")]
    if any(os.path.isfile(os.path.join(LOOKDIR, "d5_P1_%s.png" % v)) for v in views):
        cols.append(("d5_P1", "cloud host D 5 km (step 6.5 m)"))
    met = lk.get("metrics") or {}
    rows_img = len(views)
    rows = table_rows(res)
    head = "tremble at 'base' (mean of repeats)       reps | static wob  cloud-only |  W wob   W d2b |  D wob   D d2b"
    table = [head] + ["%-40s %d  |  %s     %s   | %s  %s | %s  %s" % (r[0], r[1], fmt(r[2]), fmt(r[3]), fmt(r[4]), fmt(r[5]), fmt(r[6]), fmt(r[7]))
                      for r in rows]
    cost_ = _load(CP)
    for v in ("cur", "p1", "p1_d1", "p1_d5", "p1_s4", "p1_m1"):
        c = cost_.get(v, {})
        if c:
            table.append("cost %-6s base: cloud %s ms, fog %s ms | owner view: cloud %s ms, fog %s ms (probe window)" % (
                v, fmt((c.get("base") or {}).get("cloud_ms_median")), fmt((c.get("base") or {}).get("fog_ms_median")),
                fmt((c.get("owner") or {}).get("cloud_ms_median")), fmt((c.get("owner") or {}).get("fog_ms_median"))))
    sheet = Image.new("RGB", (LAB + W * len(cols), 26 + H * rows_img + 18 * (len(table) + 2)), (18, 18, 18))
    dr = ImageDraw.Draw(sheet)
    for j, (_, t) in enumerate(cols): dr.text((LAB + j * W + 6, 6), t, fill=(235, 235, 235))
    for i, vn in enumerate(views):
        e = met.get(vn, {})
        dr.text((6, 26 + i * H + 8), vn, fill=(240, 220, 90))
        info = ["ROI brightness P1/W38 x%s" % fmt(e.get("ratio_P1")), "  without lobe x%s" % (
                    fmt(e["mean_P1_nolobe"] / e["mean_W38_nolobe"]) if e.get("mean_P1_nolobe") and e.get("mean_W38_nolobe") else "-"),
                "IoU (lit masks) %s" % fmt(e.get("iou_roi")),
                "edge/core lit W38 %s P1 %s" % (fmt(e.get("rimgeo_lit_W38")), fmt(e.get("rimgeo_lit_P1"))),
                "edge/core own W38 %s P1 %s" % (fmt(e.get("rimgeo_own_W38")), fmt(e.get("rimgeo_own_P1"))),
                "P1 without field x%s" % fmt(e.get("ratio_P1_nofield"))]
        for k, t in enumerate(info): dr.text((6, 26 + i * H + 30 + 16 * k), t, fill=(200, 200, 200))
        for j, (c, _) in enumerate(cols):
            p = os.path.join(LOOKDIR, "%s_%s.png" % (c, vn))
            if os.path.isfile(p): sheet.paste(Image.open(p).convert("RGB").resize((W, H)), (LAB + j * W, 26 + i * H))
    y = 26 + H * rows_img + 8
    for i, l in enumerate(table):
        dr.text((6, y), l[:220], fill=(250, 230, 120) if i == 0 else (220, 220, 220)); y += 18
    sheet.save(os.path.join(RES, "p1_sheet.png"))
    # every look config
    cfgs = [c for c, *_ in LOOK if c != "boxoff"]
    w2, h2 = 320, 271
    s2 = Image.new("RGB", (120 + w2 * len(cfgs), 24 + h2 * len(views)), (18, 18, 18)); d2 = ImageDraw.Draw(s2)
    for j, c in enumerate(cfgs): d2.text((120 + j * w2 + 6, 6), c, fill=(235, 235, 235))
    for i, vn in enumerate(views):
        d2.text((6, 24 + i * h2 + 8), vn, fill=(240, 220, 90))
        for j, c in enumerate(cfgs):
            p = os.path.join(LOOKDIR, "%s_%s.png" % (c, vn))
            if os.path.isfile(p): s2.paste(Image.open(p).convert("RGB").resize((w2, h2)), (120 + j * w2, 24 + i * h2))
    s2.save(os.path.join(RES, "look_sheet.png"))
    # inside path strips (frames 0, 16, 32, 47); the frozen-density runs show the same cloud for every variant
    strips = [(v, os.path.join(d.M, key(v, "W", r, "inside"))) for v, r in INSIDE_FRZ + INSIDE_TD]
    strips = [(v, p) for v, p in strips if os.path.isdir(p)]
    if strips:
        w3, h3 = 320, 271
        s3 = Image.new("RGB", (110 + w3 * 4, 24 + h3 * len(strips)), (18, 18, 18)); d3 = ImageDraw.Draw(s3)
        for j, f in enumerate((0, 16, 32, 47)): d3.text((110 + j * w3 + 6, 6), "frame %d" % f, fill=(235, 235, 235))
        for i, (v, p) in enumerate(strips):
            d3.text((6, 24 + i * h3 + 8), v, fill=(240, 220, 90))
            for j, f in enumerate((0, 16, 32, 47)):
                fp = os.path.join(p, "f%03d.png" % f)
                if os.path.isfile(fp): s3.paste(Image.open(fp).convert("RGB").resize((w3, h3)), (110 + j * w3, 24 + i * h3))
        s3.save(os.path.join(RES, "inside_sheet.png"))


# ------------------------------------------------------------------ main
def main(cmd):
    owner = S.snapshot()                      # measure/owner_pre39.json, written once (the restore target)
    if cmd in ("identity_pre", "identity_post", "identity_late"):
        identity(cmd.split("_")[1], owner); print(json.dumps(identity_report(), indent=1)); return
    if cmd == "summary":
        summary(); return
    begin(owner)
    try:
        if cmd == "smoke": smoke(owner)
        if cmd == "blackdiag": print(json.dumps(blackdiag(owner), indent=1), flush=True)
        if cmd == "look_d5": look(owner, host="p1_d5", cfgs=LOOK_D5, prefix="d5_", views=("owner", "base", "against"))
        if cmd in ("series", "all"): series(owner)
        if cmd in ("series_td", "all"): series(owner, td=True)
        if cmd in ("inside_frz", "all"):
            for variant, rep in INSIDE_FRZ:
                run(variant, rep, ("W",), owner, cam="inside")
        if cmd in ("series_w0", "all"):
            for variant, reps in PLAN_W0:
                for rep in range(reps):
                    run(variant, rep, DIRS, owner)
        if cmd in ("extra", "all"):
            for variant, reps in PLAN_EXTRA:
                for rep in range(reps):
                    run(variant, rep, DIRS, owner)
        if cmd in ("look", "all"): look(owner)
        if cmd in ("cost", "all"): cost(owner)
        if cmd in ("best", "all"): print("BEST", best(owner), best(owner, td=True), flush=True)
    finally:
        end(owner)
    if cmd in ("series", "series_td", "inside_frz", "series_w0", "extra", "look", "cost", "best", "all"):
        summary()


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "smoke")
