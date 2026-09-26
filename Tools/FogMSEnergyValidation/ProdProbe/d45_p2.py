# -*- coding: utf-8 -*-
"""Round 45 / P2 (FogMS_PerPixelClouds_Design.md section 4 'P2'): Box property Render Path = Cloud Host in C++ (the Box renders
through the engine Volumetric Cloud 'cloud host' with M_FogMS_Cloud / MI_FogMS_Cloud instead of volumetric-fog froxels). Lean
verification plan (owner, 2026-09-26): one install, one noise floor per session, status checks + one frame per state.

Frames: measure/diag45 (junction to D:/FogMS_ProbeFrames/diag45); results: results/diag45 (JSON written atomically, present keys
skipped: resumable after a power cut). Every run starts and ends at the owner snapshot measure/owner_pre45.json (written once, in
the session the owner left, before any change: d41 schema = cloudproto_session.snapshot + DensityMotionReference, sun rotation, W41
props, sky cloud visibility). The map is never saved; restore prints its diff. During captures: background throttle off, the sky
Volumetric Cloud of the snapshot hidden (it is hidden in the owner's session anyway), r.SkyLight.RealTimeReflectionCapture.TimeSlice 0,
LIT (ShowFlag.OverrideDiffuseAndSpecular 0; the 'dl' look configs use 1 = Detail Lighting's 0.3 albedo, the condition of the round-39
look numbers). A cloud host created by a run ('FogMS Cloud Host') is destroyed before the run returns.

Subcommands:
  snapshot [--force]  measure/owner_pre45.json                                                   [round-44 session, before any change]
  idref               criterion 1 reference: owner view, Box frozen t = 100 s, 4 frames x 2 (a, b = floor) + Box MID dump [round 44]
  id45                the same in the round-45 session (Render Path default Froxel Fog) -> identity vs idref           [round 45]
  states              criteria 2, 3, 5: owner view, frozen; froxel0 / nohost / create / back_froxel / again_cloud / layer_miss /
                      layer_ok (+ host hidden status): status, host MID, one frame each (4 dumped, mean of the last 2)
  tremble             criterion 4 (P1 criterion 4): camera 'base', live animation, world time dilation 0.25 during the dumps,
                      24 frames: boxoff x1, froxel x1, host x2 -> wob_med, cloud-only wob, d2b
  look                criterion 4 (P1 criterion 6): views owner / against / front, frozen: boxoff, froxel, host, host_black (albedo 0,
                      opacity masks), froxel_dl, host_dl (Detail Lighting albedo, round-39 condition) -> ratios, IoU, rim/core
  cost                ProfileGPU x3 at 'base' (frozen): froxel, host -> VolumetricCloud trace + reconstruct + compose, fog, FogMS
  summary             offline: results/diag45/r45_summary.txt + p2_sheet.png
  restore             restore the snapshot (also destroys a leftover host, Render Path back to Froxel Fog)
Environment: FOGMS_LOG = the running editor's log (LogPython flood check, ProfileGPU). Usage: python d45_p2.py <subcommand>"""
import os, sys, json, time, math
import numpy as np
import diag34lib as d
import diag35lib as L
import d35_dolly as B
import cloudproto_session as S
import d39_cloud as C
import d41_sun as X

HERE = d.HERE
d.M = os.path.join(HERE, "measure", "diag45")        # after the imports (the libraries set their own)
os.makedirs(d.M, exist_ok=True)
RES = os.path.join(HERE, "results", "diag45"); LOOKDIR = os.path.join(RES, "look")
os.makedirs(LOOKDIR, exist_ok=True)
OWNER = os.path.join(HERE, "measure", "owner_pre45.json")
IDP, STP, TRP, LKP, CSP = (os.path.join(RES, n) for n in ("identity.json", "states.json", "tremble.json", "look.json", "cost.json"))
FROZEN_T = 100.0
NF = 4                 # frames per still capture; the mean of the last 2 is compared
NT = 24                # frames per tremble capture
TD = 0.25              # world time dilation during tremble dumps (the dump runs the editor at ~6 fps: ~25 fps animation step)
SETTLE = 3.0
HOST_LABEL = "FogMS Cloud Host"
HOSTF = (S.FIND + "hosts=[a for a in _A if a.get_actor_label()==%r]\nhost=hosts[0] if hosts else None\n"
         "hc=host.get_component_by_class(unreal.VolumetricCloudComponent) if host else None\n") % HOST_LABEL


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
def snapshot(force=False):
    """measure/owner_pre45.json, written once (the restore target after a power cut too)."""
    if os.path.isfile(OWNER) and not force:
        return json.load(open(OWNER))
    s = S.snapshot(path=OWNER + ".base.tmp", force=True)
    os.remove(OWNER + ".base.tmp")
    x = X.read_state()
    s["extra"]["box_extra"] = {k: v for k, v in x["box_extra"].items() if v is not None}
    s["x41"] = {k: x[k] for k in ("sun", "skylight", "sun_rotation", "animation_time_offset", "motion_reference", "hf_albedo")}
    s["x41"]["sun_props"] = {k: v for k, v in x["sun_props"].items() if v is not None}
    s["x41"]["showflag_override_diffuse"] = 2
    s["x41"]["note"] = "round 45 snapshot, read before any change in the round-44 session the owner left (camera = his)"
    _atomic_dump(s, OWNER)
    return s


