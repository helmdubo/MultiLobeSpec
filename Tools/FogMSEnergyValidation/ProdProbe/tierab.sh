#!/bin/bash
# Tier A/B on frozen density: B2 (6 dirs) vs B3-16 (Production) vs B3-96/64 reference; cost + field error vs reference.
S="$(cd "$(dirname "$0")" && pwd)"; M="$S/measure"
U() { python "$S/uemcp.py" "$@" >/dev/null; }
PY() { python "$S/uemcp.py" py "$1" | grep -o '"output": "[^"]*"' | head -1; }
BOX="b=[a for a in unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors() if a.get_actor_label()=='FogMS - Live Box'][0]"
MEAS() { python "$S/measure.py" "$@" 2>&1 | grep "^==" | sed 's/| metrics.*"maxRelativeCellResidual": \([0-9.e-]*\).*/| maxres \1/' | cut -c1-170; }
rm -rf "$M"/tier_* 2>/dev/null
U cmd "r.FogMS.Transport.WarmStart 1"; U cmd "r.FogMS.Transport.SunAligned 1"; U cmd "r.FogMS.Transport.SweepThreads 1024"; U cmd "r.FogMS.Transport.AsyncCompute 0"
PY "import unreal
$BOX
b.freeze_density_animation(); b.set_editor_property('emissive_injection', False); b.set_editor_property('transport_preset', unreal.FogMSTransportPreset.CUSTOM); b.set_editor_property('transport_tolerance', 1e-14); b.update_density(); print('FROZEN')"
MEAS tier_ref96 mode=ANGULAR_TRANSPORT quality=HIGH96 iterations=64 settle=10
PY "import unreal
$BOX
b.set_editor_property('transport_tolerance', 1e-6); b.update_density(); print('TOL 1e-6')"
MEAS tier_b3_16 mode=ANGULAR_TRANSPORT quality=LOW16 iterations=16 settle=8
MEAS tier_b3_24 mode=ANGULAR_TRANSPORT quality=MEDIUM24 iterations=16 settle=8
MEAS tier_b2 mode=TRANSPORT iterations=16 settle=8
MEAS tier_b2_it8 mode=TRANSPORT iterations=8 settle=8
PY "import unreal
$BOX
print('STATUS', b.get_editor_property('spatial_status'))"
echo "--- field vs reference (96/64, tol 1e-14)"
for t in tier_b3_16 tier_b3_24 tier_b2 tier_b2_it8; do python "$S/fielddiff.py" "$M/$t/dump.rgba32f" "$M/tier_ref96/dump.rgba32f"; done
echo "--- image PSNR vs reference (box front face)"; python - <<'PYEOF'
from PIL import Image; import numpy as np, os
M=os.path.join(os.path.dirname(os.path.abspath("tierab.sh")),"measure")
def load(t): return np.asarray(Image.open(os.path.join(M,t,"shot.png")).convert('RGB'),dtype=np.float64)[140:700,430:1340]
R=load("tier_ref96")
for t in ("tier_b3_16","tier_b3_24","tier_b2","tier_b2_it8"):
    A=load(t); mse=((A-R)**2).mean(); print("%-12s PSNR %.1f dB | mean ratio %.4f"%(t,10*np.log10(255**2/mse),A.mean()/R.mean()))
PYEOF
PY "import unreal
$BOX
b.set_editor_property('scattering_mode', unreal.FogMSScatteringMode.ANGULAR_TRANSPORT); b.set_editor_property('angular_quality', unreal.FogMSAngularQuality.HIGH96); b.set_editor_property('transport_iterations', 16); b.set_editor_property('transport_tolerance', -1.0); b.resume_density_animation(); b.update_density(); print('RESTORED')"
