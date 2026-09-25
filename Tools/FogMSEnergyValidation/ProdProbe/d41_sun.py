# -*- coding: utf-8 -*-
"""Round 41 (W41 Sun Detail Shadow) driver. The Box's sun-space map (C++ FogMS_SunDetail.usf, material node FogMS_SunDetail v1)
redistributes the hybrid's per-cell sun share inside each 32^3 cell: S_hi = min(1, S_cell * T_hi / Tbar). This script verifies it
in the owner's scene with the owner's Box state.

Frames: measure/diag41 (junction to D:/FogMS_ProbeFrames/diag41); results: results/diag41 (JSON written atomically, present keys
skipped: resumable after a power cut). Every run starts and ends at the owner snapshot measure/owner_pre41.json (written once, in the
session the owner left, before any change) plus the W41 Box properties read in the round-41 session before any change (x41); the
map is never saved; the restore prints its diff. During captures: background throttle off, the sky Volumetric Cloud hidden,
r.SkyLight.RealTimeReflectionCapture.TimeSlice 0, and the LIT view mode forced with ShowFlag.OverrideDiffuseAndSpecular 0 (the
owner's FourPanes perspective viewport is saved in Detail Lighting, which replaces every albedo by 0.3); 2 (no override) after.
Views: 'mine' = the owner camera of measure/owner_pre40.json, 'against' = far side of the cloud looking toward the live sun (~10 deg
off axis), 'front' = sun behind the camera (d39_cloud.sun_views), 'base' = 40 m under the cloud base (d35_dolly CAMS).

Subcommands:
  snapshot [--force]  measure/owner_pre41.json (d40 schema + DensityMotionReference, sun rotation)          [any session]
  w40ref              W40 reference frames (Box frozen t = 100 s, Lit): boxoff / w40 a / w40 b per view       [the W40 session]
  x41                 record the round-41 Box properties before any change (sun_detail_shadow / strength)     [round 41]
  matedit             matedit_density.py in the editor (W41 FogMS_SunDetail; saves the material only if it changed)
  look                criteria 1, 2, 5a: frozen Box, per view boxoff / black / off / on / off_rep / on_rep / g0.6 off/on /
                      strength 0.5 -> results/diag41/look/<cfg>_<view>.png, look.json (ROI means, ratios, rim/core, W40 identity)
  stability           criterion 3: live animation, world time dilation 0.25 during the dumps, static camera, 48 frames:
                      boxoff, off x2, on x2 at 'base' and 'mine' -> stability.json (wob, d2b, cloud-only wob)
  cost                criterion 4: ProfileGPU x3 with the map on and off (frozen, 'mine') -> cost.json
  night               criterion 5b: sun at -3 and -15 deg elevation: off / on / off_rep at 'mine' and 'front' -> night.json
  metrics | sheet     offline: look metrics; contact sheet results/diag41/w41_sheet.png
  restore             restore the snapshot
Environment: FOGMS_LOG = the running editor's log (LogPython flood check). Usage: python d41_sun.py <subcommand>"""
import os, sys, json, time, math
import numpy as np
import diag34lib as d
import diag35lib as L
import d35_dolly as B
import cloudproto_session as S
import d39_cloud as C

HERE = d.HERE
d.M = os.path.join(HERE, "measure", "diag41")       # after the imports (diag35lib / d39_cloud set their own)
os.makedirs(d.M, exist_ok=True)
RES = os.path.join(HERE, "results", "diag41"); LOOKDIR = os.path.join(RES, "look")
os.makedirs(LOOKDIR, exist_ok=True)
OWNER = os.path.join(HERE, "measure", "owner_pre41.json")
OWNER40 = os.path.join(HERE, "measure", "owner_pre40.json")
LP, SP, CP, NP, WP = (os.path.join(RES, n) for n in ("look.json", "stability.json", "cost.json", "night.json", "w40ref.json"))
FROZEN_T = 100.0
NFRAMES = 6
TD = 0.25
NSTAB = 48
# Box properties outside diag35lib.BOXPROPS that this round may touch or that the restore must keep (read before any change).
BOX_EXTRA = ["wind_speed", "authored_sun_shadow", "cast_sun_shadow", "filtered_sun_shadow", "filter_sun_inside_volume"]
SUN_PROPS = ["sun_detail_shadow", "sun_detail_strength"]      # W41 (absent in the W40 session)
LIT = 0        # ShowFlag.OverrideDiffuseAndSpecular: 0 forces Lit albedo in every view, 2 = no override (the viewport's own mode)

FINDX = ("import unreal, json\n_A=unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()\n"
         "A={a.get_actor_label():a for a in _A}\nbox=A['FogMS - Live Box']\nsunA=A['DirectionalLight']\n"
         "sun=sunA.light_component; skyl=A['SkyLight'].light_component\n"
         "dc=box.get_editor_property('density_component'); bmid=dc.get_material(0)\n")


def _atomic_dump(obj, path):
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        json.dump(obj, f, indent=1); f.flush(); os.fsync(f.fileno())
    os.replace(tmp, path)


def _load(path):
    return json.load(open(path)) if os.path.isfile(path) else {}


def pyj(code, tag):
    return S.last_json(d.py(code), tag)


# ------------------------------------------------------------------ snapshot / restore
def read_state():
    """Everything this round may touch that diag35lib / cloudproto_session do not snapshot."""
    code = FINDX + (
        "def _p(o, n):\n"
        "    try:\n        v=o.get_editor_property(n)\n        return v if isinstance(v,(bool,int,float)) else float(v)\n"
        "    except Exception:\n        return None\n"
        "r=box.get_editor_property('density_motion_reference')\n"
        "mr={'initialized': bool(r.get_editor_property('initialized')), 'time': r.get_editor_property('time')}\n"
        "for k in ('displacement0','displacement1','displacement2','velocity0','velocity1','velocity2'):\n"
        "    v=r.get_editor_property(k); mr[k]=[v.x, v.y, v.z]\n"
        "rot=sunA.get_actor_rotation(); c=A['FogMS - Height Fog'].component.get_editor_property('volumetric_fog_albedo')\n"
        "out={'box_extra': {p: _p(box, p) for p in %r}, 'sun_props': {p: _p(box, p) for p in %r},\n"
        " 'sun': {'volumetric_scattering_intensity': sun.get_editor_property('volumetric_scattering_intensity')},\n"
        " 'skylight': {'volumetric_scattering_intensity': skyl.get_editor_property('volumetric_scattering_intensity')},\n"
        " 'sun_rotation': [rot.pitch, rot.yaw, rot.roll], 'animation_time_offset': box.get_editor_property('animation_time_offset'),\n"
        " 'motion_reference': mr, 'hf_albedo': [c.r, c.g, c.b, c.a]}\n"
        "print('X41 '+json.dumps(out))") % (BOX_EXTRA, SUN_PROPS)
    return pyj(code, "X41")