def destroy_host():
    return d.py(HOSTF + "n=0\nfor h in hosts:\n    unreal.get_editor_subsystem(unreal.EditorActorSubsystem).destroy_actor(h); n+=1\nprint('DESTROYED', n)")


def set_rp(cloud):
    return d.setbox("b.set_editor_property('render_path', unreal.FogMSRenderPath.%s)" % ("CLOUD_HOST" if cloud else "FROXEL_FOG"))


def restore(owner):
    """Leftover host destroyed, Render Path Froxel Fog, Box visible and albedo white (the owner's), then the d41 restore."""
    print(destroy_host(), flush=True)
    d.py(S.FIND + "box.set_is_temporarily_hidden_in_editor(False)\n"
         "if box.get_editor_property('render_path') != unreal.FogMSRenderPath.FROXEL_FOG: box.set_editor_property('render_path', unreal.FogMSRenderPath.FROXEL_FOG)\n"
         "print('RP', box.get_editor_property('render_path'))")
    alb = owner.get("extra", {}).get("box_density_albedo")
    if alb:
        d.setbox("b.set_editor_property('density_albedo', unreal.LinearColor(%r, %r, %r, %r))" % tuple(alb))
    d.cmd("ShowFlag.OverrideDiffuseAndSpecular 2")
    set_game_view(owner.get("extra", {}).get("game_view"))
    return X.restore(owner)


def set_game_view(on):
    """Level viewport Game View (G): hides editor primitives (Box bounds, axis gizmo). The round-44 owner frames had it on (no Box
    bounds drawn; inferred, the snapshot predates the property); the relaunched editor starts with it off."""
    if on is None:
        return None
    return d.py("import unreal\nles=unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)\nles.editor_set_game_view(%s)\n"
                "print('GAMEVIEW', les.editor_get_game_view())" % bool(on))


def begin(owner, lit=True):
    """Throttle off, sky clouds of the snapshot hidden (never the host), TimeSlice 0, Lit."""
    if C.editor_minimized():
        raise SystemExit("The Unreal Editor window is minimized: nothing would render. Nothing changed; retry when it is restored.")
    C.flood_check()
    L.set_throttle(False)
    d.cmd("r.SkyLight.RealTimeReflectionCapture.TimeSlice 0")
    labels = list(owner.get("extra", {}).get("sky_hidden", {}))
    d.py(S.FIND + "[A[l].set_is_temporarily_hidden_in_editor(True) for l in %r if l in A]\nprint('sky hidden')" % labels)
    if lit:
        d.cmd("ShowFlag.OverrideDiffuseAndSpecular 0")
    set_game_view(True)       # captures without editor primitives, as the round-44 owner frames
    # P1 host step (2 km / 768 = 2.6 m). The plugin sets these itself at game-setting priority while a Box uses a host, but a
    # console value (this harness's restore pushes the snapshot's cloud cvars by console) outranks it for the rest of the session.
    d.cmd("r.VolumetricCloud.DistanceToSampleMaxCount 2")
    d.cmd("r.VolumetricCloud.SampleMinCount 8")


def freeze(owner, on):
    X.freeze(owner, on)


def box_albedo():
    return pyj(S.FIND + "c=box.get_editor_property('density_albedo')\nprint('ALB '+json.dumps([c.r, c.g, c.b, c.a]))", "ALB")


