# On-demand only: run this file explicitly with UE console py. Import does not start a probe.
"""Explicit UE bridge entry for the B2 wall probe; async globals remain isolated."""
import importlib.util
import pathlib
import sys

_gpu_file = pathlib.Path(__file__).resolve().parent / 'gpu_wall_impl.py'
_gpu_spec = importlib.util.spec_from_file_location('_fogms_b2_wall_impl', _gpu_file)
_gpu_module = importlib.util.module_from_spec(_gpu_spec)
sys.modules[_gpu_spec.name] = _gpu_module
_gpu_spec.loader.exec_module(_gpu_module)
if __name__ == '__main__':
    _gpu_module.launch()
