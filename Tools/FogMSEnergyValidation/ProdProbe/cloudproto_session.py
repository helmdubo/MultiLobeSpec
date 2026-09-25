# -*- coding: utf-8 -*-
"""FogMS P1 session (FogMS_PerPixelClouds_Design.md section 4, 'P1'): the owner's Live Box rendered by the engine's
Volumetric Cloud (per-pixel ray march) instead of volumetric-fog froxels, in the running editor, without C++ or engine edits.
Runs outside the editor (CPython) and drives it through the UE-MCP bridge (uemcp.py, ws://127.0.0.1:9877).

Data flow of the prototype (every frame):
  Box (C++, unchanged): density + animation phases -> Box MID (M_FogMS_Density) -> froxel copy; solver -> TransportField (hybrid)
  tick callback (Python, slate post-tick, self-unregisters on the first exception):
      Box MID FogMS_FroxelWeight <- weight (0 = the froxel copy adds nothing to the volumetric fog, 1 = as round 38)
      cloud MID FogMS_WorldPhase0..2 <- Box MID (animation; one frame late: the cloud renders the phases the Box set this tick)
  cloud actor 'FogMS - Cloud P1' (AVolumetricCloud, the only visible cloud component: the sky's is hidden) with the MID of
      /MultiLobeSpec/FogMS/Proto/M_FogMS_CloudBox_P1 (cloudproto_material.py): every other Box MID parameter, the Box's field
      texture and the density cube's world-to-local rows are copied once by sync() (again after any Box property change).
Host settings (design table 3.9): layer = the Box's density band (height profile Bottom..Top, else the Box) +- 10 m, at least
0.1 km; TracingMaxDistanceMode = from the camera, TracingMaxDistance = r.VolumetricCloud.DistanceToSampleMaxCount = D km;
ViewSampleCountScale S (samples = min(96 S, 768)): step = D / samples on rays shorter than D; sun march 0.25 km x 32 samples;
stop at transmittance 0.005; r.VolumetricCloud.SampleMinCount 8; per-sample atmosphere transmittance off; not visible in the
real-time sky capture; r.VolumetricRenderTarget.Mode M.

Safety (project rules): the map is never saved; snapshot() writes measure/owner_pre39.json once (the restore target, reused
after a power cut); restore() puts back the Box, Height Fog, cvars (incl. r.SkyLight.RealTimeReflectionCapture.TimeSlice and
the cloud cvars), the sky clouds' visibility, the Box MID's FogMS_FroxelWeight / FogMS_Albedo, the camera and the background
throttle, destroys the prototype actor and prints 'RESTORE box diff {...}'.
Usage: python cloudproto_session.py snapshot|matedit|material|start [variant]|froxel|boxoff|cloud|stop|restore|status"""
import os, sys, json, time
import diag34lib as d
import diag35lib as L

HERE = d.HERE
MEAS = os.path.join(HERE, "measure")
OWNER = os.path.join(MEAS, "owner_pre39.json")
LABEL = "FogMS - Cloud P1"
PROTO = "/MultiLobeSpec/FogMS/Proto/M_FogMS_CloudBox_P1"
LOG = os.environ.get("FOGMS_LOG", r"E:/GITHUB/MultiLobeSpec/.codex-build/FogMS_Prod_20260922/Main38c.log")
# Cloud cvars the prototype changes (restored from the snapshot).
P1_CVARS = ["r.VolumetricCloud.DistanceToSampleMaxCount", "r.VolumetricCloud.SampleMinCount", "r.VolumetricRenderTarget.Mode",
            "r.VolumetricRenderTarget.UpsamplingMode", "r.VolumetricCloud.StepSizeOnZeroConservativeDensity",
            "r.VolumetricRenderTarget.MinimumDistanceKmToEnableReprojection", "r.SkyLight.RealTimeReflectionCapture.TimeSlice",
            "r.VolumetricCloud.ViewRaySampleMaxCount", "r.VolumetricCloud.Shadow.ViewRaySampleMaxCount"]
