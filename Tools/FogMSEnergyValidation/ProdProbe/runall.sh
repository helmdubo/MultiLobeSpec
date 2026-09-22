#!/bin/bash
# Measurement series. Env FOGMS_LOG must point to the active editor log.
S="$(cd "$(dirname "$0")" && pwd)"; M() { python "$S/measure.py" "$@" 2>&1 | grep "^==" ; }
python "$S/uemcp.py" cmd "r.FogMS.Transport.WarmStart 0" >/dev/null
M ref_96_it64_cold  mode=ANGULAR_TRANSPORT quality=HIGH96     iterations=64 settle=8
M b96_it16_cold     quality=HIGH96     iterations=16 settle=6
python "$S/uemcp.py" cmd "r.FogMS.Transport.WarmStart 1" >/dev/null
M b96_it16_warm     quality=HIGH96     iterations=16 settle=8
M b96_it4_warm      quality=HIGH96     iterations=4  settle=8
M b96_it2_warm      quality=HIGH96     iterations=2  settle=10
M b48_it4_warm      quality=BALANCED48 iterations=4  settle=8
M b24_it4_warm      quality=MEDIUM24   iterations=4  settle=8
M b16_it4_warm      quality=LOW16      iterations=4  settle=8
M b16_it2_warm      quality=LOW16      iterations=2  settle=10
# restore authored: 96 / 16 / warm on
M restore_96_it16_warm quality=HIGH96 iterations=16 settle=4
