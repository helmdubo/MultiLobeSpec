#!/bin/bash
# A/B of the pass-2 trims on frozen density, Production tier, injection+hybrid as authored, async ON (so the graphics-queue
# "FogMS transport/world" time is the RT/direct cost only) and SolveInterval 1: baseline (skip 0, samples 8),
# DirectSkipEmpty 1, DirectSamples 4, both. Field vs baseline via the resident atlas needs overlay delivery, so a second
# short block repeats the four configs with overlay for fielddiff. Flicker probe: two consecutive dumps at DirectSamples 4.
S="$(cd "$(dirname "$0")" && pwd)"; M="$S/measure"
U() { python "$S/uemcp.py" "$@" >/dev/null; }
PY() { python "$S/uemcp.py" py "$1" | grep -o '"output": "[^"]*"' | head -1; }
BOX="b=[a for a in unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors() if a.get_actor_label()=='FogMS - Live Box'][0]"
MEAS() { python "$S/measure.py" "$@" 2>&1 | grep "^==" | sed 's/| metrics.*"maxRelativeCellResidual": \([0-9.e-]*\).*/| maxres \1/' | cut -c1-170; }
python "$S/boxstate.py" save "$M/trims_state.json" | cut -c1-120
rm -rf "$M"/trim_* 2>/dev/null
U cmd "r.FogMS.Transport.WarmStart 1"; U cmd "r.FogMS.Transport.SunAligned 1"; U cmd "r.FogMS.Transport.SweepThreads 1024"; U cmd "r.FogMS.Transport.SolveInterval 1"
U cmd "r.SkyLight.RealTimeReflectionCapture 0"
PY "import unreal
$BOX
b.set_editor_property('manual_animation_time', 100.0); b.set_editor_property('use_manual_animation_time', True)
b.set_editor_property('transport_preset', unreal.FogMSTransportPreset.PRODUCTION); b.update_density(); print('FROZEN inj=%s hyb=%s' % (b.get_editor_property('emissive_injection'), b.get_editor_property('hybrid_single_scattering')))"
echo "=== graphics-queue cost (async on): baseline / skip / s4 / both"
U cmd "r.FogMS.Transport.AsyncCompute 1"
for R in base:0:8 skip:1:8 s4:0:4 both:1:4; do
  L=${R%%:*}; rest=${R#*:}; SK=${rest%%:*}; SM=${rest##*:}
  U cmd "r.FogMS.Transport.DirectSkipEmpty $SK"; U cmd "r.FogMS.Transport.DirectSamples $SM"; sleep 3
  MEAS trim_g_$L quality=LOW16 iterations=16 settle=8
  python "$S/gpuprofile.py" trim_g_${L}_b --parse-only | grep "direct cell average" | head -2 | sed 's/^/     /'
done
echo "=== field vs baseline (overlay delivery, async off)"
U cmd "r.FogMS.Transport.AsyncCompute 0"
PY "import unreal
$BOX
b.set_editor_property('emissive_injection', False); b.update_density(); print('OVERLAY')"
for R in base:0:8 skip:1:8 s4:0:4 s4b:0:4 both:1:4; do
  L=${R%%:*}; rest=${R#*:}; SK=${rest%%:*}; SM=${rest##*:}
  U cmd "r.FogMS.Transport.DirectSkipEmpty $SK"; U cmd "r.FogMS.Transport.DirectSamples $SM"; sleep 3
  MEAS trim_f_$L quality=LOW16 iterations=16 settle=6
done
for L in skip s4 s4b both; do printf "%-5s " "$L"; python "$S/fielddiff.py" "$M/trim_f_$L/dump.rgba32f" "$M/trim_f_base/dump.rgba32f"; done
echo "--- flicker probe: two consecutive DirectSamples 4 solves (s4 vs s4b; compare with the noise floor of skip runs if needed)"
python "$S/fielddiff.py" "$M/trim_f_s4/dump.rgba32f" "$M/trim_f_s4b/dump.rgba32f"
U cmd "r.FogMS.Transport.DirectSkipEmpty 0"; U cmd "r.FogMS.Transport.DirectSamples 8"; U cmd "r.FogMS.Transport.SolveInterval 2"
U cmd "r.SkyLight.RealTimeReflectionCapture 1"
python "$S/boxstate.py" restore "$M/trims_state.json" | cut -c1-200
