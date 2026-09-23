#!/bin/bash
# Reference field for cross-session comparison: fixed manual animation time, Production tier, injection ON, async OFF.
# usage: refdump.sh <label>   -> measure/<label>/dump.rgba32f + shot.png
S="$(cd "$(dirname "$0")" && pwd)"; L=${1:-ref}
PY() { python "$S/uemcp.py" py "$1" | grep -o '"output": "[^"]*"' | head -1; }
BOX="b=[a for a in unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors() if a.get_actor_label()=='FogMS - Live Box'][0]"
python "$S/uemcp.py" cmd "r.FogMS.Transport.AsyncCompute 0" >/dev/null; python "$S/uemcp.py" cmd "r.FogMS.Transport.SweepThreads 1024" >/dev/null
PY "import unreal
$BOX
b.set_editor_property('manual_animation_time', 100.0); b.set_editor_property('use_manual_animation_time', True)
b.set_editor_property('transport_preset', unreal.FogMSTransportPreset.PRODUCTION); b.set_editor_property('emissive_injection', True); b.update_density(); print('FIXED_PHASE')"
python "$S/measure.py" "$L" quality=LOW16 iterations=16 settle=10 2>&1 | grep "^==" | cut -c1-170
PY "import unreal
$BOX
print('STATUS', b.get_editor_property('spatial_status'))"
PY "import unreal
$BOX
b.set_editor_property('use_manual_animation_time', False); b.set_editor_property('emissive_injection', False); b.set_editor_property('transport_preset', unreal.FogMSTransportPreset.CUSTOM); b.set_editor_property('angular_quality', unreal.FogMSAngularQuality.HIGH96); b.set_editor_property('transport_iterations', 16); b.set_editor_property('transport_tolerance', -1.0); b.update_density(); print('RESTORED')"