def host_state():
    code = HOSTF + (
        "out={'exists': bool(host), 'status': str(box.get_editor_property('spatial_status'))}\n"
        "if host:\n"
        "    m=hc.get_editor_property('material')\n"
        "    out.update({'label': host.get_actor_label(), 'hidden': bool(host.is_temporarily_hidden_in_editor()),\n"
        "      'material': m.get_name() if m else None, 'material_class': m.get_class().get_name() if m else None,\n"
        "      'parent': (m.get_editor_property('parent').get_name() if isinstance(m, unreal.MaterialInstance) and m.get_editor_property('parent') else None),\n"
        "      'layer_km': [hc.get_editor_property('layer_bottom_altitude'), hc.get_editor_property('layer_height')],\n"
        "      'trace_km': hc.get_editor_property('tracing_max_distance'), 'trace_mode': str(hc.get_editor_property('tracing_max_distance_mode')),\n"
        "      'view_scale': hc.get_editor_property('view_sample_count_scale'), 'shadow_scale': hc.get_editor_property('shadow_view_sample_count_scale'),\n"
        "      'shadow_km': hc.get_editor_property('shadow_tracing_distance'), 'sky_capture': bool(hc.get_editor_property('visible_in_real_time_sky_captures'))})\n"
        "    if isinstance(m, unreal.MaterialInstanceDynamic):\n"
        "        out['mid']={p: m.get_scalar_parameter_value(p) for p in ('FogMS_Density','FogMS_Threshold','FogMS_Softness','FogMS_InjectionMode',"
        "'FogMS_ForwardG','FogMS_ForwardStrength','FogMS_ErosionStrength','FogMS_HeightProfile','FogMS_CloudFieldGain','FogMS_CloudPhaseG2')}\n"
        "        out['mid_vec']={p: (lambda v: [v.r, v.g, v.b, v.a])(m.get_vector_parameter_value(p)) for p in ('FogMS_WorldPhase0','FogMS_Albedo',"
        "'FogMS_CloudBoxCenter','FogMS_CloudWorldToLocal0','FogMS_CloudWorldToLocal2')}\n"
        "        t=m.get_texture_parameter_value('FogMS_TransportField'); out['field']=t.get_name() if t else None\n"
        "        t=m.get_texture_parameter_value('FogMS_Noise'); out['noise']=t.get_name() if t else None\n"
        "out['box']={p: bmid.get_scalar_parameter_value(p) for p in ('FogMS_FroxelWeight','FogMS_InjectionMode','FogMS_Density','FogMS_Threshold','FogMS_Softness')}\n"
        "out['box_vec']={p: (lambda v: [v.r, v.g, v.b, v.a])(bmid.get_vector_parameter_value(p)) for p in ('FogMS_WorldPhase0',)}\n"
        "out['cvars']={n: unreal.SystemLibrary.get_console_variable_float_value(n) for n in ('r.VolumetricCloud.DistanceToSampleMaxCount','r.VolumetricCloud.SampleMinCount')}\n"
        "out['render_path']=str(box.get_editor_property('render_path'))\n"
        "print('HOST '+json.dumps(out))")
    return pyj(code, "HOST")


def create_host():
    out = d.py(S.FIND + "box.create_cloud_host()\nprint('CREATE called')")
    return out


def set_host_layer(bottom_km=None, hidden=None):
    code = HOSTF
    if bottom_km is not None:
        code += "hc.set_editor_property('layer_bottom_altitude', %r)\n" % float(bottom_km)
    if hidden is not None:
        code += "host.set_is_temporarily_hidden_in_editor(%s)\n" % bool(hidden)
    code += "print('LAYER', hc.get_editor_property('layer_bottom_altitude'), host.is_temporarily_hidden_in_editor())"
    return d.py(code)


def still(name, n=NF):
    """n real frames of the current state into measure/diag45/<name>; the mean of the last 2 saved as mean.npy and look/<name>.png."""
    _, got = C.capture(name, n)
    if got < n:
        raise RuntimeError("%s: %d of %d frames (editor minimized?)" % (name, got, n))
    F = d.load(name).astype(np.float32)
    img = F[-2:].mean(axis=0)
    np.save(os.path.join(d.M, name, "mean.npy"), img)
    X.save_png(img, os.path.join(LOOKDIR, "%s.png" % name))
    L4 = F.mean(axis=3)
    flick = float(np.mean([np.abs(L4[i] - L4[i - 1]).mean() for i in range(1, len(L4))]))
    return img, round(flick, 3)


# ------------------------------------------------------------------ identity (criterion 1)
MID_DUMP = ("ml=unreal.MaterialEditingLibrary\n"
            "sc={str(n): bmid.get_scalar_parameter_value(n) for n in ml.get_scalar_parameter_names(bmid)}\n"
            "vc={str(n): (lambda v: [v.r, v.g, v.b, v.a])(bmid.get_vector_parameter_value(n)) for n in ml.get_vector_parameter_names(bmid)}\n"
            "tx={}\n"
            "for n in ml.get_texture_parameter_names(bmid):\n"
            "    t=bmid.get_texture_parameter_value(n); tx[str(n)]=t.get_class().get_name() if t else None\n"
            "print('MIDDUMP '+json.dumps({'scalars': sc, 'vectors': vc, 'textures': tx, 'parent': bmid.get_editor_property('parent').get_name()}))")


def mid_dump():
    return pyj(S.FIND + MID_DUMP, "MIDDUMP")


