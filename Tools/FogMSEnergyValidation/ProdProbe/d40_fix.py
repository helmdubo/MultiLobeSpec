# -*- coding: utf-8 -*-
"""Round 40 (W40) driver. Defect 1: M_FogMS_Density's FogMS_ForwardLobe.FieldA read the transport field's RGB (index 0) instead
of its alpha (index 4) since the W37 patch, so the lobe's sun share was S = saturate(2 J_ms.R - 1) instead of T_sun * k and MS
Occlusion did nothing. Look captures before ('old') and after ('fixed') the matedit_density.py repair. Defect 2: the froxel Box
shows no native single scattering (round 39 blackdiag: albedo 0 == injection mode 0 == 106.8 at the owner view): diagnostic
series, one candidate at a time.

Frames: measure/diag40 (junction to D:/FogMS_ProbeFrames/diag40); results: results/diag40. Every subcommand starts from and
ends with the owner snapshot measure/owner_pre40.json (written once, before any change), never saves the map and prints the
restore diff ('RESTORE box diff {}'). During captures: background throttle off, sky Volumetric Cloud hidden and
r.SkyLight.RealTimeReflectionCapture.TimeSlice 0 (a stable background, round-39 methodology), Box animation frozen at
t = 100 s (manual animation time; the live phase is unchanged afterwards: AnimationTimeOffset is not touched).
Views: 'mine' = the owner's camera at snapshot time, 'against' = far side of the cloud looking toward the live sun (~10 deg off
axis), 'front' = sun behind the camera (d35_look / d37_lobe / d39_cloud geometry), 'owner39' = the round-39 blackdiag camera.

Subcommands:
  snapshot [--force]   write measure/owner_pre40.json (once)
  look old|fixed       Box-off + LOOK_<tag> configs per view (Box properties, the C++ Box writes the MID), 6 frames each, mean of
                       the last two -> results/diag40/look/<tag>_<cfg>_<view>.png; refuses if the lobe wiring is not <tag>
  matedit              runs matedit_density.py in the editor (W40 field-wiring repair; saves the material only if it changed)
  native <tag>         defect-2 series (NATIVE configs, MID written directly, Box frozen) -> results/diag40/native_<tag>.json
  overlayoff <tag>     r.FogMS.Enable 0 + FogMS.Apply (stock engine fog shaders), native-only Box (field cleared) at albedo 1 / 0,
                       then r.FogMS.Enable back + FogMS.Apply -> results/diag40/overlay_<tag>.json
  sheet                contact sheet results/diag40/w40_lobe_sheet.png (offline)
  restore              restore the snapshot
Resumable: results are written atomically per capture; present keys are skipped. Usage: python d40_fix.py <subcommand> [...]"""
import os, sys, json, time, math
import numpy as np
import diag34lib as d
import diag35lib as L
import d4_density as D4
import cloudproto_session as S
import d39_cloud as C

HERE = d.HERE
d.M = os.path.join(HERE, "measure", "diag40")      # after the imports (diag35lib / d39_cloud set their own)
os.makedirs(d.M, exist_ok=True)
RES = os.path.join(HERE, "results", "diag40"); LOOKDIR = os.path.join(RES, "look")
os.makedirs(LOOKDIR, exist_ok=True)
OWNER = os.path.join(HERE, "measure", "owner_pre40.json")
LP = os.path.join(RES, "look.json")
FROZEN_T = 100.0
NFRAMES = 6
OWNER39 = ((-6885.844530665757, -6380.660411588274, 1930.7587284347403), (37.80000156164169, 48.738534569740295, 0.0))
# Box properties outside diag35lib.BOXPROPS that this round may touch (read before any change, restored by S.restore).
BOX_EXTRA = ["wind_speed", "authored_sun_shadow", "cast_sun_shadow", "filtered_sun_shadow", "filter_sun_inside_volume"]
# Other objects this round may touch (restored explicitly).
SUN_PROPS = ["volumetric_scattering_intensity"]
SKY_PROPS = ["volumetric_scattering_intensity"]

# Defect-1 look configs: Box property overrides on top of the owner's look (MS Contribution s, MS Occlusion b).
LOOK = {"old": [("owner", {}), ("c0", {"ms_contribution": 0.0}), ("c08_o02", {"ms_contribution": 0.8, "ms_occlusion": 0.2}),
                ("c08_o10", {"ms_contribution": 0.8, "ms_occlusion": 1.0})],
        "fixed": [("owner", {}), ("c0", {"ms_contribution": 0.0})] + [
            ("c%02d_o%02d" % (int(round(s * 10)), int(round(b * 10))), {"ms_contribution": s, "ms_occlusion": b})
            for s in (0.4, 0.8) for b in (0.2, 0.5, 1.0)]}