# Host variants (design section 5, series 1). D km, S = ViewSampleCountScale, M = r.VolumetricRenderTarget.Mode.
HOST = {"p1": {"D": 2.0, "S": 8.0, "M": 0}, "p1_d1": {"D": 1.0, "S": 8.0, "M": 0}, "p1_d5": {"D": 5.0, "S": 8.0, "M": 0},
        "p1_s4": {"D": 2.0, "S": 4.0, "M": 0}, "p1_m1": {"D": 2.0, "S": 8.0, "M": 1}}
SHADOW = {"distance_km": 0.25, "scale": 3.2, "stop": 0.005, "min_samples": 8}
# Box MID parameters copied into the cloud MID (all that M_FogMS_CloudBox_P1 shares with M_FogMS_Density).
SCALARS = ["FogMS_WorldAligned", "FogMS_DetailScale", "FogMS_Threshold", "FogMS_Softness", "FogMS_Density", "FogMS_DensityFeather",
           "FogMS_DetailStrength", "FogMS_DetailSecondOctave", "FogMS_InjectionMode", "FogMS_ErosionStrength", "FogMS_ErosionDepth",
           "FogMS_HeightProfile", "FogMS_HeightBottom", "FogMS_HeightTop", "FogMS_HeightBottomSoftness", "FogMS_HeightTopSoftness",
           "FogMS_HeightAnvilStrength", "FogMS_ForwardStrength", "FogMS_ForwardG", "FogMS_ForwardDepth", "FogMS_ForwardFloor",
           "FogMS_ForwardEcc"]
VECTORS = ["FogMS_TileScale", "FogMS_WorldFrequencies", "FogMS_WorldPhase0", "FogMS_WorldPhase1", "FogMS_WorldPhase2",
           "FogMS_ChannelMask", "FogMS_WorldExtent", "FogMS_Albedo", "FogMS_ErosionMask"]
TEXTURES = ["FogMS_Noise", "FogMS_TransportField"]

FIND = ("import unreal, json\n"
        "_A=unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()\n"
        "A={a.get_actor_label():a for a in _A}\n"
        "box=A['FogMS - Live Box']; dc=box.get_editor_property('density_component'); bmid=dc.get_material(0)\n"
        "sky=[a for a in _A if a.get_class().get_name()=='VolumetricCloud' and a.get_actor_label()!=%r]\n"
        "proto=[a for a in _A if a.get_actor_label()==%r]\n") % (LABEL, LABEL)


def last_json(out, tag):
    for line in reversed(out.splitlines()):
        if line.startswith(tag + " "):
            return json.loads(line[len(tag) + 1:])
    raise RuntimeError("no %s line in editor output: %s" % (tag, out[-800:]))


def python_errors():
    """Count of 'LogPython: Error' lines in the editor log (a flood means a broken callback: stop)."""
    try:
        with open(LOG, "rb") as f:
            return f.read().count(b"LogPython: Error")
    except OSError:
        return -1


# ------------------------------------------------------------------ snapshot / restore
def snapshot_extra():
    """Sky clouds' editor visibility, prototype presence, the Box MID's FogMS_Albedo (the opacity renders zero it) and the
    actor list. FogMS_FroxelWeight is NOT snapshotted: before the patch the parameter does not exist and reads 0; restore()
    always puts the identity 1 (the material default) back."""
    out = d.py(FIND + "mid_v={p: [c for c in (lambda v: (v.r, v.g, v.b, v.a))(bmid.get_vector_parameter_value(p))] for p in ('FogMS_Albedo',)}\n"
               "w=unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()\n"
               "print('EXTRA '+json.dumps({'sky_hidden': {a.get_actor_label(): bool(a.is_temporarily_hidden_in_editor()) for a in sky},"
               " 'proto_present': len(proto), 'box_mid_vector': mid_v, 'time_dilation': unreal.GameplayStatics.get_global_time_dilation(w),"
               " 'actors': sorted(a.get_actor_label() for a in _A)}))")
    return last_json(out, "EXTRA")


def set_time_dilation(value):
    """World time dilation of the editor world (the Box animation clock is World->GetTimeSeconds). d39_cloud.py uses it to
    advance the density ~1/25 s per captured frame while PNG dumps run at ~6 fps."""
    out = d.py("import unreal\nw=unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()\n"
               "unreal.GameplayStatics.set_global_time_dilation(w, %r)\nprint('TD', unreal.GameplayStatics.get_global_time_dilation(w))" % float(value))
    return float(out.split("TD")[-1].split()[0])