def snapshot(force=False):
    """measure/owner_pre41.json, written once (the restore target after a power cut too)."""
    if os.path.isfile(OWNER) and not force:
        return json.load(open(OWNER))
    s = S.snapshot(path=OWNER + ".base.tmp", force=True)
    os.remove(OWNER + ".base.tmp")
    x = read_state()
    s["extra"]["box_extra"] = {k: v for k, v in x["box_extra"].items() if v is not None}
    s["x41"] = {k: x[k] for k in ("sun", "skylight", "sun_rotation", "animation_time_offset", "motion_reference", "hf_albedo")}
    s["x41"]["sun_props"] = {k: v for k, v in x["sun_props"].items() if v is not None}
    s["x41"]["showflag_override_diffuse"] = 2
    s["x41"]["note"] = ("round 41 snapshot, read before any change in the session the owner left (camera = his); sun_props filled "
                        "by 'x41' in the round-41 session before any change")
    _atomic_dump(s, OWNER)
    return s


def record_x41(owner):
    """Round-41 session, before any change: the W41 Box properties as loaded (the map has none saved: class defaults)."""
    if owner["x41"].get("sun_props"):
        return owner["x41"]["sun_props"]
    x = read_state()
    owner["x41"]["sun_props"] = {k: v for k, v in x["sun_props"].items() if v is not None}
    owner["x41"]["motion_reference_r41"] = x["motion_reference"]
    _atomic_dump(owner, OWNER)
    return owner["x41"]["sun_props"]


def restore(owner):
    x = owner.get("x41", {})
    code = FINDX
    for p, v in x.get("sun", {}).items():
        code += "if abs(sun.get_editor_property(%r) - %r) > 1e-6: sun.set_editor_property(%r, %r)\n" % (p, v, p, v)
    for p, v in x.get("skylight", {}).items():
        code += "if abs(skyl.get_editor_property(%r) - %r) > 1e-6: skyl.set_editor_property(%r, %r)\n" % (p, v, p, v)
    if x.get("sun_rotation"):
        code += ("_r=sunA.get_actor_rotation()\n"
                 "if max(abs(_r.pitch-%r), abs(_r.yaw-%r), abs(_r.roll-%r)) > 1e-6: sunA.set_actor_rotation(unreal.Rotator(pitch=%r, yaw=%r, roll=%r), False)\n"
                 % tuple(x["sun_rotation"] * 2))
    if x.get("hf_albedo"):
        code += ("hf=A['FogMS - Height Fog'].component\n"
                 "hf.set_editor_property('volumetric_fog_albedo', unreal.Color(r=%d, g=%d, b=%d, a=%d))\n" % tuple(int(c) for c in x["hf_albedo"]))
    for p, v in x.get("sun_props", {}).items():
        code += "if box.get_editor_property(%r) != %r: box.set_editor_property(%r, %r)\n" % (p, v, p, v)
    if x.get("animation_time_offset") is not None:   # stability2 seeks through it; an explicit seek back to the owner's phase
        code += ("if abs(box.get_editor_property('animation_time_offset') - %r) > 1e-9: box.set_editor_property('animation_time_offset', %r)\n"
                 % (x["animation_time_offset"], x["animation_time_offset"]))
    code += "bmid.set_scalar_parameter_value('FogMS_ZeroEmission', 0.0)\nprint('X41R ok')"
    d.py(code)
    d.cmd("ShowFlag.OverrideDiffuseAndSpecular %d" % int(x.get("showflag_override_diffuse", 2)))
    r = S.restore(owner)       # Box (BOXPROPS + box_extra), Height Fog, cvars, sky clouds, Box MID, time dilation, camera, throttle
    now = read_state()
    diff = {}
    for k in ("sun", "skylight", "hf_albedo"):
        if x.get(k) is not None and x[k] != now[k]:
            diff[k] = (x[k], now[k])
    if x.get("sun_rotation") and max(abs(a - b) for a, b in zip(x["sun_rotation"], now["sun_rotation"])) > 1e-4:
        diff["sun_rotation"] = (x["sun_rotation"], now["sun_rotation"])
    for k, v in x.get("sun_props", {}).items():
        if now["sun_props"].get(k) != v:
            diff["sun_props." + k] = (v, now["sun_props"].get(k))
    ref = x.get("motion_reference_r41") or x.get("motion_reference")
    if ref and ref != now["motion_reference"]:
        diff["motion_reference"] = "changed"
    if x.get("animation_time_offset") is not None and abs(now["animation_time_offset"] - x["animation_time_offset"]) > 1e-9:
        diff["animation_time_offset"] = (x["animation_time_offset"], now["animation_time_offset"])
    print("RESTORE x41 diff", diff, flush=True)
    r["x41"] = diff
    return r


def begin(lit=True):
    if C.editor_minimized():
        raise SystemExit("The Unreal Editor window is minimized: nothing would render. Nothing changed; retry when it is restored.")
    C.flood_check()
    L.set_throttle(False)
    d.cmd("r.SkyLight.RealTimeReflectionCapture.TimeSlice 0")
    d.py(S.FIND + "[a.set_is_temporarily_hidden_in_editor(True) for a in sky]\nprint('sky hidden')")
    if lit:
        d.cmd("ShowFlag.OverrideDiffuseAndSpecular %d" % LIT)


def freeze(owner, on):
    if on:
        d.setbox("b.set_editor_property('manual_animation_time', %r)\nb.set_editor_property('use_manual_animation_time', True)" % FROZEN_T)
    else:
        d.setbox("b.set_editor_property('use_manual_animation_time', %s)\nb.set_editor_property('manual_animation_time', %r)"
                 % (owner["box"]["use_manual_animation_time"], owner["box"]["manual_animation_time"]))


