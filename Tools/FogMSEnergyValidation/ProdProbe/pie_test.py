# -*- coding: utf-8 -*-
"""Auto-start check for the injection-only Box: (1) editor world - set Production + Emissive Injection and wait for the
runtime to start by itself (no Enable Live Box call); (2) Simulate-in-Editor (BeginPlay) - read the game-world Box status
and take a screenshot; then end the session and restore the actor. Usage: python pie_test.py"""
import sys, os, time, subprocess, json
HERE = os.path.dirname(os.path.abspath(__file__))
BOX = "b=[a for a in unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors() if a.get_actor_label()=='FogMS - Live Box'][0]"

def py(code):
    r = subprocess.run([sys.executable, os.path.join(HERE, "uemcp.py"), "py", code], capture_output=True, text=True, encoding="utf-8", errors="replace")
    try:
        j = json.loads(r.stdout)
        return ((j.get("result") or {}).get("output", "") or str(j.get("error", ""))).strip()
    except Exception:
        return r.stdout[-300:]

def cmd(c):
    subprocess.run([sys.executable, os.path.join(HERE, "uemcp.py"), "cmd", c], capture_output=True)

print("EDITOR_BEFORE |", py("import unreal\n" + BOX + "\nprint(b.get_editor_property('spatial_status'))"))
print(py("import unreal\n" + BOX + "\nb.set_editor_property('manual_animation_time', 100.0); b.set_editor_property('use_manual_animation_time', True)\nb.set_editor_property('transport_preset', unreal.FogMSTransportPreset.PRODUCTION); b.set_editor_property('emissive_injection', True); b.update_density(); print('SET injection on, no EnableLiveBox call')"))
for i in range(6):
    time.sleep(2)
    s = py("import unreal\n" + BOX + "\nprint(b.get_editor_property('spatial_status'))")
    print("EDITOR t=%ds |" % (2 * (i + 1)), s[:170])
    if s.startswith("Active"):
        break
# Simulate in editor: actors get BeginPlay in the game world.
print("SIE start |", py("import unreal\nles=unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)\nles.editor_play_simulate()\nprint('requested')"))
time.sleep(8)
game = ("import unreal\nw=unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_game_world()\n"
        "print('GAME_WORLD', w.get_name() if w else None)\n"
        "if w:\n"
        "    L=[a for a in unreal.GameplayStatics.get_all_actors_of_class(w, unreal.FogMSBoxVolume)]\n"
        "    for a in L: print('PIE_BOX', a.get_name(), '|', a.get_editor_property('emissive_injection'), '|', a.get_editor_property('spatial_status'))\n")
for i in range(5):
    out = py(game)
    print("SIE t=%ds |" % (8 + 2 * i), out[:400].replace("\n", " || "))
    if "Active" in out:
        break
    time.sleep(2)
cmd("HighResShot 1600x900")
time.sleep(4)
print("SIE end |", py("import unreal\nles=unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)\nprint('in_pie', les.is_in_play_in_editor())\nles.editor_request_end_play()\nprint('end requested')"))
time.sleep(5)
print("RESTORE |", py("import unreal\n" + BOX + "\nb.set_editor_property('use_manual_animation_time', False); b.set_editor_property('emissive_injection', False); b.set_editor_property('transport_preset', unreal.FogMSTransportPreset.CUSTOM); b.set_editor_property('angular_quality', unreal.FogMSAngularQuality.HIGH96); b.set_editor_property('transport_iterations', 16); b.set_editor_property('transport_tolerance', -1.0); b.update_density(); print('RESTORED', b.get_editor_property('spatial_status'))"))
