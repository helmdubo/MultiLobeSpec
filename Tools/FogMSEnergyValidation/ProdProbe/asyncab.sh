#!/bin/bash
# A/B: r.FogMS.Transport.AsyncCompute 0 vs 1 on frozen density, Production tier (16 dirs, <=16 it, tol 1e-6), overlay delivery.
# Optional: INJ=1 repeats the pair with Emissive Injection on.
S="$(cd "$(dirname "$0")" && pwd)"; M="$S/measure"
U() { python "$S/uemcp.py" "$@" >/dev/null; }
PY() { python "$S/uemcp.py" py "$1" | grep -o '"output": "[^"]*"' | head -1; }
BOX="b=[a for a in unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors() if a.get_actor_label()=='FogMS - Live Box'][0]"
MEAS() { python "$S/measure.py" "$@" 2>&1 | grep "^==" | sed 's/| metrics.*"maxRelativeCellResidual": \([0-9.e-]*\).*/| maxres \1/' | cut -c1-170; }
rm -rf "$M"/async_* 2>/dev/null
PY "import unreal
$BOX
b.freeze_density_animation(); b.set_editor_property('transport_preset', unreal.FogMSTransportPreset.PRODUCTION); b.set_editor_property('emissive_injection', False); b.update_density(); print('FROZEN')"
U cmd "r.FogMS.Transport.WarmStart 1"; U cmd "r.FogMS.Transport.SunAligned 1"; U cmd "r.FogMS.Transport.SweepThreads 1024"
U cmd "r.SkyLight.RealTimeReflectionCapture 0"  # frozen sky for the A/B (noise floor otherwise ~1 %); restored below
for INJMODE in 0 ${INJ:+1}; do
  PY "import unreal
$BOX
b.set_editor_property('emissive_injection', bool($INJMODE)); b.update_density(); print('INJ $INJMODE')"
  for A in 0 1 0; do
    U cmd "r.FogMS.Transport.AsyncCompute $A"; sleep 2
    MEAS async_i${INJMODE}_a$A quality=LOW16 iterations=16 settle=8
    python "$S/gpuprofile.py" async_i${INJMODE}_a$A --parse-only | grep -i "async\|ComputeVolumetricFog\|top 10" | head -6
    PY "import unreal
$BOX
print('STATUS', b.get_editor_property('spatial_status'))"
  done
  echo "--- field check i$INJMODE (async 1 vs 0)"; python "$S/fielddiff.py" "$M/async_i${INJMODE}_a1/dump.rgba32f" "$M/async_i${INJMODE}_a0/dump.rgba32f"
done
U cmd "r.FogMS.Transport.AsyncCompute 0"
PY "import unreal
$BOX
b.set_editor_property('emissive_injection', False); b.set_editor_property('transport_preset', unreal.FogMSTransportPreset.CUSTOM); b.set_editor_property('angular_quality', unreal.FogMSAngularQuality.HIGH96); b.set_editor_property('transport_iterations', 16); b.set_editor_property('transport_tolerance', -1.0); b.resume_density_animation(); b.update_density(); print('RESTORED')"
U cmd "r.SkyLight.RealTimeReflectionCapture 1"
