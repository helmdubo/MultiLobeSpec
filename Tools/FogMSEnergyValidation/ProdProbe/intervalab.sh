#!/bin/bash
# A/B of r.FogMS.Transport.SolveInterval on frozen density: Production + Emissive Injection (+hybrid as authored),
# N = 1 / 2 / 4 with async late publish, then N = 1 again. GPU frame, transport cost, status text and screenshot PSNR
# against N = 1 inside the Box. Restores the actor to the snapshot taken at the start. Needs FOGMS_LOG and a free editor.
S="$(cd "$(dirname "$0")" && pwd)"; M="$S/measure"
U() { python "$S/uemcp.py" "$@" >/dev/null; }
PY() { python "$S/uemcp.py" py "$1" | grep -o '"output": "[^"]*"' | head -1; }
BOX="b=[a for a in unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors() if a.get_actor_label()=='FogMS - Live Box'][0]"
MEAS() { python "$S/measure.py" "$@" 2>&1 | grep "^==" | sed 's/| metrics.*"maxRelativeCellResidual": \([0-9.e-]*\).*/| maxres \1/' | cut -c1-170; }
STATUS() { PY "import unreal
$BOX
print('$1', b.get_editor_property('spatial_status'))"; }
python "$S/boxstate.py" save "$M/intervalab_state.json" | cut -c1-120
rm -rf "$M"/int_* 2>/dev/null
U cmd "r.FogMS.Transport.WarmStart 1"; U cmd "r.FogMS.Transport.SunAligned 1"; U cmd "r.FogMS.Transport.SweepThreads 1024"; U cmd "r.FogMS.Transport.AsyncCompute 1"
PY "import unreal
$BOX
b.set_editor_property('manual_animation_time', 100.0); b.set_editor_property('use_manual_animation_time', True)
b.set_editor_property('transport_preset', unreal.FogMSTransportPreset.PRODUCTION); b.set_editor_property('emissive_injection', True); b.update_density(); print('FROZEN hybrid=%s' % b.get_editor_property('hybrid_single_scattering'))"
for N in 1 2 4 1; do
  U cmd "r.FogMS.Transport.SolveInterval $N"; sleep 3
  MEAS int_n$N quality=LOW16 iterations=16 settle=8; STATUS "N=$N"
  # second profile a frame later catches the other phase of the interval
  python "$S/gpuprofile.py" int_n${N}_b 2>&1 | grep "GPU frame\|transport/world" | tr '\n' ' ' | cut -c1-150; echo
done
U cmd "r.FogMS.Transport.SolveInterval 1"; U cmd "r.FogMS.Transport.AsyncCompute 0"
echo "--- screenshot PSNR vs N=1 (box front face / core)"
python - <<'PYEOF'
from PIL import Image; import numpy as np, os
M = os.path.join(os.getcwd(), "measure")
def load(t): return np.asarray(Image.open(os.path.join(M, t, "shot.png")).convert('RGB'), dtype=np.float64)
try:
    R = load("int_n1")
    for t in ("int_n2", "int_n4"):
        A = load(t)
        for name, (y0, y1, x0, x1) in {"front face": (140, 700, 430, 1340), "core": (250, 550, 600, 1200)}.items():
            a = A[y0:y1, x0:x1]; r = R[y0:y1, x0:x1]; mse = ((a - r) ** 2).mean()
            print("%-8s %-11s PSNR %5.1f dB | mean ratio %.4f" % (t, name, 10 * np.log10(255 ** 2 / max(mse, 1e-9)), a.mean() / r.mean()))
except Exception as e:
    print("compare ERR", e)
PYEOF
python "$S/boxstate.py" restore "$M/intervalab_state.json" | cut -c1-200
