# -*- coding: utf-8 -*-
"""Multi-Box smoke test (round 29+, editor world, injection path). Steps:
 1. snapshot the Live Box (boxstate.py), freeze density, Production + Emissive Injection + hybrid;
 2. duplicate it twice along +X ('FogMS - Test Box 2', 'FogMS - Test Box 3', each 1.25 x width apart), injection on;
 3. poll the three statuses (expect 'Active ... [Box #N...]' on all three with the default budget 4);
 4. r.FogMS.MaxBoxesPerFrame 1 -> expect 'Queued' / hold on the non-packet Boxes, then back to 4;
 5. disable Box 2 (bEnabled False) -> its status leaves Active, the others stay Active;
 6. destroy Test Boxes 2 and 3 -> the Live Box stays Active (single-Box path);
 7. HighResShot at step 3 and 6 (measure/multibox_*.png), log scan, restore the Live Box snapshot.
Usage: FOGMS_LOG=<editor log> python multibox_test.py      (editor must be free: no owner work while this runs)"""
import sys, os, time, subprocess, json, re, shutil
HERE = os.path.dirname(os.path.abspath(__file__))
M = os.path.join(HERE, "measure")
LOG = os.environ.get("FOGMS_LOG", "")
EAS = "eas=unreal.get_editor_subsystem(unreal.EditorActorSubsystem)"
LIVE = "FogMS - Live Box"
T2, T3 = "FogMS - Test Box 2", "FogMS - Test Box 3"

def py(code):
    r = subprocess.run([sys.executable, os.path.join(HERE, "uemcp.py"), "py", code], capture_output=True, text=True, encoding="utf-8", errors="replace")
    try:
        j = json.loads(r.stdout)
        return ((j.get("result") or {}).get("output", "") or str(j.get("error", ""))).strip()
    except Exception:
        return r.stdout[-400:]

def cmd(c):
    subprocess.run([sys.executable, os.path.join(HERE, "uemcp.py"), "cmd", c], capture_output=True)

def statuses(tag):
    code = ("import unreal\n%s\nfor lab in ['%s','%s','%s']:\n    L=[a for a in eas.get_all_level_actors() if a.get_actor_label()==lab]\n"
            "    print('%s |', lab, '|', (L[0].get_editor_property('spatial_status') if L else 'MISSING'))" % (EAS, LIVE, T2, T3, tag))
    out = py(code)
    for line in out.splitlines():
        print(line[:230])
    return out

def wait_for(pred, tag, tries=8, dt=2):
    out = ""
    for i in range(tries):
        time.sleep(dt)
        out = statuses("%s t=%ds" % (tag, dt * (i + 1)))
        if pred(out):
            break
    return out

def shot(name):
    cmd("HighResShot 1600x900")
    time.sleep(5)
    if LOG:
        m = re.findall(r"Screenshot saved to (.*?\.png)", open(LOG, encoding="utf-8", errors="replace").read())
        if m:
            src = m[-1].strip()
            if os.path.isfile(src):
                os.makedirs(M, exist_ok=True)
                dst = os.path.join(M, "multibox_%s.png" % name)
                shutil.copy(src, dst)
                print("SHOT", dst)

def dup(label, k):
    # Duplicate the Live Box along its local +X by k x (1.25 x width); the copy keeps material/preset/injection.
    return py("import unreal\n%s\n%s\n"
              "src=[a for a in eas.get_all_level_actors() if a.get_actor_label()=='%s'][0]\n"
              "old=[a for a in eas.get_all_level_actors() if a.get_actor_label()=='%s']\n"
              "for a in old: eas.destroy_actor(a)\n"
              "o,e=src.get_actor_bounds(False)\n"
              "off=src.get_actor_forward_vector()*(e.x*2.0*1.25*%d)\n"
              "d=eas.duplicate_actor(src, src.get_world(), off)\n"
              "d.set_actor_label('%s')\n"
              "d.set_editor_property('emissive_injection', True); d.set_editor_property('enabled', True); d.update_density()\n"
              "print('DUP', d.get_actor_label(), d.get_actor_location(), '| src', src.get_actor_location())" % (EAS, LIVE, label, k, label))

def log_scan(tag):
    if not LOG or not os.path.isfile(LOG):
        return
    txt = open(LOG, encoding="utf-8", errors="replace").read()
    bad = [l for l in txt.splitlines() if re.search(r"Ensure condition failed|Fatal error|DEVICE_REMOVED|LogFogMSTransport: Error|LogMultiLobeSpec: Error|RDG", l) and "EditorViewportClient" not in l]
    print("LOGSCAN %s | %d suspicious lines" % (tag, len(bad)))
    for l in bad[-6:]:
        print("   ", l[:200])

snap = os.path.join(M, "multibox_state.json")
print(subprocess.run([sys.executable, os.path.join(HERE, "boxstate.py"), "save", snap], capture_output=True, text=True).stdout.strip()[:160])
print("SETUP |", py("import unreal\n%s\nb=[a for a in eas.get_all_level_actors() if a.get_actor_label()=='%s'][0]\n"
                     "b.set_editor_property('manual_animation_time', 100.0); b.set_editor_property('use_manual_animation_time', True)\n"
                     "b.set_editor_property('transport_preset', unreal.FogMSTransportPreset.PRODUCTION); b.set_editor_property('emissive_injection', True); b.set_editor_property('hybrid_single_scattering', True); b.update_density()\n"
                     "print('live box ready', b.get_editor_property('spatial_status'))" % (EAS, LIVE))[:220])
cmd("r.FogMS.MaxBoxesPerFrame 4"); cmd("r.SkyLight.RealTimeReflectionCapture 0")
wait_for(lambda o: o.count("Active") >= 1, "one-box", tries=5)
print(dup(T2, 1)[:200]); print(dup(T3, 2)[:200])
out = wait_for(lambda o: o.count("Active") >= 3, "three-boxes", tries=10)
print("RESULT three Active:", out.count("Active") >= 3)
shot("three")
log_scan("three")
print("=== budget 1")
cmd("r.FogMS.MaxBoxesPerFrame 1")
out = wait_for(lambda o: ("Queued" in o) or (o.count("hold") >= 1), "budget1", tries=6)
print("RESULT queued/hold seen:", ("Queued" in out) or ("hold" in out))
cmd("r.FogMS.MaxBoxesPerFrame 4")
wait_for(lambda o: o.count("Active") >= 3, "budget4-again", tries=6)
print("=== disable Box 2")
print(py("import unreal\n%s\nd=[a for a in eas.get_all_level_actors() if a.get_actor_label()=='%s'][0]\nd.set_editor_property('enabled', False); d.update_density(); print('box2 disabled')" % (EAS, T2))[:120])
out = wait_for(lambda o: o.count("Active") == 2, "box2-off", tries=6)
print("RESULT two Active after disable:", out.count("Active") == 2)
print("=== destroy test boxes")
print(py("import unreal\n%s\nfor lab in ['%s','%s']:\n    for a in [a for a in eas.get_all_level_actors() if a.get_actor_label()==lab]: eas.destroy_actor(a)\nprint('destroyed')" % (EAS, T2, T3))[:120])
out = wait_for(lambda o: o.count("Active") == 1 and o.count("MISSING") == 2, "single-again", tries=6)
print("RESULT single Box Active:", out.count("Active") == 1)
shot("single")
log_scan("end")
cmd("r.SkyLight.RealTimeReflectionCapture 1")
print(subprocess.run([sys.executable, os.path.join(HERE, "boxstate.py"), "restore", snap], capture_output=True, text=True).stdout.strip()[:200])