def weight():
    """The Box MID's FogMS_FroxelWeight; 1.0 (identity) when the material has no such parameter (not patched yet)."""
    out = d.py(FIND + "names=[str(n) for n in unreal.MaterialEditingLibrary.get_scalar_parameter_names(bmid)]\n"
               "print('WEIGHT', bmid.get_scalar_parameter_value('FogMS_FroxelWeight') if 'FogMS_FroxelWeight' in names else 1.0)")
    return float(out.split("WEIGHT")[-1].split()[0])


def snapshot(path=OWNER, force=False):
    """The owner's state before P1 (written once; after a power cut the existing file stays the restore target)."""
    if os.path.isfile(path) and not force:
        return json.load(open(path))
    s = L.snapshot()
    s["cvars"].update(L.getcv(P1_CVARS))
    s["extra"] = snapshot_extra()
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        json.dump(s, f, indent=1); f.flush(); os.fsync(f.fileno())
    os.replace(tmp, path)
    return s


def stop():
    """Unregister the tick callback, destroy the prototype actor(s), weight 1 on the Box MID (the material default)."""
    return d.py(FIND + "h=getattr(unreal,'_p1_h',None)\n"
                "if h is not None:\n"
                "    try: unreal.unregister_slate_post_tick_callback(h)\n"
                "    except Exception as e: print('unregister', e)\n"
                "unreal._p1_h=None\n"
                "for a in proto: unreal.get_editor_subsystem(unreal.EditorActorSubsystem).destroy_actor(a)\n"
                "bmid.set_scalar_parameter_value('FogMS_FroxelWeight', 1.0)\n"
                "print('STOPPED proto', len(proto), 'weight', bmid.get_scalar_parameter_value('FogMS_FroxelWeight'))")


def restore(owner=None):
    """Everything back to the owner snapshot; prints the Box diff (must be {})."""
    owner = owner or json.load(open(OWNER))
    out = [stop()]
    ex = owner.get("extra", {})
    code = FIND
    for label, hidden in ex.get("sky_hidden", {}).items():
        code += "A[%r].set_is_temporarily_hidden_in_editor(%s)\n" % (label, bool(hidden))
    code += "bmid.set_scalar_parameter_value('FogMS_FroxelWeight', 1.0)\n"   # identity (material default), never a snapshot value
    code += ("unreal.GameplayStatics.set_global_time_dilation(unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world(), %r)\n"
             % float(ex.get("time_dilation", 1.0)))
    for p, v in ex.get("box_mid_vector", {}).items():
        code += "bmid.set_vector_parameter_value(%r, unreal.LinearColor(%r, %r, %r, %r))\n" % ((p,) + tuple(float(c) for c in v))
    for p, v in ex.get("box_extra", {}).items():   # Box properties outside diag35lib.BOXPROPS (wind_speed)
        code += "if box.get_editor_property(%r) != %r: box.set_editor_property(%r, %r)\n" % (p, v, p, v)
    code += "print('EXTRA restored')"
    out.append(d.py(code))
    out += L.restore(owner)          # Box, Height Fog, cvars (incl. P1_CVARS, TimeSlice), camera, throttle
    now = L.snapshot(); now["cvars"].update(L.getcv(P1_CVARS)); nex = snapshot_extra()
    if ex.get("box_extra"):
        got = last_json(d.py(FIND + "print('BOXEXTRA '+json.dumps({p: box.get_editor_property(p) for p in %r}))" % (list(ex["box_extra"]),)), "BOXEXTRA")
        box_extra_diff = {k: (v, got.get(k)) for k, v in ex["box_extra"].items() if abs(float(v) - float(got.get(k, 1e30))) > 1e-6}
    else:
        box_extra_diff = {}
    box_diff = {k: (owner["box"][k], now["box"].get(k)) for k in owner["box"] if owner["box"][k] != now["box"].get(k)}
    box_diff.update(box_extra_diff)
    cv_diff = {k: (v, now["cvars"].get(k)) for k, v in owner["cvars"].items()
               if k not in ("r.BufferVisualizationDumpFrames", "r.DumpingMovie") and abs(float(v) - float(now["cvars"].get(k, 1e30))) > 1e-6}
    hf_diff = {k: (v, now["hf"].get(k)) for k, v in owner["hf"].items() if v != now["hf"].get(k)}
    ex_diff = {k: (ex.get(k), nex.get(k)) for k in ("sky_hidden", "proto_present", "box_mid_vector", "actors", "time_dilation")
               if k in ex and ex.get(k) != nex.get(k)}
    w = weight()
    if abs(w - 1.0) > 1e-6:
        ex_diff["box_weight"] = w
    cam_diff = max(abs(a - b) for a, b in zip(owner["camera"][0] + owner["camera"][1], now["camera"][0] + now["camera"][1]))
    print("RESTORE box diff", box_diff, flush=True)
    print("RESTORE hf diff", hf_diff, "cvar diff", cv_diff, "extra diff", ex_diff, "camera max diff %.4f" % cam_diff,
          "throttle", now["throttle"], "status", now["status"][:120], flush=True)
    return {"box": box_diff, "hf": hf_diff, "cvars": cv_diff, "extra": ex_diff, "camera": cam_diff, "status": now["status"]}


