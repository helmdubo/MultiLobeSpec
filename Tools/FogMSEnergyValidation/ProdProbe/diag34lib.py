# -*- coding: utf-8 -*-
"""Round-34 diagnostics helpers (D1 black blocks, D2 camera-motion tremble, D3 spot flicker).
Frame capture WITHOUT HighResShot: r.BufferVisualizationDumpFrames 1 + r.DumpingMovie N makes every realtime level
viewport write its back buffer (the real, temporally accumulated frame) as MovieFrameNNNNN.png for N consecutive frames.
Works in the FourPanes layout (only the realtime perspective pane dumps). Extra G-buffer dumps are suppressed by pointing
r.BufferVisualizationOverviewTargets at a non-existent target (restore_overview() puts the engine default back).
An in-editor slate post-tick callback arms the dump and can move the viewport camera by a fixed step every tick
(one tick = one frame), logging per-tick wall time, so camera motion and frame dumps stay in lock-step."""
import sys, os, time, subprocess, json, glob, shutil
HERE = os.path.dirname(os.path.abspath(__file__))
M = os.path.join(HERE, "measure", "diag34")
SS = "D:/PersonalProjects/UE5/MimirHead_portfolio 5.7 5.8 - 3/Saved/Screenshots/WindowsEditor"
BOX = "b=[a for a in unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors() if a.get_actor_label()=='FogMS - Live Box'][0]"
ACT = "A={a.get_actor_label():a for a in unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()}"
DEFAULT_OVERVIEW = ("BaseColor,Specular,SubsurfaceColor,WorldNormal,SeparateTranslucencyRGB,,,WorldTangent,SeparateTranslucencyA,,,"
                    "Opacity,SceneDepth,Roughness,Metallic,ShadingModel,,SceneDepthWorldUnits,SceneColor,PreTonemapHDRColor,PostTonemapHDRColor")

def py(code, timeout=180):
    r = subprocess.run([sys.executable, os.path.join(HERE, "uemcp.py"), "py", code], capture_output=True, text=True,
                       encoding="utf-8", errors="replace", timeout=timeout)
    try:
        j = json.loads(r.stdout)
        return ((j.get("result") or {}).get("output", "") or str(j.get("error", ""))).replace("\r\n\n", "\n").replace("\r\n", "\n").strip()
    except Exception:
        return r.stdout[-400:]

def cmd(c):
    subprocess.run([sys.executable, os.path.join(HERE, "uemcp.py"), "cmd", c], capture_output=True, timeout=60)

def status():
    return py("import unreal\n" + BOX + "\nprint(b.get_editor_property('spatial_status'))")

def setbox(assign):
    return py("import unreal\n" + BOX + "\n" + assign + "\nb.update_density()\nprint(b.get_editor_property('spatial_status'))")

def set_g(g):
    return py("import unreal\n" + ACT + "\nhf=A['FogMS - Height Fog'].get_component_by_class(unreal.ExponentialHeightFogComponent)\n"
              "hf.set_volumetric_fog_scattering_distribution(%r)\n"
              "print('g=', hf.get_editor_property('volumetric_fog_scattering_distribution'))" % float(g))

def set_cam(loc, rot):
    return py("import unreal\nles=unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem)\n"
              "les.set_level_viewport_camera_info(unreal.Vector(%r,%r,%r), unreal.Rotator(pitch=%r, yaw=%r, roll=%r))\n"
              "l,r=les.get_level_viewport_camera_info(); print('CAM', l, r)" % (loc[0], loc[1], loc[2], rot[0], rot[1], rot[2]))

def _frames():
    return set(glob.glob(os.path.join(SS, "MovieFrame[0-9][0-9][0-9][0-9][0-9].png")))

def capture(name, n=8, move=None, settle=0.0, timeout=300):
    """Dump n consecutive frames into measure/diag34/<name>/f###.png. move=(dx,dy,dz) cm per tick moves the camera
    every tick from the arming tick on (so motion is already steady when the first frame is dumped)."""
    out = os.path.join(M, name)
    shutil.rmtree(out, ignore_errors=True); os.makedirs(out)
    if settle: time.sleep(settle)
    before = _frames()
    cmd("r.BufferVisualizationOverviewTargets FogMSNoTarget")
    cmd("r.BufferVisualizationDumpFrames 1")
    tj = os.path.join(out, "ticks.json").replace("\\", "/")
    code = ("import unreal, time, json\nles=unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem)\n"
            "st={'n':0,'t':[],'cam':[]}\nN=%d\nMV=%r\nPRE=%d\n"
            "def _tick(dt):\n"
            "    l,r=les.get_level_viewport_camera_info()\n"
            "    st['t'].append(time.perf_counter()); st['cam'].append((l.x,l.y,l.z))\n"
            "    if MV is not None:\n"
            "        rr=unreal.Rotator(pitch=r.pitch, yaw=r.yaw+(MV[3] if len(MV)>3 else 0.0), roll=r.roll)\n"
            "        les.set_level_viewport_camera_info(unreal.Vector(l.x+MV[0],l.y+MV[1],l.z+MV[2]), rr)\n"
            "    st['n']+=1\n"
            "    if st['n']==PRE:\n"
            "        unreal.SystemLibrary.execute_console_command(None, 'r.DumpingMovie %%d' %% N)\n"
            "    if st['n']>=PRE+N+3:\n"
            "        unreal.unregister_slate_post_tick_callback(unreal._diag34_h)\n"
            "        open(r'%s','w').write(json.dumps(st))\n"
            "unreal._diag34_h=unreal.register_slate_post_tick_callback(_tick)\nprint('ARMED')" % (n, move, 12 if move else 1, tj))
    py(code)
    t0 = time.time()
    while time.time() - t0 < timeout:
        if len(_frames() - before) >= n and os.path.isfile(tj):
            break
        time.sleep(1.0)
    time.sleep(1.0)
    cmd("r.BufferVisualizationDumpFrames 0"); cmd("r.DumpingMovie 0")
    new = sorted(_frames() - before)
    for i, f in enumerate(new):
        shutil.move(f, os.path.join(out, "f%03d.png" % i))
    for f in glob.glob(os.path.join(SS, "MovieFrame*_*.png")):
        try: os.remove(f)
        except Exception: pass
    print("CAPTURE %s: %d frames%s" % (name, len(new), "" if len(new) == n else " (WANTED %d)" % n), flush=True)
    return out

def restore_overview():
    cmd("r.BufferVisualizationOverviewTargets " + DEFAULT_OVERVIEW)

def load(dirname):
    from PIL import Image
    import numpy as np
    fs = sorted(glob.glob(os.path.join(M, dirname, "f*.png")))
    return np.stack([np.asarray(Image.open(f).convert("RGB"), dtype=np.float64) for f in fs])
