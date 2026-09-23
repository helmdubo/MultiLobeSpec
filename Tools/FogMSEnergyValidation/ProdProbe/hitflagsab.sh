#!/bin/bash
# A/B of r.FogMS.Transport.PublicHitFlags (1 = CastShadow flags rebuilt from public ray-tracing bindings, 0 = private
# FViewInfo buffer) on frozen density, Production tier, overlay delivery (so FogMS.DumpSpatial dumps the resident atlas).
# Expected: identical field unless the scene has dynamic meshes with ray-traced shadows off. Needs BindlessAll editor,
# FOGMS_LOG and a free editor; restores the actor from a snapshot.
S="$(cd "$(dirname "$0")" && pwd)"; M="$S/measure"
U() { python "$S/uemcp.py" "$@" >/dev/null; }
PY() { python "$S/uemcp.py" py "$1" | grep -o '"output": "[^"]*"' | head -1; }
BOX="b=[a for a in unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors() if a.get_actor_label()=='FogMS - Live Box'][0]"
MEAS() { python "$S/measure.py" "$@" 2>&1 | grep "^==" | sed 's/| metrics.*"maxRelativeCellResidual": \([0-9.e-]*\).*/| maxres \1/' | cut -c1-170; }
python "$S/boxstate.py" save "$M/hitflags_state.json" | cut -c1-120
rm -rf "$M"/hf_* 2>/dev/null
U cmd "r.FogMS.Transport.WarmStart 1"; U cmd "r.FogMS.Transport.SunAligned 1"; U cmd "r.FogMS.Transport.SweepThreads 1024"; U cmd "r.FogMS.Transport.AsyncCompute 0"; U cmd "r.FogMS.Transport.SolveInterval 1"
PY "import unreal
$BOX
b.set_editor_property('manual_animation_time', 100.0); b.set_editor_property('use_manual_animation_time', True)
b.set_editor_property('transport_preset', unreal.FogMSTransportPreset.PRODUCTION); b.set_editor_property('emissive_injection', False); b.set_editor_property('hybrid_single_scattering', False); b.update_density(); print('FROZEN overlay')"
for F in 1 0 1; do
  U cmd "r.FogMS.Transport.PublicHitFlags $F"; sleep 3
  MEAS hf_$F quality=LOW16 iterations=16 settle=8
  PY "import unreal
$BOX
print('FLAGS=$F', b.get_editor_property('spatial_status'))"
done
echo "--- field: public (1) vs private (0)"; python "$S/fielddiff.py" "$M/hf_1/dump.rgba32f" "$M/hf_0/dump.rgba32f"
python - <<'PYEOF'
from PIL import Image; import numpy as np, os
M = os.path.join(os.getcwd(), "measure")
def load(t): return np.asarray(Image.open(os.path.join(M, t, "shot.png")).convert('RGB'), dtype=np.float64)[140:700, 430:1340]
try:
    A = load("hf_1"); B = load("hf_0"); mse = ((A - B) ** 2).mean()
    print("screenshot public vs private: PSNR %.1f dB | mean ratio %.4f" % (10 * np.log10(255 ** 2 / max(mse, 1e-9)), A.mean() / B.mean()))
except Exception as e:
    print("compare ERR", e)
PYEOF
U cmd "r.FogMS.Transport.PublicHitFlags 1"
python "$S/boxstate.py" restore "$M/hitflags_state.json" | cut -c1-200