# ------------------------------------------------------------------ prototype
def pylong(code, timeout=1800.0):
    """execute_python with a long socket timeout (material builds wait for shader compilation inside the call)."""
    import uemcp
    r = uemcp.call("execute_python", {"code": code}, timeout=timeout)
    res = r.get("result") or {}
    return ((res.get("output") or "") or json.dumps(r)[:2000]).replace("\r\n\n", "\n").replace("\r\n", "\n").strip()


def run_file(name):
    """Runs a script of this folder inside the editor from disk (runpy sets __file__; the bridge rejects ~70 KB payloads)."""
    path = os.path.join(HERE, name).replace("\\", "/")
    return pylong("import runpy\nrunpy.run_path(%r, run_name='__main__')" % path)


def material():
    """Build or check the prototype material in the editor (cloudproto_material.py; idempotent)."""
    return run_file("cloudproto_material.py")


def matedit():
    """FogMS_FroxelWeight patch of M_FogMS_Density (matedit_density.py; idempotent, saves the material only when it changed)."""
    return run_file("matedit_density.py")


TICK = r'''
import unreal
def _p1_install():
    old = getattr(unreal, '_p1_h', None)
    if old is not None:
        try: unreal.unregister_slate_post_tick_callback(old)
        except Exception: pass
        unreal._p1_h = None
    A = {a.get_actor_label(): a for a in unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()}
    dc = A['FogMS - Live Box'].get_editor_property('density_component')
    cloud = A.get(%(label)r)
    cmid = cloud.get_component_by_class(unreal.VolumetricCloudComponent).get_editor_property('material') if cloud else None
    st = getattr(unreal, '_p1_state', None) or {}
    st.setdefault('weight', 1.0); st.setdefault('copy', True); st['ticks'] = 0; st['error'] = ''
    unreal._p1_state = st
    PH = ('FogMS_WorldPhase0', 'FogMS_WorldPhase1', 'FogMS_WorldPhase2')
    def _tick(dt):
        try:
            st['ticks'] += 1
            mid = dc.get_material(0)
            if isinstance(mid, unreal.MaterialInstanceDynamic):
                w = float(st['weight'])
                if mid.get_scalar_parameter_value('FogMS_FroxelWeight') != w:
                    mid.set_scalar_parameter_value('FogMS_FroxelWeight', w)
                if cmid is not None and st['copy']:
                    for p in PH:
                        cmid.set_vector_parameter_value(p, mid.get_vector_parameter_value(p))
        except Exception as e:
            try: unreal.unregister_slate_post_tick_callback(unreal._p1_h)
            except Exception: pass
            unreal._p1_h = None
            st['error'] = str(e)
            try: unreal.log_warning('FogMS P1 tick stopped: %%s' %% e)
            except Exception: pass
    unreal._p1_h = unreal.register_slate_post_tick_callback(_tick)
    print('TICK installed, cloud MID', cmid.get_name() if cmid else None)
_p1_install()
''' % {"label": LABEL}