def identity(tag, owner):
    """Owner view, Box frozen at t = 100 s, owner properties otherwise; captures id_<tag>_a / _b (the b repeat is the floor)."""
    res = _load(IDP)
    names = ["id_%s_a" % tag, "id_%s_b" % tag]
    if all(n in res for n in names):
        return res
    begin(owner)
    try:
        freeze(owner, True)
        C.set_cam(*owner["camera"]); time.sleep(5.0)
        for nm in names:
            if nm in res:
                continue
            C.flood_check()
            still(nm)
            res = _load(IDP)
            res[nm] = {"frames": NF, "status": d.status()[:600], "mid": mid_dump()}
            _atomic_dump(res, IDP)
            print("IDENTITY", nm, res[nm]["status"][-200:], flush=True)
            time.sleep(2.0)
    finally:
        freeze(owner, False)
        restore(owner)
    return _load(IDP)


def _mean(nm):
    p = os.path.join(d.M, nm, "mean.npy")
    return np.load(p).astype(np.float64) if os.path.isfile(p) else None


def cmp_frames(a, b, roi=None):
    A, Bm = (_mean(a) if isinstance(a, str) else a), (_mean(b) if isinstance(b, str) else b)
    if A is None or Bm is None:
        return None
    D = A - Bm
    if roi is not None:
        D = D[roi]
    diff = np.abs(D)
    mse = float((diff ** 2).mean())
    return {"mae": round(float(diff.mean()), 4), "p99": round(float(np.percentile(diff, 99)), 2),
            "psnr": round(10 * math.log10(255.0 ** 2 / max(mse, 1e-12)), 2), "mean_diff": round(float(D.mean()), 4)}


def identity_report():
    res = _load(IDP)
    out = {}
    for a, b in (("id_44_a", "id_44_b"), ("id_45_a", "id_45_b"), ("id_44_a", "id_45_a"), ("id_44_b", "id_45_b"), ("id_44_b", "id_45_a")):
        out["%s~%s" % (a, b)] = cmp_frames(a, b)
    m44, m45 = (res.get("id_44_a") or {}).get("mid"), (res.get("id_45_a") or {}).get("mid")
    if m44 and m45:
        diffs = {}
        for kind in ("scalars", "vectors", "textures"):
            for k in sorted(set(m44[kind]) | set(m45[kind])):
                a, b = m44[kind].get(k), m45[kind].get(k)
                same = a == b if kind == "textures" or a is None or b is None else (
                    abs(a - b) <= 1e-6 if kind == "scalars" else max(abs(x - y) for x, y in zip(a, b)) <= 1e-6)
                if not same:
                    diffs["%s.%s" % (kind, k)] = [a, b]
        out["mid_diff"] = diffs
        out["status_44"] = res["id_44_a"]["status"]; out["status_45"] = res["id_45_a"]["status"]
    res["report"] = out; _atomic_dump(res, IDP)
    return out


# ------------------------------------------------------------------ states (criteria 2, 3, 5)
def states(owner):
    """The whole sequence every time (it builds on its own state): results/diag45/states.json."""
    res = {"started": time.strftime("%Y-%m-%d %H:%M:%S")}
    begin(owner)
    try:
        destroy_host(); set_rp(False)
        freeze(owner, True)
        C.set_cam(*owner["camera"]); time.sleep(5.0)
        base_layer = None
        steps = [("froxel0", lambda: set_rp(False)), ("nohost", lambda: set_rp(True)), ("create", create_host),
                 ("back_froxel", lambda: set_rp(False)), ("again_cloud", lambda: set_rp(True)),
                 ("layer_miss", "layer_miss"), ("layer_ok", "layer_ok")]
        for name, action in steps:
            C.flood_check()
            if action == "layer_miss":
                base_layer = host_state()["layer_km"][0]
                print(set_host_layer(bottom_km=base_layer + 1.0), flush=True)      # layer 1 km above: does not cover the Box
            elif action == "layer_ok":
                print(set_host_layer(bottom_km=base_layer), flush=True)
            else:
                print(name, action(), flush=True)
            time.sleep(SETTLE)
            _, flick = still("st_%s" % name)
            hs = host_state()
            res[name] = {"host": hs, "flicker": flick}
            _atomic_dump(res, STP)
            print("STATE %-12s weight %s | host %s | %s" % (name, hs["box"]["FogMS_FroxelWeight"], hs.get("mid", {}).get("FogMS_Density") if hs.get("mid") else None,
                                                           hs["status"][-230:]), flush=True)
        # host hidden: the Box falls back (status only)
        print(set_host_layer(hidden=True), flush=True); time.sleep(SETTLE)
        res["host_hidden"] = {"host": host_state()}
        print(set_host_layer(hidden=False), flush=True); time.sleep(SETTLE)
        res["host_unhidden"] = {"host": host_state()}
        # log lines of this run (host binding, emptying, step settings)
        res["log"] = log_lines(("FogMS cloud host", "FogMS.CloudHost", "Create Cloud Host", "created cloud host", "cloud host '"))
        _atomic_dump(res, STP)
        print("HIDDEN", res["host_hidden"]["host"]["status"][-160:], "| UNHIDDEN", res["host_unhidden"]["host"]["status"][-160:], flush=True)
    finally:
        freeze(owner, False)
        restore(owner)
    return states_report()


