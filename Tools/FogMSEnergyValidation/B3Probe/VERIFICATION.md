# Offline verification receipt

- Four Python files compile successfully.
- Entry/implementation import does not load `unreal` or begin a capture.
- Nine synthetic CPU-generated cases pass the analyzer, including RGB and the
  independent full-wall topology/zero-leakage pair.
- Wrong directions, old transport scheme, stale source frame and wrong albedo
  are rejected.
- Static checks confirm added quality/albedo restoration and no actor/level
  creation, deletion or map-save calls.
- `verify_offline.py`: **13/13 PASS**. Its generated receipt is
  `_verification/offline-receipt.json` (ignored, reproducible).
- CPU cost at 32^3: GL48 with scalar albedo 0.9 converged in 13 iterations /
  10.8 seconds; GL96 pure absorption took 4.5 seconds without PCG iterations.

These checks used no Unreal process. Synthetic output is not a GPU validation
result. New UE enum/property exports, real callback restoration and installed
shader output still require the parent agent's runtime test.
