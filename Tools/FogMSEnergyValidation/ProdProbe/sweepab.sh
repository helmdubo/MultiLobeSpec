#!/bin/bash
# A/B: AngularSweepCS group width (r.FogMS.Transport.SweepThreads 256/512/1024) on frozen density, Production tier (16 dirs, <=16 it, tol 1e-6).
S="$(cd "$(dirname "$0")" && pwd)"; M="$S/measure"
U() { python "$S/uemcp.py" "$@" >/dev/null; }
PY() { python "$S/uemcp.py" py "$1" | grep -o '"output": "[^"]*"' | head -1; }
BOX="b=[a for a in unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors() if a.get_actor_label()=='FogMS - Live Box'][0]"
MEAS() { python "$S/measure.py" "$@" 2>&1 | grep "^==" | sed 's/| metrics.*"maxRelativeCellResidual": \([0-9.e-]*\).*/| maxres \1/' | cut -c1-170; }
rm -rf "$M"/sweep_* 2>/dev/null
PY "import unreal
$BOX
b.freeze_density_animation(); b.set_editor_property('transport_preset', unreal.FogMSTransportPreset.PRODUCTION); b.update_density(); print('FROZEN', b.get_editor_property('spatial_status'))"
U cmd "r.FogMS.Transport.WarmStart 1"; U cmd "r.FogMS.Transport.SunAligned 1"
for T in 256 512 1024 256; do
  U cmd "r.FogMS.Transport.SweepThreads $T"; sleep 1
  MEAS sweep_$T quality=LOW16 iterations=16 settle=8
  python "$S/gpuprofile.py" sweep_$T --parse-only | grep "ordinate wavefronts" | head -3
done
echo "--- field check (must be identical)"; python "$S/fielddiff.py" "$M/sweep_1024/dump.rgba32f" "$M/sweep_256/dump.rgba32f"
U cmd "r.FogMS.Transport.SweepThreads 256"
PY "import unreal
$BOX
b.set_editor_property('transport_preset', unreal.FogMSTransportPreset.CUSTOM); b.set_editor_property('angular_quality', unreal.FogMSAngularQuality.HIGH96); b.set_editor_property('transport_iterations', 16); b.set_editor_property('transport_tolerance', -1.0); b.resume_density_animation(); b.update_density(); print('RESTORED', b.get_editor_property('spatial_status'))"
