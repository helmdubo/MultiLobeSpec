#!/bin/bash
# A/B: overlay delivery vs emissive-injection delivery of the same transport field (frozen density, 16 dirs, adaptive budget).
S="$(cd "$(dirname "$0")" && pwd)"; M="$S/measure"
U() { python "$S/uemcp.py" "$@" >/dev/null; }
PY() { python "$S/uemcp.py" py "$1" | grep -o '"output": "[^"]*"' | head -1; }
BOX="b=[a for a in unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors() if a.get_actor_label()=='FogMS - Live Box'][0]"
MEAS() { python "$S/measure.py" "$@" 2>&1 | grep "^==" | sed 's/| metrics.*"maxRelativeCellResidual": \([0-9.e-]*\).*/| maxres \1/' | cut -c1-170; }
rm -rf "$M"/inj_* 2>/dev/null
PY "import unreal
$BOX
b.freeze_density_animation(); b.update_density(); print('FROZEN')"
U cmd "r.FogMS.Transport.Tolerance 0.000001"; U cmd "r.FogMS.Transport.WarmStart 1"; U cmd "r.FogMS.Transport.SunAligned 1"
PY "import unreal
$BOX
b.set_editor_property('emissive_injection', False); b.update_density(); print('OVERLAY')"
MEAS inj_off_overlay quality=LOW16 iterations=16 settle=8
PY "import unreal
$BOX
b.set_editor_property('emissive_injection', True); b.update_density(); print('INJECTION', b.get_editor_property('spatial_status'))"
sleep 2
MEAS inj_on_material quality=LOW16 iterations=16 settle=8
PY "import unreal
$BOX
print('STATUS', b.get_editor_property('spatial_status'), '|', b.get_editor_property('sun_shadow_status'))"
echo "--- image comparison (PSNR, injection vs overlay)"; python "$S/compare.py" inj_off_overlay 2>/dev/null | grep "inj_"
echo "--- field comparison (should be identical field, same solver)"; python "$S/fielddiff.py" "$M/inj_on_material/dump.rgba32f" "$M/inj_off_overlay/dump.rgba32f"
U cmd "r.FogMS.Transport.Tolerance 1e-14"
PY "import unreal
$BOX
b.set_editor_property('emissive_injection', False); b.set_editor_property('angular_quality', unreal.FogMSAngularQuality.HIGH96); b.set_editor_property('transport_iterations', 16); b.resume_density_animation(); b.update_density(); print('RESTORED')"
