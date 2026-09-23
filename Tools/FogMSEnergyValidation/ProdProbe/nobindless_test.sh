#!/bin/bash
# Injection-only check in an editor launched WITHOUT -BindlessAll: Production tier + Emissive Injection at a fixed
# animation phase (manual time 100 s, same as refdump.sh), status text, screenshot PSNR against measure/ref_bindlessall.
# usage: FOGMS_LOG=<editor log> nobindless_test.sh
S="$(cd "$(dirname "$0")" && pwd)"; M="$S/measure"
PY() { python "$S/uemcp.py" py "$1" | grep -o '"output": "[^"]*"' | head -1; }
BOX="b=[a for a in unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors() if a.get_actor_label()=='FogMS - Live Box'][0]"
python "$S/uemcp.py" cmd "r.FogMS.Transport.AsyncCompute 0" >/dev/null
PY "import unreal
$BOX
print('BEFORE', b.get_editor_property('spatial_status'))"
PY "import unreal
$BOX
b.call_method('EnableLiveBox'); print('ENABLE_LIVE_BOX', b.get_editor_property('spatial_status'))"
sleep 3
PY "import unreal
$BOX
b.set_editor_property('manual_animation_time', 100.0); b.set_editor_property('use_manual_animation_time', True)
b.set_editor_property('transport_preset', unreal.FogMSTransportPreset.PRODUCTION); b.set_editor_property('emissive_injection', False); b.update_density(); print('INJ_OFF')"
sleep 3
PY "import unreal
$BOX
print('STATUS_INJ_OFF', b.get_editor_property('spatial_status'))"
PY "import unreal
$BOX
b.set_editor_property('emissive_injection', True); b.update_density(); print('INJ_ON')"
sleep 3
PY "import unreal
$BOX
print('STATUS_INJ_ON', b.get_editor_property('spatial_status'), '|', b.get_editor_property('sun_shadow_status'))"
rm -rf "$M/nobindless_inj" 2>/dev/null
python "$S/measure.py" nobindless_inj quality=LOW16 iterations=16 settle=10 2>&1 | grep "^==" | cut -c1-170
python "$S/uemcp.py" cmd "r.FogMS.Transport.AsyncCompute 1" >/dev/null; sleep 3
PY "import unreal
$BOX
print('STATUS_ASYNC', b.get_editor_property('spatial_status'))"
rm -rf "$M/nobindless_inj_async" 2>/dev/null
python "$S/measure.py" nobindless_inj_async quality=LOW16 iterations=16 settle=10 2>&1 | grep "^==" | cut -c1-170
python "$S/uemcp.py" cmd "r.FogMS.Transport.AsyncCompute 0" >/dev/null
echo "--- screenshot PSNR vs BindlessAll reference (box front face)"
python - <<'PYEOF'
from PIL import Image; import numpy as np, os
M = os.path.join(os.getcwd(), "measure")
def load(t): return np.asarray(Image.open(os.path.join(M, t, "shot.png")).convert('RGB'), dtype=np.float64)[140:700, 430:1340]
R = load("ref_bindlessall")
for t in ("nobindless_inj", "nobindless_inj_async"):
    try:
        A = load(t); mse = ((A - R) ** 2).mean()
        print("%-22s PSNR %.1f dB | mean ratio %.4f" % (t, 10 * np.log10(255 ** 2 / mse), A.mean() / R.mean()))
    except Exception as e:
        print(t, "ERR", e)
PYEOF
if [ -n "$FOGMS_LOG" ]; then
  echo "--- log: bindless summary / errors"
  grep -i "LogFogMSRHICompatibility\|Bindless" "$FOGMS_LOG" | grep -iv "LogConfig\|Cmd:" | head -6 | cut -c1-220
  grep -ci "Ensure condition failed" "$FOGMS_LOG"
  grep -i "Ensure condition failed\|LogShaderCompilers: Error\|LogShaderCompilers: Warning: Failed\|error X\|DEVICE_REMOVED\|Fatal error\|LogMultiLobeSpec: Error\|LogRDG" "$FOGMS_LOG" | head -8 | cut -c1-220
fi
PY "import unreal
$BOX
b.set_editor_property('use_manual_animation_time', False); b.set_editor_property('emissive_injection', False); b.set_editor_property('transport_preset', unreal.FogMSTransportPreset.CUSTOM); b.set_editor_property('angular_quality', unreal.FogMSAngularQuality.HIGH96); b.set_editor_property('transport_iterations', 16); b.set_editor_property('transport_tolerance', -1.0); b.update_density(); print('RESTORED')"
