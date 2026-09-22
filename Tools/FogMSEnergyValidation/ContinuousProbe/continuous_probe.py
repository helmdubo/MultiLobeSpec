"""Isolated entry point for UE bridge runpy; see README.md. Does not run on import."""
import importlib.util
import pathlib
import sys


def run(config):
    name = '_fogms_continuous_probe_impl'
    module = sys.modules.get(name)
    controller = getattr(module, 'CONTROLLER', None)
    if module is None or controller is None or controller.done:
        spec = importlib.util.spec_from_file_location(name, pathlib.Path(__file__).with_name('continuous_impl.py'))
        module = importlib.util.module_from_spec(spec)
        sys.modules[name] = module
        spec.loader.exec_module(module)
    return module.launch(config)


if __name__ == '__main__':
    # A bridge wrapper can call run(absolute_config_path) instead of using this file.
    run(pathlib.Path(__file__).with_name('continuous_config.json'))