def views():
    VIEWS, travel, elev = C.sun_views()
    cam = json.load(open(OWNER40))["camera"]
    return {"mine": (tuple(cam[0]), tuple(cam[1])), "against": VIEWS["against"], "front": VIEWS["front"]}, travel, elev


def frame(name):
    F = d.load(name).astype(np.float32)
    return F, F[-2:].mean(axis=0)


def save_png(img, path):
    from PIL import Image
    Image.fromarray(np.clip(img, 0, 255).astype(np.uint8)).save(path)


def _lum(p):
    from PIL import Image
    return np.asarray(Image.open(p).convert("RGB"), dtype=np.float64).mean(axis=2)


# ------------------------------------------------------------------ configs
# (name, Box property overrides, Box MID overrides, Height Fog overrides). Box properties go first (the C++ Box rewrites its MID
# when its material state changes); the MID baseline (froxel weight 1, owner albedo) and the config's MID overrides follow, so a
# previous config's direct MID write never persists.
OFFP = {"sun_detail_shadow": False}
ONP = {"sun_detail_shadow": True, "sun_detail_strength": 1.0}
LOOK = [("boxoff", OFFP, {"FogMS_FroxelWeight": 0.0}, {}),
        ("black", OFFP, {"FogMS_Albedo": (0.0, 0.0, 0.0, 1.0)}, {}),
        ("off", OFFP, {}, {}), ("on", ONP, {}, {}), ("off_rep", OFFP, {}, {}), ("on_rep", ONP, {}, {}),
        ("on_s05", {"sun_detail_shadow": True, "sun_detail_strength": 0.5}, {}, {}),
        ("off_g06", OFFP, {}, {"volumetric_fog_scattering_distribution": 0.6}),
        ("on_g06", ONP, {}, {"volumetric_fog_scattering_distribution": 0.6})]
W40REF = [("boxoff", {}, {"FogMS_FroxelWeight": 0.0}, {}), ("w40", {}, {}, {}), ("w40_rep", {}, {}, {}),
          ("black", {}, {"FogMS_Albedo": (0.0, 0.0, 0.0, 1.0)}, {})]


def apply(owner, cfg, frozen=True):
    name, box, mid, hf = cfg
    props = dict(owner["box"])
    if frozen:
        props.update(use_manual_animation_time=True, manual_animation_time=FROZEN_T)
    code = L.box_assign(props)
    base_sun = owner.get("x41", {}).get("sun_props", {})
    for p, v in dict(base_sun, **box).items():
        code += "\nb.set_editor_property(%r, %r)" % (p, v)
    d.setbox(code)
    L.set_hf(dict(owner["hf"], **hf))
    m = dict({"FogMS_FroxelWeight": 1.0, "FogMS_Albedo": tuple(owner["extra"]["box_mid_vector"]["FogMS_Albedo"])}, **mid)
    code = S.FIND
    for p, v in m.items():
        if isinstance(v, tuple):
            code += "bmid.set_vector_parameter_value(%r, unreal.LinearColor(%r, %r, %r, %r))\n" % ((p,) + tuple(float(c) for c in v))
        else:
            code += "bmid.set_scalar_parameter_value(%r, %r)\n" % (p, float(v))
    code += ("names=[str(n) for n in unreal.MaterialEditingLibrary.get_scalar_parameter_names(bmid)]\n"
             "print('MID '+json.dumps({'weight': bmid.get_scalar_parameter_value('FogMS_FroxelWeight'), 'sun_detail': "
             "bmid.get_scalar_parameter_value('FogMS_SunDetail') if 'FogMS_SunDetail' in names else None, 'albedo': "
             "str(bmid.get_vector_parameter_value('FogMS_Albedo'))}))")
    return S.last_json(d.py(code), "MID")


# ------------------------------------------------------------------ series: frozen looks (w40ref, look, night)
def frozen_series(owner, cfgs, path, prefix, view_names, settle=5.0):
    res = _load(path)
    VIEWS, travel, elev = views()
    VIEWS = {k: VIEWS[k] for k in view_names}
    res.setdefault("views", {k: [list(v[0]), list(v[1])] for k, v in VIEWS.items()}); res["sun"] = [travel, elev]
    _atomic_dump(res, path)
    begin()
    freeze(owner, True); time.sleep(8.0)
    try:
        for vn, (loc, rot) in VIEWS.items():
            todo = [c for c in cfgs if "%s%s|%s" % (prefix, c[0], vn) not in res.get("cap", {})]
            if not todo:
                continue
            C.set_cam(loc, rot); time.sleep(4.0)
            for cfg in todo:
                C.flood_check()
                mid = apply(owner, cfg)
                time.sleep(settle)
                nm = "%s%s_%s" % (prefix, cfg[0], vn)
                _, got = C.capture(nm, NFRAMES)
                if got < NFRAMES:
                    raise RuntimeError("%s: %d of %d frames (editor minimized?)" % (nm, got, NFRAMES))
                F, img = frame(nm); save_png(img, os.path.join(LOOKDIR, "%s.png" % nm))
                L4 = F[-4:].mean(axis=3)
                flick = float(np.mean([np.abs(L4[i] - L4[i - 1]).mean() for i in range(1, len(L4))]))
                res = _load(path)
                res.setdefault("cap", {})["%s%s|%s" % (prefix, cfg[0], vn)] = {"frames": got, "mid": mid, "flicker_abs": round(flick, 3),
                                                                                "status": d.status()[:260]}
                _atomic_dump(res, path)
                print("CAP", nm, got, "flicker %.3f" % flick, mid, d.status()[-120:], flush=True)
    finally:
        freeze(owner, False)
        restore(owner)


# ------------------------------------------------------------------ metrics
def rim_core(lit, off, black):
    """Round-39 geometry: alpha = 1 - black / boxoff (valid where boxoff > 12), thin edge 0.1 <= a <= 0.5, core a >= 0.9;
    own = lit - boxoff * (1 - a) (the cloud's own light). Returns (lit edge/core, own edge/core, px edge, px core)."""
    valid = off > 12.0
    a = np.clip(1.0 - black / np.maximum(off, 1e-3), 0.0, 1.0); a[~valid] = 0.0
    edge = (a >= 0.1) & (a <= 0.5) & valid; core = (a >= 0.9) & valid
    if edge.sum() < 200 or core.sum() < 200:
        return None
    own = lit - off * (1.0 - a)
    return (round(float(lit[edge].mean() / max(lit[core].mean(), 1e-3)), 3), round(float(own[edge].mean() / max(own[core].mean(), 1e-3)), 3),
            int(edge.sum()), int(core.sum()))


