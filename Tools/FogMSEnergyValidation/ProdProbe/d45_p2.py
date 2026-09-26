# -*- coding: utf-8 -*-
"""Round 45 / P2 (FogMS_PerPixelClouds_Design.md section 4 'P2'): Box property Render Path = Cloud Host in C++ (the Box renders
through the engine Volumetric Cloud 'cloud host' with M_FogMS_Cloud / MI_FogMS_Cloud instead of volumetric-fog froxels). Lean
verification plan (owner, 2026-09-26): one install, one noise floor per session, status checks + one frame per state.

Frames: measure/diag45 (junction to D:/FogMS_ProbeFrames/diag45); results: results/diag45 (JSON written atomically, present keys
skipped: resumable after a power cut). Every run starts and ends at the owner snapshot measure/owner_pre45.json (written once, in
the session the owner left, before any change: d41 schema = cloudproto_session.snapshot + DensityMotionReference, sun rotation, W41
props, sky cloud visibility). The map is never saved; restore prints its diff. During captures: background throttle off, the sky
Volumetric Cloud hidden (it is hidden in the owner's session anyway), r.SkyLight.RealTimeReflectionCapture.TimeSlice 0, LIT
(ShowFlag.OverrideDiffuseAndSpecular 0). A cloud host created by a run is destroyed before the run returns.

Subcommands:
  snapshot [--force]  measure/owner_pre45.json                                                   [round-44 session, before any change]
  idref               criterion 1 reference: owner view, Box frozen t = 100 s, 2 frames x 2 (a, b = floor) + Box MID dump [round 44]
  id45                the same in the round-45 session (Render Path default Froxel Fog) -> identity vs idref           [round 45]
  restore             restore the snapshot
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
IDP = os.path.join(RES, "identity.json")
FROZEN_T = 100.0
ID_FRAMES = 4          # frames per identity capture; the mean of the last 2 is compared


def _atomic_dump(obj, path):
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        json.dump(obj, f, indent=1); f.flush(); os.fsync(f.fileno())
    os.replace(tmp, path)


def _load(path):
    return json.load(open(path)) if os.path.isfile(path) else {}


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


def restore(owner):
    return X.restore(owner)


def begin():
    X.begin(lit=True)


# ------------------------------------------------------------------ identity (criterion 1)
MID_DUMP = ("mid=bmid\nml=unreal.MaterialEditingLibrary\n"
            "sc={str(n): mid.get_scalar_parameter_value(n) for n in ml.get_scalar_parameter_names(mid)}\n"
            "vc={str(n): (lambda v: [v.r, v.g, v.b, v.a])(mid.get_vector_parameter_value(n)) for n in ml.get_vector_parameter_names(mid)}\n"
            "tx={}\n"
            "for n in ml.get_texture_parameter_names(mid):\n"
            "    t=mid.get_texture_parameter_value(n); tx[str(n)]=t.get_class().get_name() if t else None\n"
            "print('MIDDUMP '+json.dumps({'scalars': sc, 'vectors': vc, 'textures': tx, 'parent': mid.get_editor_property('parent').get_name()}))")


def mid_dump():
    return S.last_json(d.py(S.FIND + MID_DUMP), "MIDDUMP")


def identity(tag, owner):
    """Owner view, Box frozen at t = 100 s, owner properties otherwise; captures id_<tag>_a / _b (the b repeat is the floor)."""
    res = _load(IDP)
    names = ["id_%s_a" % tag, "id_%s_b" % tag]
    if all(n in res for n in names):
        return res
    if C.editor_minimized():
        raise SystemExit("The Unreal Editor window is minimized: nothing would render. Nothing changed; retry when it is restored.")
    C.flood_check()
    begin()
    try:
        X.freeze(owner, True)
        C.set_cam(*owner["camera"]); time.sleep(5.0)
        for nm in names:
            if nm in res:
                continue
            _, got = C.capture(nm, ID_FRAMES)
            if got < ID_FRAMES:
                raise RuntimeError("%s: %d of %d frames (editor minimized?)" % (nm, got, ID_FRAMES))
            F = d.load(nm)[-2:].mean(axis=0)
            np.save(os.path.join(d.M, nm, "mean.npy"), F.astype(np.float32))
            X.save_png(F, os.path.join(LOOKDIR, "%s.png" % nm))
            res = _load(IDP)
            res[nm] = {"frames": got, "status": d.status()[:400], "mid": mid_dump()}
            _atomic_dump(res, IDP)
            print("IDENTITY", nm, got, res[nm]["status"][-160:], flush=True)
            time.sleep(2.0)
    finally:
        X.freeze(owner, False)
        restore(owner)
    return _load(IDP)


def _mean(nm):
    p = os.path.join(d.M, nm, "mean.npy")
    return np.load(p).astype(np.float64) if os.path.isfile(p) else None


def cmp_frames(a, b, roi=None):
    A, Bm = (_mean(a) if isinstance(a, str) else a), (_mean(b) if isinstance(b, str) else b)
    if A is None or Bm is None:
        return None
    diff = np.abs(A - Bm)
    if roi is not None:
        diff = diff[roi]
    mse = float((diff ** 2).mean())
    return {"mae": round(float(diff.mean()), 4), "p99": round(float(np.percentile(diff, 99)), 2),
            "psnr": round(10 * math.log10(255.0 ** 2 / max(mse, 1e-12)), 2),
            "mean_diff": round(float(((A - Bm)[roi] if roi is not None else (A - Bm)).mean()), 4)}


def identity_report():
    res = _load(IDP)
    out = {}
    for a, b in (("id_44_a", "id_44_b"), ("id_45_a", "id_45_b"), ("id_44_a", "id_45_a"), ("id_44_b", "id_45_b"), ("id_44_b", "id_45_a")):
        out["%s~%s" % (a, b)] = cmp_frames(a, b)
    # MID identity: every parameter of the round-44 Box MID in the round-45 one (FogMS_FroxelWeight is new: material default 1)
    m44, m45 = (res.get("id_44_a") or {}).get("mid"), (res.get("id_45_a") or {}).get("mid")
    if m44 and m45:
        diffs = {}
        for kind in ("scalars", "vectors", "textures"):
            for k in sorted(set(m44[kind]) | set(m45[kind])):
                if m44[kind].get(k) != m45[kind].get(k):
                    diffs["%s.%s" % (kind, k)] = [m44[kind].get(k), m45[kind].get(k)]
        out["mid_diff"] = diffs
        out["status_44"] = res["id_44_a"]["status"]; out["status_45"] = res["id_45_a"]["status"]
    res["report"] = out; _atomic_dump(res, IDP)
    return out


# ------------------------------------------------------------------ main
def main(cmd):
    if cmd == "snapshot":
        s = snapshot(force="--force" in sys.argv)
        print(json.dumps({k: s[k] for k in ("camera", "throttle", "status", "extra", "x41")}, indent=1)); return
    owner = snapshot()
    if cmd == "idref":
        identity("44", owner); print(json.dumps(identity_report(), indent=1)); return
    if cmd == "id45":
        identity("45", owner); print(json.dumps(identity_report(), indent=1)); return
    if cmd == "restore":
        restore(owner); return
    raise SystemExit("unknown subcommand " + cmd)


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "snapshot")