def _atomic_dump(obj, path):
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        json.dump(obj, f, indent=1); f.flush(); os.fsync(f.fileno())
    os.replace(tmp, path)


def _load(path):
    return json.load(open(path)) if os.path.isfile(path) else {}


def pyout(code):
    return d.py(code)


# ------------------------------------------------------------------ snapshot / restore
FINDX = ("import unreal, json\n_A=unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()\n"
         "A={a.get_actor_label():a for a in _A}\nbox=A['FogMS - Live Box']\n"
         "sun=A['DirectionalLight'].light_component; skyl=A['SkyLight'].light_component\n")


def read_extra():
    out = pyout(FINDX + "print('X40 '+json.dumps({'box_extra': {p: box.get_editor_property(p) for p in %r},"
                " 'sun': {p: sun.get_editor_property(p) for p in %r}, 'skylight': {p: skyl.get_editor_property(p) for p in %r},"
                " 'animation_time_offset': box.get_editor_property('animation_time_offset')}))" % (BOX_EXTRA, SUN_PROPS, SKY_PROPS))
    return S.last_json(out, "X40")


def snapshot(force=False):
    """measure/owner_pre40.json: diag35lib state + P1 cvars + extras (sky visibility, Box MID albedo, time dilation, actors) + the
    Box/sun/skylight properties of this round. Written once (a cut run restores to it)."""
    if os.path.isfile(OWNER) and not force:
        return json.load(open(OWNER))
    s = S.snapshot(path=OWNER + ".base.tmp", force=True)
    os.remove(OWNER + ".base.tmp")
    x = read_extra()
    s["extra"]["box_extra"] = x["box_extra"]
    s["x40"] = {"sun": x["sun"], "skylight": x["skylight"], "animation_time_offset": x["animation_time_offset"],
                "note": "round 40 snapshot, read before any change; camera = the owner's ('mine')"}
    _atomic_dump(s, OWNER)
    return s


def restore(owner):
    x = owner.get("x40", {})
    code = FINDX
    for p, v in x.get("sun", {}).items():
        code += "if abs(sun.get_editor_property(%r) - %r) > 1e-6: sun.set_editor_property(%r, %r)\n" % (p, v, p, v)
    for p, v in x.get("skylight", {}).items():
        code += "if abs(skyl.get_editor_property(%r) - %r) > 1e-6: skyl.set_editor_property(%r, %r)\n" % (p, v, p, v)
    code += "print('X40 restored')"
    pyout(code)
    d.cmd("FogMS.Debug 0")
    r = S.restore(owner)        # Box (BOXPROPS + box_extra), Height Fog, cvars, sky clouds, Box MID, time dilation, camera, throttle
    now = read_extra()
    xdiff = {k: (v, now[k]) for k in ("sun", "skylight") if x.get(k) is not None and x.get(k) != now[k]}
    ato = abs(now["animation_time_offset"] - x.get("animation_time_offset", now["animation_time_offset"]))
    print("RESTORE x40 diff", xdiff, "animation_time_offset drift %.6f" % ato, flush=True)
    r["x40"] = xdiff; r["animation_time_offset_drift"] = ato
    return r


def begin(owner):
    if C.editor_minimized():
        raise SystemExit("The Unreal Editor window is minimized: nothing would render. Nothing changed; retry when it is restored.")
    C.flood_check()
    L.set_throttle(False)
    d.cmd("r.SkyLight.RealTimeReflectionCapture.TimeSlice 0")
    pyout(S.FIND + "[a.set_is_temporarily_hidden_in_editor(True) for a in sky]\nprint('sky hidden')")


def freeze(owner, on):
    if on:
        d.setbox("b.set_editor_property('manual_animation_time', %r)\nb.set_editor_property('use_manual_animation_time', True)" % FROZEN_T)
    else:
        d.setbox("b.set_editor_property('use_manual_animation_time', %s)\nb.set_editor_property('manual_animation_time', %r)"
                 % (owner["box"]["use_manual_animation_time"], owner["box"]["manual_animation_time"]))


def views(owner):
    VIEWS, travel, elev = C.sun_views()
    return {"mine": (tuple(owner["camera"][0]), tuple(owner["camera"][1])), "against": VIEWS["against"], "front": VIEWS["front"],
            "owner39": OWNER39}, travel, elev


def frame(name):
    F = d.load(name).astype(np.float32)
    return F, F[-2:].mean(axis=0)


def save_png(img, path):
    from PIL import Image
    Image.fromarray(np.clip(img, 0, 255).astype(np.uint8)).save(path)