def look_metrics():
    res = _load(LP); m = {}
    for vn in res.get("views", {}):
        P = lambda c: os.path.join(LOOKDIR, "%s_%s.png" % (c, vn))
        if not all(os.path.isfile(P(c)) for c in ("boxoff", "off", "on")):
            continue
        off_img = _lum(P("boxoff")); ref = _lum(P("off"))
        roi = np.abs(ref - off_img) > 4.0
        e = {"roi_frac": round(float(roi.mean()), 3)}
        for c, *_ in LOOK:
            if c in ("boxoff", "black") or not os.path.isfile(P(c)):
                continue
            e[c] = round(float(_lum(P(c))[roi].mean()), 2)
        for c in list(e):
            if c in ("roi_frac",) or c.endswith("_x"):
                continue
            base = "off_g06" if c.endswith("_g06") else "off"
            if base in e and c != base:
                e[c + "_x"] = round(e[c] / max(e[base], 1e-6), 4)
        if os.path.isfile(P("black")):
            black = _lum(P("black"))
            for c in ("off", "on", "off_rep", "on_rep", "on_s05", "off_g06", "on_g06"):
                if os.path.isfile(P(c)):
                    rc = rim_core(_lum(P(c)), off_img, black)
                    if rc:
                        e["rim_lit_" + c], e["rim_own_" + c], e["rim_px"] = rc[0], rc[1], [rc[2], rc[3]]
        # floors / identity: MAE over the ROI (8-bit), mean signed difference
        def cmpimg(a, b):
            A, Bm = _lum(a), _lum(b)
            return {"mae": round(float(np.abs(A - Bm)[roi].mean()), 3), "mean_diff": round(float((A - Bm)[roi].mean()), 3)}
        if os.path.isfile(P("off_rep")):
            e["floor_off"] = cmpimg(P("off"), P("off_rep"))
        if os.path.isfile(P("on_rep")):
            e["floor_on"] = cmpimg(P("on"), P("on_rep"))
        e["on_vs_off"] = cmpimg(P("on"), P("off"))
        # criterion 5a: map off (round 41) vs the W40 session's image of the same frozen cloud
        w = os.path.join(LOOKDIR, "w40_w40_%s.png" % vn); w2 = os.path.join(LOOKDIR, "w40_w40_rep_%s.png" % vn)
        if os.path.isfile(w):
            e["w40_vs_off"] = cmpimg(w, P("off"))
            e["w40_vs_off_rep"] = cmpimg(w, P("off_rep")) if os.path.isfile(P("off_rep")) else None
            if os.path.isfile(w2):
                e["w40_floor"] = cmpimg(w, w2)
            e["w40_roi_mean"] = round(float(_lum(w)[roi].mean()), 2)
        m[vn] = e
    res = _load(LP); res["metrics"] = m; _atomic_dump(res, LP)
    for vn, e in m.items():
        print(vn, json.dumps(e), flush=True)
    return m


# ------------------------------------------------------------------ stability (criterion 3)
STAB = [("boxoff", OFFP, {"FogMS_FroxelWeight": 0.0}, {}), ("off", OFFP, {}, {}), ("on", ONP, {}, {}),
        ("off_rep", OFFP, {}, {}), ("on_rep", ONP, {}, {})]
STAB_CAMS = ("base", "mine")


def stability(owner):
    res = _load(SP)
    VIEWS, _, _ = views()
    cams = {"base": B.CAMS["base"], "mine": VIEWS["mine"]}
    begin()
    try:
        for cam in STAB_CAMS:
            todo = [c for c in STAB if "%s|%s" % (c[0], cam) not in res]
            if not todo:
                continue
            C.set_cam(*cams[cam]); time.sleep(4.0)
            for cfg in todo:
                C.flood_check()
                mid = apply(owner, cfg, frozen=False)
                time.sleep(5.0)
                S.set_time_dilation(TD)
                nm = "stab_%s_%s" % (cfg[0], cam)
                try:
                    _, got = C.capture(nm, NSTAB)
                finally:
                    S.set_time_dilation(owner["extra"].get("time_dilation", 1.0))
                if got < NSTAB:
                    raise RuntimeError("%s: %d of %d frames (editor minimized?)" % (nm, got, NSTAB))
                r = B.analyse(nm, None); r.update(B.wobble(nm, None))
                try:
                    t = np.diff(np.array(json.load(open(os.path.join(d.M, nm, "ticks.json")))["t"]))
                    r["dump_dt"] = round(float(np.median(t[1:1 + NSTAB])), 4); r["anim_s_per_frame"] = round(r["dump_dt"] * TD, 4)
                except Exception as e:
                    r["dump_dt_error"] = str(e)
                r.update({"frames": got, "mid": mid, "status": d.status()[:260]})
                res = _load(SP); res["%s|%s" % (cfg[0], cam)] = r; _atomic_dump(res, SP)
                print("STAB %-18s wob %s/%s d2b %.3f d1 %.3f mean %.1f" % (nm, r.get("wob_med"), r.get("wob_p75"), r["d2_blur"], r["d1"], r["mean"]),
                      flush=True)
    finally:
        restore(owner)
    stability_metrics()


SP2 = os.path.join(RES, "stability2.json")
STAB2 = [("boxoff", OFFP, {"FogMS_FroxelWeight": 0.0}, {})] + [(c, p, {}, {}) for r in range(3) for c, p in (("off%d" % r, OFFP), ("on%d" % r, ONP))]


def _world_time():
    return float(d.py("import unreal\nw=unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()\n"
                      "print('WT', unreal.GameplayStatics.get_time_seconds(w))").split("WT")[-1].split()[0])


