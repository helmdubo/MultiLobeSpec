#!/bin/bash
# A/B of r.FogMS.Transport.PublicHitFlags (1 = CastShadow flags rebuilt from public ray-tracing bindings, 0 = private
# FViewInfo buffer) on frozen density, Production tier, overlay delivery (so FogMS.DumpSpatial dumps the resident atlas).
# Expected: identical field unless the scene has dynamic meshes with ray-traced shadows off. Needs BindlessAll editor,
# FOGMS_LOG and a free editor; restores the actor from a snapshot.
# Runs 1a / 0 / 1b: 1a vs 1b is the noise floor of the same configuration. r.FogMS.Transport.HitFlagsDebug 2 is on for all
# three runs (both buffers built, compared every r.FogMS.Transport.HitFlagsDebugInterval frames; the selected buffer still
# shadows, so the field is unaffected, but timings include the debug work); the last 'HitFlagsDebug' log block of each run is printed.
S="$(cd "$(dirname "$0")" && pwd)"; M="$S/measure"
U() { python "$S/uemcp.py" "$@" >/dev/null; }
PY() { python "$S/uemcp.py" py "$1" | grep -o '"output": "[^"]*"' | head -1; }
BOX="b=[a for a in unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors() if a.get_actor_label()=='FogMS - Live Box'][0]"
MEAS() { python "$S/measure.py" "$@" 2>&1 | grep "^==" | sed 's/| metrics.*"maxRelativeCellResidual": \([0-9.e-]*\).*/| maxres \1/' | cut -c1-170; }
python "$S/boxstate.py" save "$M/hitflags_state.json" | cut -c1-120
rm -rf "$M"/hf_* 2>/dev/null
U cmd "r.FogMS.Transport.WarmStart 1"; U cmd "r.FogMS.Transport.SunAligned 1"; U cmd "r.FogMS.Transport.SweepThreads 1024"; U cmd "r.FogMS.Transport.AsyncCompute 0"; U cmd "r.FogMS.Transport.SolveInterval 1"
U cmd "r.SkyLight.RealTimeReflectionCapture 0"  # frozen sky for the A/B (noise floor otherwise ~1 %); restored below
PY "import unreal
$BOX
b.set_editor_property('manual_animation_time', 100.0); b.set_editor_property('use_manual_animation_time', True)
b.set_editor_property('transport_preset', unreal.FogMSTransportPreset.PRODUCTION); b.set_editor_property('emissive_injection', False); b.set_editor_property('hybrid_single_scattering', False); b.update_density(); print('FROZEN overlay')"
# Last 'HitFlagsDebug' block (from the last 'HitFlagsDebug frame' header on) among the log lines after line $1.
HFLOG() {
  if [ -z "$FOGMS_LOG" ] || [ ! -f "$FOGMS_LOG" ]; then echo "  (FOGMS_LOG not set or missing: no HitFlagsDebug lines)"; return; fi
  tail -n +"$(( $1 + 1 ))" "$FOGMS_LOG" | grep "HitFlagsDebug" | grep -v "Cmd:" | sed 's/^.*LogFogMSTransport: //' \
    | awk '/^HitFlagsDebug frame/{buf=""} {buf=buf $0 "\n"} END{printf "%s", buf}' | cut -c1-220
}
LOGLINES() { if [ -n "$FOGMS_LOG" ] && [ -f "$FOGMS_LOG" ]; then wc -l < "$FOGMS_LOG"; else echo 0; fi; }
U cmd "r.FogMS.Transport.HitFlagsDebug 2"
for R in 1a:1 0:0 1b:1; do
  L=${R%%:*}; F=${R##*:}; L0=$(LOGLINES)
  U cmd "r.FogMS.Transport.PublicHitFlags $F"; sleep 3
  MEAS hf_$L quality=LOW16 iterations=16 settle=8
  PY "import unreal
$BOX
print('RUN=$L FLAGS=$F', b.get_editor_property('spatial_status'))"
  echo "--- HitFlagsDebug log, run $L (PublicHitFlags $F)"; HFLOG "$L0"
done
U cmd "r.FogMS.Transport.HitFlagsDebug 0"
echo "--- noise floor: public (1a) vs public (1b)"; python "$S/fielddiff.py" "$M/hf_1a/dump.rgba32f" "$M/hf_1b/dump.rgba32f"
echo "--- field: public (1a) vs private (0)"; python "$S/fielddiff.py" "$M/hf_1a/dump.rgba32f" "$M/hf_0/dump.rgba32f"
echo "--- field: public (1b) vs private (0)"; python "$S/fielddiff.py" "$M/hf_1b/dump.rgba32f" "$M/hf_0/dump.rgba32f"
M="$M" python - <<'PYEOF'
from PIL import Image; import numpy as np, os
M = os.environ["M"]
def load(t): return np.asarray(Image.open(os.path.join(M, t, "shot.png")).convert('RGB'), dtype=np.float64)[140:700, 430:1340]
for a, b, what in (("hf_1a", "hf_1b", "noise floor public 1a vs 1b"), ("hf_1a", "hf_0", "public 1a vs private"), ("hf_1b", "hf_0", "public 1b vs private")):
    try:
        A = load(a); B = load(b); mse = ((A - B) ** 2).mean()
        print("screenshot %s: PSNR %.1f dB | mean ratio %.4f" % (what, 10 * np.log10(255 ** 2 / max(mse, 1e-9)), A.mean() / B.mean()))
    except Exception as e:
        print("compare ERR", what, e)
PYEOF
U cmd "r.FogMS.Transport.PublicHitFlags 0"
U cmd "r.SkyLight.RealTimeReflectionCapture 1"
python "$S/boxstate.py" restore "$M/hitflags_state.json" | cut -c1-200