def log_lines(keys, tail=4000):
    try:
        lines = open(S.LOG, "rb").read().decode("utf-8", "replace").splitlines()[-tail:]
    except OSError:
        return []
    return [l[:400] for l in lines if any(k in l for k in keys)][-40:]


def states_report():
    res = _load(STP)
    floor = cmp_frames("id_45_a", "id_45_b")
    out = {"floor_id45": floor}
    for a, b in (("st_nohost", "st_froxel0"), ("st_back_froxel", "st_froxel0"), ("st_layer_miss", "st_froxel0"),
                 ("st_again_cloud", "st_create"), ("st_layer_ok", "st_create"), ("st_create", "st_froxel0"), ("st_froxel0", "id_45_a")):
        out["%s~%s" % (a, b)] = cmp_frames(a, b)
    res["report"] = out; _atomic_dump(res, STP)
    return out


# ------------------------------------------------------------------ tremble (criterion 4 / P1 criterion 4)
def tremble(owner):
    res = _load(TRP)
    begin(owner)
    try:
        destroy_host(); set_rp(False)
        C.set_cam(*B.CAMS["base"]); time.sleep(4.0)
        runs = [("boxoff", "boxoff"), ("froxel", False), ("host_a", True), ("host_b", True)]
        for name, mode in runs:
            if name in res:
                continue
            C.flood_check()
            if mode == "boxoff":
                d.py(S.FIND + "box.set_is_temporarily_hidden_in_editor(True)\nprint('box hidden')")
            else:
                d.py(S.FIND + "box.set_is_temporarily_hidden_in_editor(False)\nprint('box shown')")
                if mode:
                    if not host_state()["exists"]:
                        print(create_host(), flush=True)
                    set_rp(True)
                else:
                    set_rp(False)
            time.sleep(SETTLE + 1.0)
            S.set_time_dilation(TD)
            nm = "tr_%s" % name
            try:
                _, got = C.capture(nm, NT)
            finally:
                S.set_time_dilation(owner["extra"].get("time_dilation", 1.0))
            if got < NT:
                raise RuntimeError("%s: %d of %d frames (editor minimized?)" % (nm, got, NT))
            r = B.analyse(nm, None); r.update(B.wobble(nm, None))
            try:
                t = np.diff(np.array(json.load(open(os.path.join(d.M, nm, "ticks.json")))["t"]))
                r["dump_dt"] = round(float(np.median(t[1:1 + NT])), 4); r["anim_s_per_frame"] = round(r["dump_dt"] * TD, 4)
            except Exception as e:
                r["dump_dt_error"] = str(e)
            r["status"] = d.status()[-300:]
            res = _load(TRP); res[name] = r; _atomic_dump(res, TRP)
            print("TREMBLE %-8s wob %s/%s d2b %.3f d1 %.3f mean %.1f | %s" % (name, r.get("wob_med"), r.get("wob_p75"), r["d2_blur"], r["d1"],
                                                                         r["mean"], r["status"][-120:]), flush=True)
    finally:
        d.py(S.FIND + "box.set_is_temporarily_hidden_in_editor(False)\nprint('box shown')")
        restore(owner)
    return tremble_report()


def tremble_report():
    res = _load(TRP)
    if "boxoff" not in res:
        return res
    off = d.load("tr_boxoff").mean(axis=3).mean(0).astype(np.float32)
    for name in ("froxel", "host_a", "host_b"):
        if name in res and "wob_cloud_med" not in res[name]:
            res[name].update(C.wobble_cloud("tr_%s" % name, off))
    _atomic_dump(res, TRP)
    return res


# ------------------------------------------------------------------ look (criterion 4 / P1 criterion 6)
LOOK = [("boxoff", "boxoff"), ("froxel", "froxel"), ("froxel_dl", "froxel_dl"), ("host", "host"), ("host_rep", "host"), ("host_dl", "host_dl"),
        ("host_black", "host_black")]


def look_views(owner):
    V, travel, elev = C.sun_views()
    return {"owner": (tuple(owner["camera"][0]), tuple(owner["camera"][1])), "against": V["against"], "front": V["front"]}, travel, elev