def stability2(owner):
    """Criterion 3, controlled: every run replays the same density sequence. Before each capture the Box's Animation Time Offset
    is set so that the density time at the capture start equals a fixed reference (an explicit seek: one native history reset,
    absorbed by the 4 s settle before the capture), with world time dilation 0.25 already on (the dump rate ~6.4 fps then advances
    the density ~0.038 s per frame, ~25 fps). Alternating off / on, 3 repeats per camera; per-run own cloud mask. The owner's
    offset is put back at the end (restore)."""
    res = _load(SP2)
    VIEWS, _, _ = views()
    cams = {"base": B.CAMS["base"], "mine": VIEWS["mine"]}
    off0 = float(owner["x41"]["animation_time_offset"])
    begin()
    try:
        S.set_time_dilation(TD)
        t_ref = res.get("t_ref")
        if t_ref is None:
            t_ref = _world_time() + off0 + 30.0          # a density time a little ahead of the live one
            res["t_ref"] = t_ref; _atomic_dump(res, SP2)
        for cam in STAB_CAMS:
            todo = [c for c in STAB2 if "%s|%s" % (c[0], cam) not in res]
            if not todo:
                continue
            C.set_cam(*cams[cam]); time.sleep(4.0)
            for cfg in todo:
                C.flood_check()
                mid = apply(owner, cfg, frozen=False)
                settle = 4.0
                offset = t_ref - (_world_time() + settle * TD)
                d.setbox("b.set_editor_property('animation_time_offset', %r)" % offset)
                time.sleep(settle)
                nm = "stab2_%s_%s" % (cfg[0], cam)
                _, got = C.capture(nm, NSTAB)
                if got < NSTAB:
                    raise RuntimeError("%s: %d of %d frames (editor minimized?)" % (nm, got, NSTAB))
                r = B.analyse(nm, None); r.update(B.wobble(nm, None))
                try:
                    t = np.diff(np.array(json.load(open(os.path.join(d.M, nm, "ticks.json")))["t"]))
                    r["dump_dt"] = round(float(np.median(t[1:1 + NSTAB])), 4); r["anim_s_per_frame"] = round(r["dump_dt"] * TD, 4)
                except Exception as e:
                    r["dump_dt_error"] = str(e)
                r.update({"frames": got, "mid": mid, "offset": offset, "status": d.status()[:260]})
                res = _load(SP2); res["%s|%s" % (cfg[0], cam)] = r; _atomic_dump(res, SP2)
                print("STAB2 %-18s wob %s/%s d2b %.3f d1 %.3f mean %.1f" % (nm, r.get("wob_med"), r.get("wob_p75"), r["d2_blur"], r["d1"], r["mean"]),
                      flush=True)
    finally:
        S.set_time_dilation(owner["extra"].get("time_dilation", 1.0))
        d.setbox("b.set_editor_property('animation_time_offset', %r)" % off0)
        restore(owner)
    stability2_metrics()


SP3 = os.path.join(RES, "stability3.json")
# (name, Box props, map cvars) at the owner camera, same replay procedure as stability2: what reduces the ON flicker?
STAB3 = [("off", OFFP, {}), ("on", ONP, {}), ("on_steps128", ONP, {"r.FogMS.SunMap.Steps": 128}),
         ("on_res512", ONP, {"r.FogMS.SunMap.Resolution": 512}), ("on_s05", {"sun_detail_shadow": True, "sun_detail_strength": 0.5}, {}),
         ("on_steps128_rep", ONP, {"r.FogMS.SunMap.Steps": 128}), ("off_rep", OFFP, {})]
MAP_CVARS = {"r.FogMS.SunMap.Steps": 64, "r.FogMS.SunMap.Resolution": 256}


def stability3(owner):
    res = _load(SP3)
    VIEWS, _, _ = views()
    off0 = float(owner["x41"]["animation_time_offset"])
    cv0 = L.getcv(list(MAP_CVARS))
    res["map_cvars_before"] = cv0; _atomic_dump(res, SP3)
    begin()
    try:
        S.set_time_dilation(TD)
        t_ref = res.get("t_ref")
        if t_ref is None:
            t_ref = _world_time() + off0 + 30.0
            res["t_ref"] = t_ref; _atomic_dump(res, SP3)
        C.set_cam(*VIEWS["mine"]); time.sleep(4.0)
        for name, props, cvars in STAB3:
            if name in res:
                continue
            C.flood_check()
            for k, v in dict(MAP_CVARS, **cvars).items():
                d.cmd("%s %d" % (k, v))
            mid = apply(owner, (name, props, {}, {}), frozen=False)
            settle = 5.0
            offset = t_ref - (_world_time() + settle * TD)
            d.setbox("b.set_editor_property('animation_time_offset', %r)" % offset)
            time.sleep(settle)
            nm = "stab3_%s" % name
            _, got = C.capture(nm, NSTAB)
            if got < NSTAB:
                raise RuntimeError("%s: %d of %d frames (editor minimized?)" % (nm, got, NSTAB))
            r = B.analyse(nm, None); r.update(B.wobble(nm, None))
            r.update({"frames": got, "mid": mid, "cvars": L.getcv(list(MAP_CVARS)), "status": d.status()[-200:]})
            res = _load(SP3); res[name] = r; _atomic_dump(res, SP3)
            print("STAB3 %-16s wob %s/%s d2b %.3f d1 %.3f mean %.1f %s" % (name, r.get("wob_med"), r.get("wob_p75"), r["d2_blur"], r["d1"], r["mean"], r["cvars"]), flush=True)
    finally:
        for k, v in cv0.items():
            d.cmd("%s %d" % (k, int(v)))
        S.set_time_dilation(owner["extra"].get("time_dilation", 1.0))
        d.setbox("b.set_editor_property('animation_time_offset', %r)" % off0)
        restore(owner)
        print("MAP CVARS restored", L.getcv(list(MAP_CVARS)), flush=True)
    # own-cloud metrics against the Box-off background of the same frame size
    import glob as _glob
    bos = {}
    for p in _glob.glob(os.path.join(d.M, "stab2_boxoff_mine*")):
        if _glob.glob(os.path.join(p, "f*.png")):
            A = d.load(os.path.basename(p)).mean(axis=3).mean(0); bos[A.shape] = A
    res = _load(SP3)
    for name, _, _ in STAB3:
        if name not in res:
            continue
        nm = "stab3_%s" % name
        Lm = d.load(nm).mean(axis=3); bo = bos.get(Lm.shape[1:])
        if bo is None:
            continue
        np.save(os.path.join(d.M, "roi_own_%s.npy" % nm), np.abs(Lm.mean(0) - bo) > 4)
        w = B.wobble(nm, "own_" + nm); a = B.analyse(nm, "own_" + nm)
        res[name].update({"own_cloud_wob_med": w.get("wob_med"), "own_cloud_wob_p75": w.get("wob_p75"), "own_cloud_d2b": a.get("c_d2_blur"),
                          "own_cloud_d1": a.get("c_d1"), "own_cloud_frac": a.get("c_frac")})
        print("%-16s own cloud %s wob %s/%s d2b %s d1 %s" % (name, a.get("c_frac"), w.get("wob_med"), w.get("wob_p75"), a.get("c_d2_blur"), a.get("c_d1")), flush=True)
    _atomic_dump(res, SP3)


