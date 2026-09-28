"""Read-only W51 A/B gate for the existing FogMS_Box editor level.

Run inside Unreal Python with two arguments: mode (A, C, B1) and weather
preset (Broken or Overcast). A failed gate raises before any GPU sample is
accepted; --report-only prints the receipt without raising for diagnosis.
The script does not modify actors, materials, cvars or packages.
"""

import json
import sys

import unreal


def main():
    if len(sys.argv) < 3:
        raise RuntimeError("Usage: w51_preflight.py <A|C|B1> <Broken|Overcast>")

    mode, preset = sys.argv[1:3]
    if mode not in ("A", "C", "B1") or preset not in ("Broken", "Overcast"):
        raise RuntimeError("Mode must be A, C or B1; preset must be Broken or Overcast")

    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    if not world or not world.get_path_name().startswith("/Game/FogMS_Test/FogMS_Box."):
        raise RuntimeError("W51 preflight requires the existing FogMS_Box editor level")

    actors = {
        actor.get_actor_label(): actor
        for actor in unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()
    }
    weather = actors.get("FogMS - Weather")
    box = actors.get("FogMS - Live Box")
    host_actor = actors.get("FogMS Cloud Host")
    if not weather or not box or not host_actor:
        raise RuntimeError("FogMS - Weather, FogMS - Live Box and FogMS Cloud Host must exist")

    host = host_actor.get_component_by_class(unreal.VolumetricCloudComponent)
    if not host:
        raise RuntimeError("FogMS Cloud Host has no VolumetricCloudComponent")

    state = weather.get_editor_property("weather_state")
    state_path = state.get_path_name() if state else ""
    status = weather.get_weather_status()
    preview = bool(weather.get_editor_property("native_weather_preview"))
    density = bool(box.get_editor_property("density_enabled"))
    bottom = float(host.get_editor_property("layer_bottom_altitude"))
    height = float(host.get_editor_property("layer_height"))
    trace = float(host.get_editor_property("tracing_max_distance"))
    scale = float(weather.get_editor_property("weather_scale"))

    errors = []
    if not state_path.endswith("DA_FogMS_Weather_%s.DA_FogMS_Weather_%s" % (preset, preset)):
        errors.append("Weather State is not %s" % preset)
    if not status.startswith("Active: '%s'" % preset):
        errors.append("WeatherStatus has not reached Active %s (actor tick may be inactive)" % preset)
    if preview != (mode != "A"):
        errors.append("Native Weather Preview differs from mode %s" % mode)
    if density != (mode != "B1"):
        errors.append("local Box density differs from mode %s" % mode)

    if mode != "A":
        expected_top = 3.5 * scale  # Both Broken and Overcast have a 3.5 km deck top.
        if "W51 native weather view: physical L0/L1" not in status:
            errors.append("the native weather view is not active on the Host")
        if bottom + height < expected_top - 0.1 * scale:
            errors.append("Host layer does not contain the weather deck top")
        if trace + 0.01 < float(weather.get_editor_property("native_weather_trace_distance_km")):
            errors.append("Host trace distance has not expanded for weather")

    receipt = {
        "mode": mode,
        "preset": preset,
        "world": world.get_path_name(),
        "weather_state": state_path,
        "weather_status": status,
        "native_preview": preview,
        "local_density": density,
        "host_bottom_km": bottom,
        "host_top_km": bottom + height,
        "host_trace_km": trace,
        "pass": not errors,
        "errors": errors,
    }
    print("FOGMS_W51_PREFLIGHT " + json.dumps(receipt, ensure_ascii=False))
    if errors and "--report-only" not in sys.argv[3:]:
        raise RuntimeError("W51 preflight failed: " + "; ".join(errors))


main()
