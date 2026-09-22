"""Execute on demand in UE; callbacks remain owned by an importable module."""
import importlib.util
import pathlib
import sys

name = '_fogms_motion_probe_impl'
previous = sys.modules.get(name)
controller = getattr(previous, 'CONTROLLER', None)
if previous is None or controller is None or controller.done:
    spec = importlib.util.spec_from_file_location(name, pathlib.Path(__file__).with_name('motion_impl.py'))
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
sys.modules[name].launch()