# ------------------------------------------------------------------ defect 1: wiring state + look captures
def lobe_fielda():
    """Output name the lobe's FieldA stores (matedit_density.py report mode, definitive T3D readback)."""
    p = os.path.join(HERE, "matedit_density.py").replace("\\", "/")
    out = S.pylong("import os, runpy\nos.environ['FOGMS_MATEDIT_MODE']='report'\ntry:\n    runpy.run_path(%r, run_name='__main__')\n"
                   "finally:\n    os.environ.pop('FOGMS_MATEDIT_MODE', None)" % p)
    for line in out.splitlines():
        if "FIELD FogMS_ForwardLobe.FieldA <-" in line:
            return line.split("<-")[1].split("(")[0].strip().split(".")[-1], line.strip()
    raise RuntimeError("no lobe FieldA line in the report:\n" + out[-1500:])


def look(owner, tag):
    out_name, line = lobe_fielda()
    want = {"old": "RGB", "fixed": "A"}[tag]
    if out_name != want:
        raise SystemExit("look %s needs FogMS_ForwardLobe.FieldA <- %s, the material has: %s" % (tag, want, line))
    res = _load(LP)
    res.setdefault("wiring", {})[tag] = line
    VIEWS, travel, elev = views(owner)
    VIEWS = {k: v for k, v in VIEWS.items() if k in ("mine", "against", "front")}
    res.setdefault("views", {k: [list(v[0]), list(v[1])] for k, v in VIEWS.items()}); res["sun"] = [travel, elev]
    _atomic_dump(res, LP)
    begin(owner)
    freeze(owner, True); time.sleep(8.0)
    try:
        for vn, (loc, rot) in VIEWS.items():
            todo = [c for c in LOOK[tag] if "%s|%s|%s" % (tag, c[0], vn) not in res.get("cap", {})]
            need_off = "boxoff|%s" % vn not in res.get("cap", {})
            if not todo and not need_off:
                continue
            C.set_cam(loc, rot); time.sleep(4.0)
            if need_off:
                d.setbox("b.set_editor_property('enabled', False)"); time.sleep(4.0)
                nm = "look_boxoff_%s" % vn
                _, got = C.capture(nm, 4)
                F, img = frame(nm); save_png(img, os.path.join(LOOKDIR, "boxoff_%s.png" % vn))
                res = _load(LP); res.setdefault("cap", {})["boxoff|%s" % vn] = {"frames": got}; _atomic_dump(res, LP)
                d.setbox(L.box_assign(owner["box"])); freeze(owner, True); time.sleep(8.0)
            for cfg, props in todo:
                C.flood_check()
                d.setbox(L.box_assign(dict(owner["box"], use_manual_animation_time=True, manual_animation_time=FROZEN_T, **props)))
                time.sleep(5.0)
                nm = "look_%s_%s_%s" % (tag, cfg, vn)
                _, got = C.capture(nm, NFRAMES)
                if got < NFRAMES:
                    raise RuntimeError("%s: %d of %d frames (editor minimized?)" % (nm, got, NFRAMES))
                F, img = frame(nm); save_png(img, os.path.join(LOOKDIR, "%s_%s_%s.png" % (tag, cfg, vn)))
                L4 = F[-4:].mean(axis=3)
                flick = float(np.mean([np.abs(L4[i] - L4[i - 1]).mean() for i in range(1, len(L4))]))
                res = _load(LP); res.setdefault("cap", {})["%s|%s|%s" % (tag, cfg, vn)] = {
                    "frames": got, "props": props, "flicker_abs": round(flick, 3), "status": d.status()[:200]}
                _atomic_dump(res, LP)
                print("LOOK", tag, cfg, vn, got, "flicker %.3f" % flick, flush=True)
    finally:
        freeze(owner, False)
        restore(owner)
    look_metrics()


def _lum(p):
    from PIL import Image
    return np.asarray(Image.open(p).convert("RGB"), dtype=np.float64).mean(axis=2)