def set_state(weight=None, copy=None, cloud_visible=None):
    """weight: Box froxel copy (0/1); cloud_visible: the prototype actor's editor visibility."""
    code = FIND + "st=getattr(unreal,'_p1_state',None)\nif st is None:\n    st={}; unreal._p1_state=st\n"
    if weight is not None:
        code += "st['weight']=%r\nbmid.set_scalar_parameter_value('FogMS_FroxelWeight', %r)\n" % (float(weight), float(weight))
    if copy is not None:
        code += "st['copy']=%s\n" % bool(copy)
    if cloud_visible is not None:
        code += "[a.set_is_temporarily_hidden_in_editor(%s) for a in proto]\n" % (not cloud_visible)
    code += ("print('STATE '+json.dumps({'weight': st.get('weight'), 'tick': getattr(unreal,'_p1_h',None) is not None, 'ticks': st.get('ticks'),"
             " 'error': st.get('error',''), 'cloud_hidden': [bool(a.is_temporarily_hidden_in_editor()) for a in proto],"
             " 'box_weight': bmid.get_scalar_parameter_value('FogMS_FroxelWeight')}))")
    return last_json(d.py(code), "STATE")


def host_layer():
    """Cloud layer (km above the SkyAtmosphere ground = world Z 0 here, PLANET_TOP_AT_ABSOLUTE_WORLD_ORIGIN) enclosing the
    Box's density band: the height-profile band Bottom..Top of an upright Box, else the Box's world Z range; +-10 m, >= 0.1 km."""
    out = d.py(FIND + "t=dc.get_world_transform(); q=t.rotation; s=t.scale3d; c=t.translation\n"
               "ax=[q.rotate_vector(unreal.Vector(1,0,0)), q.rotate_vector(unreal.Vector(0,1,0)), q.rotate_vector(unreal.Vector(0,0,1))]\n"
               "E=[abs(s.x)*50.0, abs(s.y)*50.0, abs(s.z)*50.0]\n"
               "hp=box.get_editor_property('height_profile'); hb=box.get_editor_property('height_bottom'); ht=box.get_editor_property('height_top')\n"
               "sa=[a for a in _A if a.get_class().get_name()=='SkyAtmosphere']\n"
               "mode=str(sa[0].get_component_by_class(unreal.SkyAtmosphereComponent).get_editor_property('transform_mode')) if sa else 'none'\n"
               "print('LAYER '+json.dumps({'c': [c.x, c.y, c.z], 'E': E, 'axz': [ax[0].z, ax[1].z, ax[2].z], 'hp': bool(hp), 'hb': hb, 'ht': ht, 'sky': mode}))")
    g = last_json(out, "LAYER")
    if "ABSOLUTE_WORLD_ORIGIN" not in g["sky"]:
        raise RuntimeError("SkyAtmosphere transform mode %s: the layer altitude needs the planet top at Z 0" % g["sky"])
    upright = abs(g["axz"][2]) > 0.999
    if g["hp"] and upright:
        z0 = g["c"][2] - g["E"][2] + g["hb"] * 2 * g["E"][2]; z1 = g["c"][2] - g["E"][2] + g["ht"] * 2 * g["E"][2]
    else:
        h = sum(abs(a) * e for a, e in zip(g["axz"], g["E"])); z0, z1 = g["c"][2] - h, g["c"][2] + h
    bottom = (z0 - 1000.0) / 1e5; height = max(0.1, (z1 - z0 + 2000.0) / 1e5)
    return {"bottom_km": bottom, "height_km": height, "band_cm": [z0, z1]}