def stability2_metrics():
    """Own-cloud metrics per run (|run mean - boxoff mean| > 4) and the mean frame-to-frame image difference between runs of the
    same frame index (replay check: off vs off, on vs on, off vs on)."""
    res = _load(SP2)
    import glob as _glob
    for cam in STAB_CAMS:
        # Box-off backgrounds by frame size (the owner's layout changed the viewport width once during the series).
        bos = {}
        for p in [os.path.join(d.M, "stab2_boxoff_%s" % cam)] + _glob.glob(os.path.join(d.M, "stab2_boxoff_%s_w*" % cam)):
            if os.path.isdir(p) and _glob.glob(os.path.join(p, "f*.png")):
                A = d.load(os.path.basename(p)).mean(axis=3).mean(0)
                bos[A.shape] = A
        if not bos:
            continue
        runs = [c[0] for c in STAB2[1:] if "%s|%s" % (c[0], cam) in res]
        means = {}
        for c in runs:
            nm = "stab2_%s_%s" % (c, cam)
            Lm = d.load(nm).mean(axis=3)
            res["%s|%s" % (c, cam)]["frame_size"] = [int(Lm.shape[2]), int(Lm.shape[1])]
            bo = bos.get(Lm.shape[1:])
            if bo is None:
                res["%s|%s" % (c, cam)]["own_cloud_note"] = "no Box-off capture of this frame size"
                continue
            means[c] = Lm
            own = np.abs(Lm.mean(0) - bo) > 4
            np.save(os.path.join(d.M, "roi_own_%s.npy" % nm), own)
            w = B.wobble(nm, "own_" + nm); a = B.analyse(nm, "own_" + nm)
            res["%s|%s" % (c, cam)].update({"own_cloud_frac": round(float(own.mean()), 3), "own_cloud_wob_med": w.get("wob_med"),
                                           "own_cloud_wob_p75": w.get("wob_p75"), "own_cloud_d2b": a.get("c_d2_blur"), "own_cloud_d1": a.get("c_d1")})
        if "off0" in means:
            ref = means["off0"]
            res["replay|%s" % cam] = {c: round(float(np.abs(means[c] - ref).mean()), 3) for c in means if c != "off0" and means[c].shape == ref.shape}
    _atomic_dump(res, SP2)
    for k, r in sorted(res.items()):
        if isinstance(r, dict) and "own_cloud_d2b" in r:
            print("%-10s own cloud %.3f wob %s/%s d2b %s d1 %s | all wob %s d2b %s" % (k, r["own_cloud_frac"], r["own_cloud_wob_med"],
                  r["own_cloud_wob_p75"], r["own_cloud_d2b"], r["own_cloud_d1"], r.get("wob_med"), r.get("d2_blur")), flush=True)
        elif k.startswith("replay"):
            print(k, r, flush=True)


def stability_metrics():
    """Cloud-only wobble: the same tiles restricted to the cloud of each camera (|mean off - mean boxoff| > 4 over the capture)."""
    res = _load(SP)
    for cam in STAB_CAMS:
        if "off|%s" % cam not in res or "boxoff|%s" % cam not in res:
            continue
        A = d.load("stab_off_%s" % cam).mean(axis=3).mean(0); Bm = d.load("stab_boxoff_%s" % cam).mean(axis=3).mean(0)
        np.save(os.path.join(d.M, "roi_stab_%s.npy" % cam), np.abs(A - Bm) > 4)
        for cfg in STAB:
            k = "%s|%s" % (cfg[0], cam)
            if k not in res:
                continue
            nm = "stab_%s_%s" % (cfg[0], cam)
            w = B.wobble(nm, "stab_%s" % cam)
            a = B.analyse(nm, "stab_%s" % cam)
            res[k]["cloud_wob_med"] = w.get("wob_med"); res[k]["cloud_wob_p75"] = w.get("wob_p75")
            res[k]["cloud_d2b"] = a.get("c_d2_blur"); res[k]["cloud_frac"] = a.get("c_frac")
    _atomic_dump(res, SP)
    for k, r in sorted(res.items()):
        print("%-14s wob %s/%s cloud wob %s/%s d2b %s cloud d2b %s" % (k, r.get("wob_med"), r.get("wob_p75"), r.get("cloud_wob_med"),
                                                                       r.get("cloud_wob_p75"), r.get("d2_blur"), r.get("cloud_d2b")), flush=True)


# ------------------------------------------------------------------ cost (criterion 4)
def profile(label):
    marker = "D41_PROFILE_%s_%d" % (label, int(time.time() * 1000))
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
        if "LogRHI" not in l:
            break
        block.append(l)
    frame_ms = next((float(__import__("re").search(r"Frame Time\s*:\s*([\d.]+)ms", l).group(1)) for l in block if "Frame Time" in l), None)
    rows = [(float(m.group(1)), m.group(2)) for m in (C.ROW.search(l) for l in block) if m]
    if len(rows) < 100 or not any(n == "Scene" for _, n in rows):
        return {"error": "no scene passes in the profile (%d events): viewport not rendering, editor minimized?" % len(rows)}
    tot = lambda pred: round(sum(ms for ms, n in rows if pred(n)), 4)
    return {"frame_ms": frame_ms, "events": len(rows),
            "sun_march": tot(lambda n: n.startswith("FogMS SunDetail march")),
            "sun_cell": tot(lambda n: n.startswith("FogMS SunDetail cell average")),
            "sun_total": tot(lambda n: n.startswith("FogMS SunDetail")),
            "fogms_all": tot(lambda n: n.startswith("FogMS")),
            "volumetric_fog": tot(lambda n: n.startswith("ComputeVolumetricFog") or n == "VolumetricFog"),
            "voxelize": tot(lambda n: "Voxeliz" in n),
            "rows_sun": [(ms, n) for ms, n in rows if n.startswith("FogMS SunDetail")],
            "rows_fog": [(ms, n) for ms, n in rows if "Voxeliz" in n or n.startswith("ComputeVolumetricFog")][:8]}


