#!/bin/bash
# Field identity of the installed round against the round-25 reference dump measure/sky_rtcFalse_s0 (skyab.sh, RTC off,
# SkySource 0 = static capture, overlay delivery, Production, frozen density t=100, DirectSamples 8, SolveInterval 1,
# async off). Proves that S0 (32-row packet), S1/S2 defaults and the multi-Box runtime leave the single-Box field unchanged.
# Prints fielddiff vs the reference and a same-round repeat (noise floor). Restores RTC flag and the actor snapshot.
# usage: identity_check.sh [reference dump dir]   (default measure/sky_rtcFalse_s0)
S="$(cd "$(dirname "$0")" && pwd)"; M="$S/measure"; REF=${1:-$M/sky_rtcFalse_s0}
[ -f "$REF/dump.rgba32f" ] || { echo "NO_REFERENCE $REF"; exit 1; }
U() { python "$S/uemcp.py" "$@" >/dev/null; }
PY() { python "$S/uemcp.py" py "$1" | grep -o '"output": "[^"]*"' | head -1; }
BOX="b=[a for a in unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors() if a.get_actor_label()=='FogMS - Live Box'][0]"
SKY="sk=[a for a in unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors() if isinstance(a, unreal.SkyLight)][0]; c=sk.get_component_by_class(unreal.SkyLightComponent)"
MEAS() { python "$S/measure.py" "$@" 2>&1 | grep "^==" | sed 's/| metrics.*"maxRelativeCellResidual": \([0-9.e-]*\).*/| maxres \1/' | cut -c1-170; }
python "$S/boxstate.py" save "$M/identity_state.json" | cut -c1-120
RTC0=$(PY "import unreal
$SKY
print('RTC0', c.get_editor_property('real_time_capture'))" | grep -o "RTC0 [A-Za-z]*" | cut -d' ' -f2); echo "SkyLight real_time_capture before: $RTC0"
rm -rf "$M"/ident_* 2>/dev/null
for c in "r.FogMS.Transport.WarmStart 1" "r.FogMS.Transport.SunAligned 1" "r.FogMS.Transport.SweepThreads 1024" "r.FogMS.Transport.AsyncCompute 0" \
         "r.FogMS.Transport.SolveInterval 1" "r.FogMS.World.SkyLutSamples 5" "r.FogMS.World.SkySource 0" "r.FogMS.Transport.DirectSamples 8" \
         "r.FogMS.Transport.DirectSkipEmpty 0" "r.FogMS.World.FallbackMedium 1" "r.FogMS.MaxBoxesPerFrame 4"; do U cmd "$c"; done
PY "import unreal
$SKY
c.set_editor_property('real_time_capture', False); print('RTC off', c.get_editor_property('real_time_capture'))"
PY "import unreal
$BOX
b.set_editor_property('manual_animation_time', 100.0); b.set_editor_property('use_manual_animation_time', True)
b.set_editor_property('transport_preset', unreal.FogMSTransportPreset.PRODUCTION); b.set_editor_property('emissive_injection', False); b.set_editor_property('hybrid_single_scattering', False)
for p, v in (('erosion_strength', 0.0), ('height_profile', False)):
    try: b.set_editor_property(p, v)
    except Exception as e: print('no prop', p, e)
b.update_density(); print('FROZEN overlay, defaults:', b.get_editor_property('spatial_status'))"
sleep 8
MEAS ident_a quality=LOW16 iterations=16 settle=8
MEAS ident_b quality=LOW16 iterations=16 settle=6
echo "--- field vs round-25 reference ($REF)"
printf "a    "; python "$S/fielddiff.py" "$M/ident_a/dump.rgba32f" "$REF/dump.rgba32f"
printf "b    "; python "$S/fielddiff.py" "$M/ident_b/dump.rgba32f" "$REF/dump.rgba32f"
echo "--- noise floor (a vs b, same round)"
printf "a/b  "; python "$S/fielddiff.py" "$M/ident_a/dump.rgba32f" "$M/ident_b/dump.rgba32f"
echo "--- erosion 0.5 / stratus profile smoke (field must CHANGE; status must stay Active)"
PY "import unreal
$BOX
try:
    b.set_editor_property('erosion_strength', 0.5); b.update_density(); print('erosion set')
except Exception as e: print('erosion prop missing', e)"
sleep 4; MEAS ident_erosion quality=LOW16 iterations=16 settle=6
printf "erosion vs a "; python "$S/fielddiff.py" "$M/ident_erosion/dump.rgba32f" "$M/ident_a/dump.rgba32f"
PY "import unreal
$BOX
try:
    b.set_editor_property('erosion_strength', 0.0); b.set_editor_property('height_profile_preset', unreal.FogMSHeightProfilePreset.STRATUS); b.update_density(); print('stratus set', b.get_editor_property('height_bottom'), b.get_editor_property('height_top'))
except Exception as e: print('profile prop missing', e)"
sleep 4; MEAS ident_stratus quality=LOW16 iterations=16 settle=6
printf "stratus vs a "; python "$S/fielddiff.py" "$M/ident_stratus/dump.rgba32f" "$M/ident_a/dump.rgba32f"
PY "import unreal
$BOX
try:
    b.set_editor_property('height_profile_preset', unreal.FogMSHeightProfilePreset.NONE); b.set_editor_property('height_profile', False); b.update_density(); print('profile off')
except Exception as e: print('profile reset failed', e)"
U cmd "r.FogMS.Transport.DirectSamples 4"; U cmd "r.FogMS.Transport.SolveInterval 2"
PY "import unreal
$SKY
c.set_editor_property('real_time_capture', $RTC0); print('RTC restored', c.get_editor_property('real_time_capture'))"
python "$S/boxstate.py" restore "$M/identity_state.json" | cut -c1-200