def look_metrics():
    """Per view: Box ROI = |old owner - Box off| > 4 (the same pixels for every config); ROI mean luminance, ratio vs the old
    owner image; occlusion response = mean |c08_o02 - c08_o10| over the ROI (levels) for old and fixed wiring; old c0 vs fixed
    c0 (lobe off: the wiring does not matter there, so this is the session-to-session floor)."""
    res = _load(LP); m = {}
    for vn in res.get("views", {}):
        P = lambda c: os.path.join(LOOKDIR, "%s_%s.png" % (c, vn))
        if not (os.path.isfile(P("boxoff")) and os.path.isfile(P("old_owner"))):
            continue
        off = _lum(P("boxoff")); ref = _lum(P("old_owner"))
        roi = np.abs(ref - off) > 4.0
        e = {"roi_frac": round(float(roi.mean()), 3)}
        for tag in ("old", "fixed"):
            for cfg, _ in LOOK[tag]:
                if os.path.isfile(P("%s_%s" % (tag, cfg))):
                    v = _lum(P("%s_%s" % (tag, cfg)))[roi].mean()
                    e["%s_%s" % (tag, cfg)] = round(float(v), 2)
                    e["%s_%s_ratio" % (tag, cfg)] = round(float(v / max(ref[roi].mean(), 1e-6)), 3)
        for tag in ("old", "fixed"):
            a, b = P("%s_c08_o02" % tag), P("%s_c08_o10" % tag)
            if os.path.isfile(a) and os.path.isfile(b):
                e["%s_occlusion_response" % tag] = round(float(np.abs(_lum(a) - _lum(b))[roi].mean()), 2)
        if os.path.isfile(P("old_c0")) and os.path.isfile(P("fixed_c0")):
            e["c0_old_vs_fixed_mae"] = round(float(np.abs(_lum(P("old_c0")) - _lum(P("fixed_c0")))[roi].mean()), 2)
        m[vn] = e
    res = _load(LP); res["metrics"] = m; _atomic_dump(res, LP)
    print(json.dumps(m, indent=1), flush=True)
    return m


def sheet():
    """Rows: old wiring (owner look, lobe off, s 0.8 b 0.2 / 1.0) then fixed (owner look, lobe off, s 0.4 / 0.8 x b 0.2 / 0.5 /
    1.0); columns: views. Each tile labelled with wiring, s, b, ROI brightness vs the old owner image."""
    from PIL import Image, ImageDraw
    res = _load(LP); met = res.get("metrics", {})
    VW = [v for v in ("mine", "against", "front") if os.path.isfile(os.path.join(LOOKDIR, "old_owner_%s.png" % v))]
    rows = [("old", c) for c, _ in LOOK["old"]] + [("fixed", c) for c, _ in LOOK["fixed"]]
    W, H, LAB = 400, 344, 250
    sh = Image.new("RGB", (LAB + W * len(VW), 28 + H * len(rows)), (18, 18, 18)); dr = ImageDraw.Draw(sh)
    for j, vn in enumerate(VW):
        dr.text((LAB + j * W + 6, 8), vn + (" (owner camera)" if vn == "mine" else ""), fill=(235, 235, 235))
    owner_s = None
    try:
        o = json.load(open(OWNER))["box"]; owner_s = (o["ms_contribution"], o["ms_occlusion"])
    except Exception:
        pass
    for i, (tag, cfg) in enumerate(rows):
        y = 28 + i * H
        props = dict(LOOK[tag])[cfg]
        s = props.get("ms_contribution", owner_s[0] if owner_s else None); b = props.get("ms_occlusion", owner_s[1] if owner_s else None)
        col = (255, 150, 120) if tag == "old" else (140, 230, 140)
        dr.text((6, y + 8), "OLD WIRING (FieldA <- RGB)" if tag == "old" else "FIXED (FieldA <- A)", fill=col)
        dr.text((6, y + 26), ("owner look" if cfg == "owner" else cfg), fill=(240, 220, 90))
        if s is not None:
            dr.text((6, y + 44), "MS Contribution %.1f" % s, fill=(210, 210, 210))
            dr.text((6, y + 60), "MS Occlusion %.1f" % b, fill=(210, 210, 210))
        for j, vn in enumerate(VW):
            p = os.path.join(LOOKDIR, "%s_%s_%s.png" % (tag, cfg, vn))
            if not os.path.isfile(p):
                continue
            sh.paste(Image.open(p).convert("RGB").resize((W, H)), (LAB + j * W, y))
            r = met.get(vn, {}).get("%s_%s_ratio" % (tag, cfg))
            if r:
                ImageDraw.Draw(sh).text((LAB + j * W + 6, y + H - 16), "ROI x%.3f vs old owner" % r, fill=(255, 255, 0))
    y = 28 + H * len(rows) - 4
    sh.save(os.path.join(RES, "w40_lobe_sheet.png"))
    print("SHEET", os.path.join(RES, "w40_lobe_sheet.png"), flush=True)


# ------------------------------------------------------------------ main
def main(argv):
    cmd = argv[0] if argv else "snapshot"
    if cmd == "snapshot":
        s = snapshot(force="--force" in argv)
        print(json.dumps({k: s[k] for k in ("camera", "throttle", "status", "x40")}, indent=1)); return
    if cmd == "sheet":
        look_metrics(); sheet(); return
    owner = snapshot()
    if cmd == "restore":
        restore(owner); return
    if cmd == "look":
        look(owner, argv[1]); return
    if cmd == "matedit":
        print(S.run_file("matedit_density.py")); return
    raise SystemExit("unknown subcommand %s" % cmd)


if __name__ == "__main__":
    main(sys.argv[1:])