def cost(owner):
    res = _load(CP)
    VIEWS, _, _ = views()
    begin()
    freeze(owner, True); time.sleep(6.0)
    try:
        C.set_cam(*VIEWS["mine"]); time.sleep(4.0)
        for tag, cfg in (("on", ("on", ONP, {}, {})), ("off", ("off", OFFP, {}, {}))):
            for rep in range(3):
                k = "%s_%d" % (tag, rep)
                if k in res:
                    continue
                C.flood_check()
                if C.editor_minimized():
                    raise SystemExit("editor minimized: profiling refused")
                apply(owner, cfg); time.sleep(5.0)
                p = profile(k)
                if "error" in p:
                    raise RuntimeError("%s: %s" % (k, p["error"]))
                res = _load(CP); res[k] = p; _atomic_dump(res, CP)
                print("COST", k, {kk: p[kk] for kk in ("frame_ms", "sun_march", "sun_cell", "sun_total", "volumetric_fog", "voxelize")}, flush=True)
    finally:
        freeze(owner, False)
        restore(owner)
    summ = {}
    for tag in ("on", "off"):
        vals = [res[k] for k in res if k.startswith(tag + "_") and isinstance(res[k], dict) and "sun_total" in res[k]]
        if vals:
            summ[tag] = {kk: float(np.median([v[kk] for v in vals])) for kk in ("frame_ms", "sun_march", "sun_cell", "sun_total", "volumetric_fog", "voxelize", "fogms_all")}
    res = _load(CP); res["median"] = summ; _atomic_dump(res, CP)
    print("COST median", json.dumps(summ), flush=True)


# ------------------------------------------------------------------ night (criterion 5b)
NIGHT = [("off", OFFP, {}, {}), ("on", ONP, {}, {}), ("off_rep", OFFP, {}, {})]


def night(owner):
    rot = owner["x41"]["sun_rotation"]
    for elev in (-3.0, -15.0):
        tag = "night%02d" % int(abs(elev))
        res = _load(NP)
        if all("%s_%s|%s" % (tag, c[0], v) in res.get("cap", {}) for c in NIGHT for v in ("mine", "front")):
            continue
        # the light travels upward (pitch > 0): the sun is 'elev' below the horizon; yaw and roll unchanged
        d.py(FINDX + "sunA.set_actor_rotation(unreal.Rotator(pitch=%r, yaw=%r, roll=%r), False)\nprint('SUN', sunA.get_actor_rotation())"
             % (-elev, rot[1], rot[2]))
        time.sleep(10.0)
        try:
            frozen_series(owner, NIGHT, NP, tag + "_", ("mine", "front"), settle=6.0)
        finally:
            restore(owner)
    res = _load(NP); m = {}
    for tag in ("night03", "night15"):
        for vn in ("mine", "front"):
            P = lambda c: os.path.join(LOOKDIR, "%s_%s_%s.png" % (tag, c, vn))
            if not all(os.path.isfile(P(c)) for c in ("off", "on", "off_rep")):
                continue
            a, b, c = _lum(P("off")), _lum(P("on")), _lum(P("off_rep"))
            m["%s|%s" % (tag, vn)] = {"mean_off": round(float(a.mean()), 3), "on_vs_off_mae": round(float(np.abs(b - a).mean()), 4),
                                      "on_vs_off_max": round(float(np.abs(b - a).max()), 1), "floor_mae": round(float(np.abs(c - a).mean()), 4),
                                      "floor_max": round(float(np.abs(c - a).max()), 1),
                                      "status_on": res.get("cap", {}).get("%s_on|%s" % (tag, vn), {}).get("status", "")[-160:]}
    res["metrics"] = m; _atomic_dump(res, NP)
    print(json.dumps(m, indent=1), flush=True)


# ------------------------------------------------------------------ sheet
def sheet():
    from PIL import Image, ImageDraw
    res = _load(LP); met = res.get("metrics", {})
    VW = [v for v in ("mine", "against", "front") if os.path.isfile(os.path.join(LOOKDIR, "off_%s.png" % v))]
    rows = [("off", "Sun Detail Shadow OFF (W40 look)"), ("on", "Sun Detail Shadow ON (strength 1)"),
            ("on_s05", "ON, strength 0.5"), ("off_g06", "OFF + fog Scattering Distribution 0.6"),
            ("on_g06", "ON + fog Scattering Distribution 0.6")]
    W, H, LAB = 440, 378, 250
    sh = Image.new("RGB", (LAB + W * len(VW), 28 + H * len(rows)), (18, 18, 18)); dr = ImageDraw.Draw(sh)
    for j, vn in enumerate(VW):
        dr.text((LAB + j * W + 6, 8), {"mine": "mine (owner camera)", "against": "against the sun", "front": "sun behind"}[vn], fill=(235, 235, 235))
    for i, (c, label) in enumerate(rows):
        y = 28 + i * H
        dr.text((6, y + 8), label, fill=(240, 220, 90))
        dr.text((6, y + 26), "Lit view mode, Box frozen t = 100 s", fill=(170, 170, 170))
        for j, vn in enumerate(VW):
            p = os.path.join(LOOKDIR, "%s_%s.png" % (c, vn))
            if not os.path.isfile(p):
                continue
            sh.paste(Image.open(p).convert("RGB").resize((W, H)), (LAB + j * W, y))
            e = met.get(vn, {})
            txt = []
            if e.get(c + "_x") is not None:
                txt.append("ROI x%.3f vs %s" % (e[c + "_x"], "OFF g0.6" if c.endswith("_g06") else "OFF"))
            if e.get("rim_own_" + c) is not None:
                txt.append("rim/core %.2f" % e["rim_own_" + c])
            if txt:
                ImageDraw.Draw(sh).text((LAB + j * W + 6, y + H - 16), "  ".join(txt), fill=(255, 255, 0))
    out = os.path.join(RES, "w41_sheet.png")
    sh.save(out)
    print("SHEET", out, flush=True)
    # Zoom sheet: the cloud band of each view at full resolution (top 300 px), OFF / ON / ON - OFF (x6 around mid grey) and the
    # same with fog Scattering Distribution 0.6.
    pairs = [("off", "on", "fog phase 0 (owner)"), ("off_g06", "on_g06", "fog Scattering Distribution 0.6")]
    tiles = []
    for vn in VW:
        for a, b, lab in pairs:
            pa, pb = (os.path.join(LOOKDIR, "%s_%s.png" % (c, vn)) for c in (a, b))
            if not (os.path.isfile(pa) and os.path.isfile(pb)):
                continue
            A = np.asarray(Image.open(pa).convert("RGB"), dtype=np.float64)[:300]
            Bm = np.asarray(Image.open(pb).convert("RGB"), dtype=np.float64)[:300]
            D = np.clip(128.0 + 6.0 * (Bm - A).mean(axis=2, keepdims=True), 0, 255).repeat(3, axis=2)
            tiles.append(("%s, %s" % (vn, lab), [A, Bm, D]))
    if tiles:
        Wd = tiles[0][1][0].shape[1]
        zs = Image.new("RGB", (3 * Wd + 8, len(tiles) * 330 + 24), (18, 18, 18)); zd = ImageDraw.Draw(zs)
        for j, t in enumerate(("Sun Detail Shadow OFF", "Sun Detail Shadow ON", "ON - OFF (x6, grey = equal)")):
            zd.text((j * (Wd + 4) + 6, 6), t, fill=(235, 235, 235))
        for i, (lab, ims) in enumerate(tiles):
            y = 24 + i * 330
            zd.text((6, y + 2), lab, fill=(240, 220, 90))
            for j, im in enumerate(ims):
                zs.paste(Image.fromarray(np.clip(im, 0, 255).astype(np.uint8)), (j * (Wd + 4), y + 22))
        zout = os.path.join(RES, "w41_zoom_sheet.png")
        zs.save(zout)
        print("SHEET", zout, flush=True)