def apply_host(name_or_host):
    """Host settings of a variant (HOST name or dict with D, S, M) on the prototype component and the cvars."""
    h = HOST[name_or_host] if isinstance(name_or_host, str) else name_or_host
    lay = host_layer()
    for k, v in (("r.VolumetricCloud.DistanceToSampleMaxCount", h["D"]), ("r.VolumetricCloud.SampleMinCount", SHADOW["min_samples"]),
                 ("r.VolumetricRenderTarget.Mode", h["M"])):
        d.cmd("%s %g" % (k, v))
    # set_editor_property (Pre/PostEditChange: the component re-registers, its proxy is re-added as the newest cloud) for every
    # value; unchanged values are skipped so a repeated apply does not re-register the component.
    want = {"layer_bottom_altitude": lay["bottom_km"], "layer_height": lay["height_km"], "tracing_max_distance": h["D"],
            "tracing_start_distance_from_camera": 0.0, "view_sample_count_scale": h["S"], "shadow_view_sample_count_scale": SHADOW["scale"],
            "shadow_tracing_distance": SHADOW["distance_km"], "stop_tracing_transmittance_threshold": SHADOW["stop"]}
    out = d.py(FIND + "vc=proto[0].get_component_by_class(unreal.VolumetricCloudComponent)\n"
               "for k,v in %r.items():\n"
               "    if abs(float(vc.get_editor_property(k)) - v) > 1e-7: vc.set_editor_property(k, v)\n"
               "M=unreal.VolumetricCloudTracingMaxDistanceMode.DISTANCE_FROM_POINT_OF_VIEW\n"
               "if vc.get_editor_property('tracing_max_distance_mode') != M: vc.set_editor_property('tracing_max_distance_mode', M)\n"
               "for k in ('visible_in_real_time_sky_captures', 'use_per_sample_atmospheric_light_transmittance'):\n"
               "    if vc.get_editor_property(k): vc.set_editor_property(k, False)\n"
               "print('HOST '+json.dumps({k: str(vc.get_editor_property(k)) for k in ('layer_bottom_altitude','layer_height','tracing_max_distance_mode',"
               "'tracing_max_distance','view_sample_count_scale','shadow_view_sample_count_scale','shadow_tracing_distance',"
               "'stop_tracing_transmittance_threshold','visible_in_real_time_sky_captures','use_per_sample_atmospheric_light_transmittance')}))"
               % (want,))
    r = last_json(out, "HOST"); r.update(lay); r.update({"cvars": L.getcv(["r.VolumetricCloud.DistanceToSampleMaxCount",
                                                                         "r.VolumetricCloud.SampleMinCount", "r.VolumetricRenderTarget.Mode"])})
    return r


def sync(look=None):
    """Copy the Box MID into the cloud MID (all shared parameters, the field, the density cube's rows) and the host look
    (P1_PhaseG / G2 / Blend, P1_FieldGain; default: Phase G of the Box, G2 0, Blend 0, gain 1). look = dict of cloud MID scalar
    overrides applied after the copy (e.g. {'FogMS_ForwardStrength': 0.0})."""
    look = dict(look or {})
    code = FIND + ("def _sync():\n"
                   "    if not proto: return {'skipped': 'no prototype actor'}\n"
                   "    vc=proto[0].get_component_by_class(unreal.VolumetricCloudComponent); cmid=vc.get_editor_property('material')\n"
                   "    for p in %r: cmid.set_scalar_parameter_value(p, bmid.get_scalar_parameter_value(p))\n"
                   "    for p in %r: cmid.set_vector_parameter_value(p, bmid.get_vector_parameter_value(p))\n"
                   "    for p in %r:\n"
                   "        tx=bmid.get_texture_parameter_value(p)\n"
                   "        if tx: cmid.set_texture_parameter_value(p, tx)\n"
                   "    t=dc.get_world_transform(); q=t.rotation; s=t.scale3d; c=t.translation\n"
                   "    cmid.set_vector_parameter_value('P1_BoxCenter', unreal.LinearColor(c.x, c.y, c.z, 0.0))\n"
                   "    for i,(e,sc) in enumerate(((unreal.Vector(1,0,0), s.x), (unreal.Vector(0,1,0), s.y), (unreal.Vector(0,0,1), s.z))):\n"
                   "        a=q.rotate_vector(e); cmid.set_vector_parameter_value('P1_WorldToLocal%%d' %% i, unreal.LinearColor(a.x/sc, a.y/sc, a.z/sc, 0.0))\n"
                   "    g=float(box.get_editor_property('phase_g'))\n"
                   "    look={'P1_PhaseG': g, 'P1_PhaseG2': 0.0, 'P1_PhaseBlend': 0.0, 'P1_FieldGain': 1.0}\n    look.update(%r)\n"
                   "    for p,v in look.items(): cmid.set_scalar_parameter_value(p, float(v))\n"
                   "    chk={p: cmid.get_scalar_parameter_value(p) for p in ('FogMS_Density','FogMS_Threshold','FogMS_InjectionMode','FogMS_ForwardStrength','P1_PhaseG','P1_PhaseG2','P1_PhaseBlend','P1_FieldGain')}\n"
                   "    tf=cmid.get_texture_parameter_value('FogMS_TransportField')\n"
                   "    return {'chk': chk, 'field': tf.get_name() if tf else None, 'rows': [str(cmid.get_vector_parameter_value('P1_WorldToLocal%%d' %% i)) for i in range(3)]}\n"
                   "try:\n    _r=_sync()\nexcept Exception as _e:\n    _r={'error': str(_e)}\n"
                   "print('SYNC '+json.dumps(_r))"
                   ) % (SCALARS, VECTORS, TEXTURES, look)
    return last_json(d.py(code), "SYNC")


