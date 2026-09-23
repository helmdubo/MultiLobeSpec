#!/bin/bash
# A/B of r.FogMS.World.SkySource against the auto choice (0) on frozen density (the private path was removed in round 27), Production tier, overlay delivery
# (resident atlas dump). Two SkyLight configurations: Real Time Capture ON (auto = Sky View LUT) and OFF (auto = processed
# capture). For each: auto (0), forced LUT (2), forced capture (3), forced SH (4), auto again (0b = noise floor); fielddiff vs auto, PSNR
# inside the Box, status text. r.SkyLight.RealTimeReflectionCapture is left ON during the RTC runs (the LUT path needs the
# live atmosphere; noise floor is reported as 0 vs 0b). Restores SkyLight RTC flag and the actor from snapshots.
S="$(cd "$(dirname "$0")" && pwd)"; M="$S/measure"
U() { python "$S/uemcp.py" "$@" >/dev/null; }
PY() { python "$S/uemcp.py" py "$1" | grep -o '"output": "[^"]*"' | head -1; }
BOX="b=[a for a in unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors() if a.get_actor_label()=='FogMS - Live Box'][0]"
SKY="sk=[a for a in unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors() if isinstance(a, unreal.SkyLight)][0]; c=sk.get_component_by_class(unreal.SkyLightComponent)"
MEAS() { python "$S/measure.py" "$@" 2>&1 | grep "^==" | sed 's/| metrics.*"maxRelativeCellResidual": \([0-9.e-]*\).*/| maxres \1/' | cut -c1-170; }
STATUS() { PY "import unreal
$BOX
print('$1', b.get_editor_property('spatial_status'))"; }
python "$S/boxstate.py" save "$M/skyab_state.json" | cut -c1-120
RTC0=$(PY "import unreal
$SKY
print('RTC0', c.get_editor_property('real_time_capture'))" | grep -o "RTC0 [A-Za-z]*" | cut -d' ' -f2); echo "SkyLight real_time_capture before: $RTC0"
rm -rf "$M"/sky_* 2>/dev/null
U cmd "r.FogMS.Transport.WarmStart 1"; U cmd "r.FogMS.Transport.SunAligned 1"; U cmd "r.FogMS.Transport.SweepThreads 1024"; U cmd "r.FogMS.Transport.AsyncCompute 0"; U cmd "r.FogMS.Transport.SolveInterval 1"; U cmd "r.FogMS.World.SkyLutSamples 5"
PY "import unreal
$BOX
b.set_editor_property('manual_animation_time', 100.0); b.set_editor_property('use_manual_animation_time', True)
b.set_editor_property('transport_preset', unreal.FogMSTransportPreset.PRODUCTION); b.set_editor_property('emissive_injection', False); b.set_editor_property('hybrid_single_scattering', False); b.update_density(); print('FROZEN overlay')"
for RTC in True False; do
  PY "import unreal
$SKY
c.set_editor_property('real_time_capture', $RTC); print('RTC set', c.get_editor_property('real_time_capture'))"
  sleep 6
  for R in 0 2 3 4 0b; do
    SRC=${R%b}
    U cmd "r.FogMS.World.SkySource $SRC"; sleep 4
    MEAS sky_rtc${RTC}_s$R quality=LOW16 iterations=16 settle=8; STATUS "RTC=$RTC SRC=$R"
  done
  echo "--- fields vs auto (0), RTC=$RTC"
  for R in 0b 2 3 4; do printf "%-4s " "$R"; python "$S/fielddiff.py" "$M/sky_rtc${RTC}_s$R/dump.rgba32f" "$M/sky_rtc${RTC}_s0/dump.rgba32f"; done
  M="$M" RTC="$RTC" python - <<'PYEOF'
from PIL import Image; import numpy as np, os
M = os.environ["M"]; RTC = os.environ["RTC"]
def load(t): return np.asarray(Image.open(os.path.join(M, t, "shot.png")).convert('RGB'), dtype=np.float64)[140:700, 430:1340]
try:
    R = load("sky_rtc%s_s0" % RTC)
    for t in ("0b", "2", "3", "4"):
        A = load("sky_rtc%s_s%s" % (RTC, t)); mse = ((A - R) ** 2).mean()
        print("screenshot RTC=%s source %-2s vs auto: PSNR %5.1f dB | mean ratio %.4f" % (RTC, t, 10 * np.log10(255 ** 2 / max(mse, 1e-9)), A.mean() / R.mean()))
except Exception as e:
    print("compare ERR", e)
PYEOF
done
U cmd "r.FogMS.World.SkySource 0"
PY "import unreal
$SKY
c.set_editor_property('real_time_capture', $RTC0); print('RTC restored', c.get_editor_property('real_time_capture'))"
python "$S/boxstate.py" restore "$M/skyab_state.json" | cut -c1-200