# ------------------------------------------------------------------ main
def main(argv):
    cmd = argv[0] if argv else "snapshot"
    if cmd == "snapshot":
        s = snapshot(force="--force" in argv)
        print(json.dumps({k: s[k] for k in ("camera", "throttle", "status", "x41")}, indent=1)); return
    if cmd == "metrics":
        look_metrics(); return
    if cmd == "sheet":
        look_metrics(); sheet(); return
    owner = snapshot()
    if cmd == "restore":
        restore(owner); return
    if cmd == "x41":
        print(record_x41(owner)); return
    if cmd == "w40ref":
        frozen_series(owner, W40REF, WP, "w40_", ("mine", "against", "front")); return
    if cmd == "matedit":
        print(S.run_file("matedit_density.py")); return
    if cmd == "look":
        record_x41(owner)
        frozen_series(owner, LOOK, LP, "", ("mine", "against", "front")); look_metrics(); return
    if cmd == "stability":
        record_x41(owner); stability(owner); return
    if cmd == "stability2":
        record_x41(owner); stability2(owner); return
    if cmd == "stability2_metrics":
        stability2_metrics(); return
    if cmd == "stability3":
        record_x41(owner); stability3(owner); return
    if cmd == "regress":
        # Build-to-build check of the map (e.g. round 41 vs 43, cell-pass rewrite): frozen off / on / on_rep at 'against' and
        # 'mine' under a build tag prefix; 'regress_metrics A B' compares two tags.
        tag = argv[1]
        record_x41(owner)
        frozen_series(owner, [("off", OFFP, {}, {}), ("on", ONP, {}, {}), ("on_rep", ONP, {}, {})],
                      os.path.join(RES, "regress.json"), tag + "_", ("against", "mine"))
        return
    if cmd == "regress_metrics":
        a, b = argv[1], argv[2]; out = {}
        for vn in ("against", "mine"):
            P = lambda t, c: os.path.join(LOOKDIR, "%s_%s_%s.png" % (t, c, vn))
            if not all(os.path.isfile(P(t, c)) for t in (a, b) for c in ("off", "on", "on_rep")):
                continue
            I = {(t, c): _lum(P(t, c)) for t in (a, b) for c in ("off", "on", "on_rep")}
            if I[(a, "on")].shape != I[(b, "on")].shape:
                out[vn] = "frame sizes differ"; continue
            roi = np.abs(I[(a, "on")] - I[(a, "off")]) > 2.0      # where the map changes the image
            mae = lambda x, y: round(float(np.abs(x - y)[roi].mean()), 3)
            out[vn] = {"roi_frac": round(float(roi.mean()), 3), "on_%s_vs_%s" % (a, b): mae(I[(a, "on")], I[(b, "on")]),
                       "floor_%s" % a: mae(I[(a, "on")], I[(a, "on_rep")]), "floor_%s" % b: mae(I[(b, "on")], I[(b, "on_rep")]),
                       "off_%s_vs_%s" % (a, b): mae(I[(a, "off")], I[(b, "off")]), "on_vs_off_%s" % a: mae(I[(a, "on")], I[(a, "off")])}
        res = _load(os.path.join(RES, "regress.json")); res["metrics_%s_%s" % (a, b)] = out; _atomic_dump(res, os.path.join(RES, "regress.json"))
        print(json.dumps(out, indent=1)); return
    if cmd == "stability2_boxoff":
        # A Box-off background at the current viewport size for the owner camera (static background: sky + ground).
        from PIL import Image
        import glob as _glob
        VIEWS, _, _ = views()
        begin()
        try:
            C.set_cam(*VIEWS["mine"]); time.sleep(4.0)
            apply(owner, ("boxoff", OFFP, {"FogMS_FroxelWeight": 0.0}, {}), frozen=False); time.sleep(5.0)
            _, got = C.capture("stab2_boxoff_mine_tmp", 8)
            w = Image.open(sorted(_glob.glob(os.path.join(d.M, "stab2_boxoff_mine_tmp", "f*.png")))[0]).size[0]
            import shutil
            dst = os.path.join(d.M, "stab2_boxoff_mine_w%d" % w)
            shutil.rmtree(dst, ignore_errors=True); os.replace(os.path.join(d.M, "stab2_boxoff_mine_tmp"), dst)
            print("BOXOFF", dst, got)
        finally:
            restore(owner)
        stability2_metrics(); return
    if cmd == "cost":
        record_x41(owner); cost(owner); return
    if cmd == "night":
        record_x41(owner); night(owner); return
    raise SystemExit("unknown subcommand %s" % cmd)


if __name__ == "__main__":
    main(sys.argv[1:])
