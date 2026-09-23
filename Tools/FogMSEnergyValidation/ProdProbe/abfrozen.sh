#!/bin/bash
# Frozen-density A/B: sun-aligned quadrature vs axis-aligned, against a 96/64 cold reference at the same phase.
S="$(cd "$(dirname "$0")" && pwd)"; M="$S/measure"
U() { python "$S/uemcp.py" "$@" >/dev/null; }
MEAS() { python "$S/measure.py" "$@" 2>&1 | grep "^==" | sed 's/| metrics.*"maxRelativeCellResidual": \([0-9.e-]*\).*/| maxres \1/' | cut -c1-170; }
rm -rf "$M"/f_* 2>/dev/null
U py "import unreal
b=[a for a in unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors() if a.get_actor_label()=='FogMS - Live Box'][0]
b.freeze_density_animation(); b.update_density(); print('FROZEN')"
U cmd "r.FogMS.Transport.Tolerance 1e-14"; U cmd "r.FogMS.Transport.WarmStart 0"; U cmd "r.FogMS.Transport.SunAligned 0"
U cmd "r.FogMS.World.SkyMipBias -20"
MEAS f_ref96_it64_cold_sun0_point  mode=ANGULAR_TRANSPORT quality=HIGH96 iterations=64 settle=8
U cmd "r.FogMS.World.SkyMipBias 0"
MEAS f_ref96_it64_cold_sun0  quality=HIGH96 iterations=64 settle=8
U cmd "r.FogMS.Transport.SunAligned 1"
MEAS f_ref96_it64_cold_sun1  quality=HIGH96 iterations=64 settle=8
U cmd "r.FogMS.Transport.WarmStart 1"; U cmd "r.FogMS.Transport.Tolerance 0.000001"
for q in LOW16 MEDIUM24 BALANCED48; do for sun in 0 1; do
  U cmd "r.FogMS.Transport.SunAligned $sun"
  MEAS "f_${q}_it16_tol1e6_sun${sun}" quality=$q iterations=16 settle=8
done; done
echo "--- field differences vs frozen reference (96/64 cold, axis-aligned)"
for d in f_ref96_it64_cold_sun0_point f_ref96_it64_cold_sun1 f_LOW16_it16_tol1e6_sun0 f_LOW16_it16_tol1e6_sun1 f_MEDIUM24_it16_tol1e6_sun0 f_MEDIUM24_it16_tol1e6_sun1 f_BALANCED48_it16_tol1e6_sun0 f_BALANCED48_it16_tol1e6_sun1; do
  python "$S/fielddiff.py" "$M/$d/dump.rgba32f" "$M/f_ref96_it64_cold_sun0/dump.rgba32f"
done
# restore: resume animation, authored 96/16, defaults
U cmd "r.FogMS.Transport.Tolerance 1e-14"; U cmd "r.FogMS.Transport.SunAligned 1"
MEAS restore_96_it16 quality=HIGH96 iterations=16 settle=3
U py "import unreal
b=[a for a in unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors() if a.get_actor_label()=='FogMS - Live Box'][0]
b.resume_density_animation(); print('RESUMED')"