def apply_look(mode, owner_albedo):
    dl = mode.endswith("_dl")
    d.cmd("ShowFlag.OverrideDiffuseAndSpecular %d" % (1 if dl else 0))
    hidden = mode == "boxoff"
    d.py(S.FIND + "box.set_is_temporarily_hidden_in_editor(%s)\nprint('box hidden', %s)" % (hidden, hidden))
    alb = (0.0, 0.0, 0.0, 1.0) if mode == "host_black" else tuple(owner_albedo)
    d.setbox("b.set_editor_property('density_albedo', unreal.LinearColor(%r, %r, %r, %r))" % alb)
    if mode.startswith("host"):
        d.py(HOSTF + "m=hc.get_editor_property('material')\n"
             "m.set_scalar_parameter_value('FogMS_CloudFieldGain', %r)\nprint('gain ok')" % (0.0 if mode == "host_black" else 1.0))
        set_rp(True)
    elif mode.startswith("froxel"):
        set_rp(False)


def look(owner):
    res = _load(LKP)
    VIEWS, travel, elev = look_views(owner)
    res.update({"views": {k: [list(v[0]), list(v[1])] for k, v in VIEWS.items()}, "sun": [travel, elev]}); _atomic_dump(res, LKP)
    owner_albedo = box_albedo()
    begin(owner)
    try:
        if not host_state()["exists"]:
            print(create_host(), flush=True)
        set_rp(False)
        freeze(owner, True); time.sleep(5.0)
        for vn, (loc, rot) in VIEWS.items():
            todo = [c for c in LOOK if "%s|%s" % (c[0], vn) not in res.get("cap", {})]
            if not todo:
                continue
            C.set_cam(loc, rot); time.sleep(3.0)
            for name, mode in todo:
                C.flood_check()
                apply_look(mode, owner_albedo)
                time.sleep(SETTLE)
                nm = "lk_%s_%s" % (name, vn)
                _, flick = still(nm)
                res = _load(LKP)
                res.setdefault("cap", {})["%s|%s" % (name, vn)] = {"flicker": flick, "status": d.status()[-260:]}
                _atomic_dump(res, LKP)
                print("LOOK", nm, "flicker %.3f" % flick, flush=True)
    finally:
        d.py(HOSTF + "m=hc.get_editor_property('material') if hc else None\n"
             "if m: m.set_scalar_parameter_value('FogMS_CloudFieldGain', 1.0)\nprint('gain reset')")
        d.setbox("b.set_editor_property('density_albedo', unreal.LinearColor(%r, %r, %r, %r))" % tuple(owner_albedo))
        freeze(owner, False)
        restore(owner)
    return look_metrics()


def _lum(p):
    from PIL import Image
    return np.asarray(Image.open(p).convert("RGB"), dtype=np.float64).mean(axis=2)


def look_metrics():
    res = _load(LKP)
    m = {}
    for vn in res.get("views", {}):
        P = lambda c: os.path.join(LOOKDIR, "lk_%s_%s.png" % (c, vn))
        if not all(os.path.isfile(P(c)) for c, _ in LOOK if c != "host_rep"):
            continue
        Lm = {c: _lum(P(c)) for c, _ in LOOK if os.path.isfile(P(c))}
        off = Lm["boxoff"]
        rf, rh = np.abs(Lm["froxel"] - off) > 4, np.abs(Lm["host"] - off) > 4
        roi = rf | rh
        e = {"roi_frac": round(float(roi.mean()), 3), "iou_roi": round(float((rf & rh).sum() / max(roi.sum(), 1)), 3)}
        for c in ("froxel", "host", "froxel_dl", "host_dl"):
            e["mean_" + c] = round(float(Lm[c][roi].mean()), 2)
        e["ratio_lit"] = round(e["mean_host"] / e["mean_froxel"], 3)
        e["ratio_dl"] = round(e["mean_host_dl"] / e["mean_froxel_dl"], 3)
        rc = {}
        for c in ("froxel", "host", "froxel_dl", "host_dl"):
            r = X.rim_core(Lm[c], off, Lm["host_black"])
            if r:
                rc[c] = {"lit": r[0], "own": r[1], "px": [r[2], r[3]]}
        e["rim_core"] = rc
        e["flicker"] = {c: (res.get("cap", {}).get("%s|%s" % (c, vn)) or {}).get("flicker") for c in ("froxel", "host", "host_rep", "host_dl", "froxel_dl")}
        if "host_rep" in Lm:
            d_rep = np.abs(Lm["host"] - Lm["host_rep"])
            e["host_floor_roi_mae"] = round(float(d_rep[roi].mean()), 3)
        m[vn] = e
    res["metrics"] = m; _atomic_dump(res, LKP)
    for vn, e in m.items():
        print(vn, json.dumps(e), flush=True)
    return m