def start(host="p1", froxel=True):
    """Hide the sky clouds, spawn (or reuse) the prototype actor with a MID of the prototype material, apply the host, copy the
    parameters, install the tick callback. Starts in froxel mode (weight 1, prototype hidden) unless froxel=False."""
    L.set_throttle(False)
    d.cmd("r.SkyLight.RealTimeReflectionCapture.TimeSlice 0")
    out = d.py(FIND + "[a.set_is_temporarily_hidden_in_editor(True) for a in sky]\n"
               "m=unreal.load_asset(%r)\n"
               "if m is None: raise RuntimeError('prototype material missing: run cloudproto_material.py')\n"
               "if not proto:\n"
               "    a=unreal.get_editor_subsystem(unreal.EditorActorSubsystem).spawn_actor_from_class(unreal.VolumetricCloud, unreal.Vector(0,0,0))\n"
               "    a.set_actor_label(%r); proto=[a]\n"
               "vc=proto[0].get_component_by_class(unreal.VolumetricCloudComponent)\n"
               "cur=vc.get_editor_property('material')\n"
               "if not (isinstance(cur, unreal.MaterialInstanceDynamic) and cur.get_editor_property('parent')==m):\n"
               "    mid=unreal.MaterialLibrary.create_dynamic_material_instance(proto[0], m, 'MID_FogMS_CloudBox_P1')\n"
               "    vc.set_material(mid)\n"
               "    if vc.get_editor_property('material') != mid: vc.set_editor_property('material', mid)\n"
               "    if vc.get_editor_property('material') != mid: raise RuntimeError('cannot assign the prototype MID')\n"
               "print('START '+json.dumps({'sky_hidden': [bool(a.is_temporarily_hidden_in_editor()) for a in sky], 'proto': proto[0].get_actor_label(),"
               " 'mid': vc.get_editor_property('material').get_name()}))" % (PROTO, LABEL))
    r = {"start": last_json(out, "START")}
    r["host"] = apply_host(host)
    r["sync"] = sync()
    d.py(TICK)
    r["state"] = set_state(weight=1.0 if froxel else 0.0, copy=True, cloud_visible=not froxel)
    return r


def mode(name):
    """'froxel' (Box in the fog, prototype hidden), 'boxoff' (neither), 'cloud' (prototype only)."""
    w, vis = {"froxel": (1.0, False), "boxoff": (0.0, False), "cloud": (0.0, True)}[name]
    return set_state(weight=w, cloud_visible=vis)


if __name__ == "__main__":
    a = sys.argv[1:] or ["status"]
    if a[0] == "snapshot":
        s = snapshot(force="--force" in a); print(json.dumps({k: s[k] for k in ("camera", "throttle", "status", "extra")}, indent=1))
    elif a[0] == "material":
        print(material())
    elif a[0] == "matedit":
        print(matedit())
    elif a[0] == "start":
        snapshot(); print(json.dumps(start(a[1] if len(a) > 1 else "p1"), indent=1))
    elif a[0] in ("froxel", "boxoff", "cloud"):
        print(mode(a[0]))
    elif a[0] == "stop":
        print(stop())
    elif a[0] == "restore":
        restore()
    else:
        print(json.dumps(set_state(), indent=1), "python errors in log:", python_errors())
