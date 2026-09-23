import unreal, sys
MODE = "__MODE__"
actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()
box = next((a for a in actors if a.get_actor_label() == 'FogMS - Live Box'), None)
prev = box.get_editor_property('scattering_mode')
box.set_editor_property('scattering_mode', getattr(unreal.FogMSScatteringMode, MODE))
print("scattering_mode: %s -> %s" % (prev, box.get_editor_property('scattering_mode')))