# ------------------------------------------------------------------ cost
def cost(owner):
    res = _load(CSP)
    begin(owner)
    try:
        if not host_state()["exists"]:
            print(create_host(), flush=True)
        freeze(owner, True)
        C.set_cam(*B.CAMS["base"]); time.sleep(4.0)
        for tag, cloud in (("froxel", False), ("host", True)):
            set_rp(cloud); time.sleep(SETTLE + 1.0)
            for rep in range(3):
                k = "%s_%d" % (tag, rep)
                if k in res:
                    continue
                C.flood_check()
                if C.editor_minimized():
                    raise SystemExit("editor minimized: profiling refused")
                p = C.profile("d45_%s" % k)
                if "error" in p:
                    raise RuntimeError("%s: %s" % (k, p["error"]))
                res = _load(CSP); res[k] = p; _atomic_dump(res, CSP)
                print("COST %-9s cloud %.3f (trace %.3f rec %.3f comp %.3f shadow %.3f) fog %.3f fogms %.3f frame %s" % (
                    k, p["cloud_ms"], p["trace"], p["reconstruct"], p["compose"], p["cloud_shadow"], p["fog"], p["fogms"], p["frame_ms"]), flush=True)
    finally:
        freeze(owner, False)
        restore(owner)
    res = _load(CSP)
    med = {}
    for tag in ("froxel", "host"):
        runs = [res["%s_%d" % (tag, r)] for r in range(3) if "%s_%d" % (tag, r) in res]
        if runs:
            med[tag] = {k: round(float(np.median([r[k] for r in runs])), 3) for k in ("cloud_ms", "trace", "reconstruct", "compose", "fog", "fogms", "frame_ms")
                        if all(r.get(k) is not None for r in runs)}
    res["median"] = med; _atomic_dump(res, CSP)
    print(json.dumps(med, indent=1), flush=True)
    return med


# ------------------------------------------------------------------ summary + sheet (offline)
def fmt(v, f="%.3f"):
    return f % v if isinstance(v, (int, float)) else "-"


def summary():
    idr = identity_report() if os.path.isfile(IDP) else {}
    st = states_report() if os.path.isfile(STP) else {}
    tr = tremble_report() if os.path.isfile(TRP) else {}
    lk = look_metrics() if os.path.isfile(LKP) else {}
    cs = _load(CSP).get("median", {})
    lines = ["Round 45 / P2: Render Path = Cloud Host (C++). Owner scene, Lit unless 'dl'.", "identity (criterion 1): " + json.dumps(idr)[:1600],
             "states (criteria 2, 3, 5): " + json.dumps(st)]
    s = _load(STP)
    for k in ("froxel0", "nohost", "create", "back_froxel", "again_cloud", "layer_miss", "layer_ok", "host_hidden", "host_unhidden"):
        h = (s.get(k) or {}).get("host") or {}
        lines.append("  %-13s weight %s host density %s | %s" % (k, (h.get("box") or {}).get("FogMS_FroxelWeight"),
                                                               (h.get("mid") or {}).get("FogMS_Density"), (h.get("status") or "")[-260:]))
    lines.append("tremble (criterion 4, base, 24 frames, time dilation %.2f): name wob_med / wob_p75 / cloud-only wob / d2b / d1 / anim s per frame" % TD)
    for k in ("boxoff", "froxel", "host_a", "host_b"):
        r = tr.get(k) or {}
        lines.append("  %-8s %s / %s / %s / %s / %s / %s" % (k, fmt(r.get("wob_med")), fmt(r.get("wob_p75")), fmt(r.get("wob_cloud_med")),
                                                          fmt(r.get("d2_blur")), fmt(r.get("d1")), fmt(r.get("anim_s_per_frame"), "%.4f")))
    lines.append("look (criterion 4): " + json.dumps(lk))
    lines.append("cost (ms, probe window, median of 3, base view): " + json.dumps(cs))
    open(os.path.join(RES, "r45_summary.txt"), "w", encoding="utf-8").write("\n".join(lines) + "\n")
    print("\n".join(lines), flush=True)
    sheet(lk, tr, cs)


