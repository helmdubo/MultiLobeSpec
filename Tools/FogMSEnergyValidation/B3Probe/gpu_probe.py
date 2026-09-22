"""Explicit UE console/bridge entry. Importing this module does not run a probe."""
import importlib.util
import pathlib
import sys

_path = pathlib.Path(__file__).resolve().with_name('gpu_impl.py')
_spec = importlib.util.spec_from_file_location('_fogms_b3_gpu_impl', _path)
_module = importlib.util.module_from_spec(_spec)
sys.modules[_spec.name] = _module
_spec.loader.exec_module(_module)
if __name__ == '__main__':
    _module.launch()
