#!/bin/bash
# A/B of the Box option Lumen Bounce (Auto = Lumen surface cache at boundary hits, Off = public fallback
# albedo*(sun*shadow + SH sky)) on frozen density, Production tier, overlay delivery (resident atlas dump), sky capture
# frozen. Also a repeat of Auto as the noise floor. Restores the actor from a snapshot.
S="$(cd "$(dirname "$0")" && pwd)"; M="$S/measure"
U() { python "$S/uemcp.py" "$@" >/dev/null; }
PY() { python "$S/uemcp.py" py "$1" | grep -o '"output": "[^"]*"' | head -1; }
BOX="b=[a for a in unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors() if a.get_actor_label()=='FogMS - Live Box'][0]"
MEAS() { python "$S/measure.py" "$@" 2>&1 | grep "^==" | sed 's/| metrics.*"maxRelativeCellResidual": \([0-9.e-]*\).*/| maxres \1/' | cut -c1-170; }
STATUS() { PY "import unreal
$BOX
print('$1', b.get_editor_property('spatial_status'))"; }
python "$S/boxstate.py" save "$M/lumenab_state.json" | cut -c1-120
rm -rf "$M"/lum_* 2>/dev/null
U cmd "r.FogMS.Transport.WarmStart 1"; U cmd "r.FogMS.Transport.SunAligned 1"; U cmd "r.FogMS.Transport.SweepThreads 1024"; U cmd "r.FogMS.Transport.AsyncCompute 0"; U cmd "r.FogMS.Transport.SolveInterval 1"
U cmd "r.SkyLight.RealTimeReflectionCapture 0"
PY "import unreal
$BOX
b.set_editor_property('manual_animation_time', 100.0); b.set_editor_property('use_manual_animation_time', True)
b.set_editor_property('transport_preset', unreal.FogMSTransportPreset.PRODUCTION); b.set_editor_property('emissive_injection', False); b.set_editor_property('hybrid_single_scattering', False); b.update_density(); print('FROZEN overlay')"
for R in auto:AUTO off:OFF auto2:AUTO; do
  L=${R%%:*}; V=${R##*:}
  PY "import unreal
$BOX
b.set_editor_property('lumen_bounce', unreal.FogMSLumenBounce.$V); b.update_density(); print('SET $V')"
  sleep 4
  MEAS lum_$L quality=LOW16 iterations=16 settle=8; STATUS "LumenBounce=$V"
done
echo "--- noise floor: auto vs auto2"; python "$S/fielddiff.py" "$M/lum_auto/dump.rgba32f" "$M/lum_auto2/dump.rgba32f"
echo "--- field: off (fallback) vs auto (Lumen)"; python "$S/fielddiff.py" "$M/lum_off/dump.rgba32f" "$M/lum_auto/dump.rgba32f"
M="$M" python - <<'PYEOF'
from PIL import Image; import numpy as np, os
M = os.environ["M"]
def load(t): return np.asarray(Image.open(os.path.join(M, t, "shot.png")).convert('RGB'), dtype=np.float64)
try:
    A = load("lum_auto"); B = load("lum_off"); C = load("lum_auto2")
    for name, (y0, y1, x0, x1) in {"front face": (140, 700, 430, 1340), "core": (250, 550, 600, 1200), "floor": (820, 900, 0, 300)}.items():
        a = A[y0:y1, x0:x1]; b = B[y0:y1, x0:x1]; c = C[y0:y1, x0:x1]
        print("%-11s off vs auto: PSNR %5.1f dB ratio %.4f | noise auto2 vs auto: PSNR %5.1f dB ratio %.4f" % (name, 10*np.log10(255**2/max(((b-a)**2).mean(),1e-9)), b.mean()/a.mean(), 10*np.log10(255**2/max(((c-a)**2).mean(),1e-9)), c.mean()/a.mean()))
except Exception as e:
    print("compare ERR", e)
PYEOF
U cmd "r.SkyLight.RealTimeReflectionCapture 1"
python "$S/boxstate.py" restore "$M/lumenab_state.json" | cut -c1-200