def sheet(lk, tr, cs):
    from PIL import Image, ImageDraw
    W, H, LAB = 440, 373, 230
    views = [v for v in ("owner", "against", "front") if os.path.isfile(os.path.join(LOOKDIR, "lk_froxel_%s.png" % v))]
    cols = [("froxel", "froxels (Render Path Froxel Fog)"), ("host", "cloud host (C++, Lit)"), ("host_dl", "cloud host, Detail Lighting")]
    strip = [n for n in ("froxel0", "nohost", "create", "back_froxel", "again_cloud", "layer_miss") if os.path.isfile(os.path.join(LOOKDIR, "st_%s.png" % n))]
    sw, sh = 300, 254
    s = _load(STP)
    table = ["tremble at 'base' (24 frames, anim ~%.3f s/frame): wob_med / cloud-only wob / d2b" % ((tr.get("host_a") or {}).get("anim_s_per_frame") or 0)]
    for k in ("boxoff", "froxel", "host_a", "host_b"):
        r = tr.get(k) or {}
        table.append("  %-8s %s / %s / %s" % (k, fmt(r.get("wob_med")), fmt(r.get("wob_cloud_med")), fmt(r.get("d2_blur"))))
    for tag in ("froxel", "host"):
        c = cs.get(tag) or {}
        table.append("cost %-6s cloud %s ms (trace %s, reconstruct %s, compose %s), volumetric fog %s ms, FogMS %s ms (probe window)" % (
            tag, fmt(c.get("cloud_ms")), fmt(c.get("trace")), fmt(c.get("reconstruct")), fmt(c.get("compose")), fmt(c.get("fog")), fmt(c.get("fogms"))))
    img = Image.new("RGB", (max(LAB + W * len(cols), 10 + sw * max(len(strip), 1)), 26 + H * len(views) + 30 + sh + 18 * (len(table) + 2)), (18, 18, 18))
    dr = ImageDraw.Draw(img)
    for j, (_, t) in enumerate(cols):
        dr.text((LAB + j * W + 6, 6), t, fill=(235, 235, 235))
    for i, vn in enumerate(views):
        e = lk.get(vn, {})
        y = 26 + i * H
        dr.text((6, y + 8), vn, fill=(240, 220, 90))
        rc = e.get("rim_core", {})
        info = ["ROI host/froxel Lit x%s" % fmt(e.get("ratio_lit")), "  Detail Lighting x%s" % fmt(e.get("ratio_dl")),
                "IoU (ROI masks) %s" % fmt(e.get("iou_roi")),
                "edge/core lit: froxel %s host %s" % (fmt((rc.get("froxel") or {}).get("lit")), fmt((rc.get("host") or {}).get("lit"))),
                "edge/core own: froxel %s host %s" % (fmt((rc.get("froxel") or {}).get("own")), fmt((rc.get("host") or {}).get("own"))),
                "flicker froxel %s host %s" % (fmt((e.get("flicker") or {}).get("froxel"), "%.2f"), fmt((e.get("flicker") or {}).get("host"), "%.2f"))]
        for k, t in enumerate(info):
            dr.text((6, y + 30 + 16 * k), t, fill=(200, 200, 200))
        for j, (c, _) in enumerate(cols):
            p = os.path.join(LOOKDIR, "lk_%s_%s.png" % (c, vn))
            if os.path.isfile(p):
                img.paste(Image.open(p).convert("RGB").resize((W, H)), (LAB + j * W, y))
    y = 26 + H * len(views) + 6
    dr.text((6, y), "states at the owner camera (frozen): Render Path / host transitions", fill=(250, 230, 120))
    for j, n in enumerate(strip):
        p = os.path.join(LOOKDIR, "st_%s.png" % n)
        img.paste(Image.open(p).convert("RGB").resize((sw, sh)), (10 + j * sw, y + 20))
        h = (s.get(n) or {}).get("host") or {}
        tag = "weight %s" % (h.get("box") or {}).get("FogMS_FroxelWeight")
        dr.text((14 + j * sw, y + 24), "%s (%s)" % (n, tag), fill=(255, 255, 120))
    y += 24 + sh
    for i, t in enumerate(table):
        dr.text((6, y + 18 * i), t[:230], fill=(250, 230, 120) if i == 0 else (220, 220, 220))
    img.save(os.path.join(RES, "p2_sheet.png"))
    print("SHEET", os.path.join(RES, "p2_sheet.png"), flush=True)


# ------------------------------------------------------------------ main
def main(cmd):
    if cmd == "snapshot":
        s = snapshot(force="--force" in sys.argv)
        print(json.dumps({k: s[k] for k in ("camera", "throttle", "status", "extra", "x41")}, indent=1)); return
    owner = snapshot()
    if cmd == "summary":
        summary(); return
    if "box_density_albedo" not in owner.get("extra", {}):
        owner["extra"]["box_density_albedo"] = box_albedo(); _atomic_dump(owner, OWNER)
    if cmd == "idref":
        identity("44", owner); print(json.dumps(identity_report(), indent=1)); return
    if cmd == "id45":
        identity("45", owner); print(json.dumps(identity_report(), indent=1)); return
    if cmd == "states":
        print(json.dumps(states(owner), indent=1)); return
    if cmd == "tremble":
        print(json.dumps(tremble(owner), indent=1)); return
    if cmd == "look":
        look(owner); return
    if cmd == "cost":
        cost(owner); return
    if cmd == "restore":
        restore(owner); return
    raise SystemExit("unknown subcommand " + cmd)


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "snapshot")
