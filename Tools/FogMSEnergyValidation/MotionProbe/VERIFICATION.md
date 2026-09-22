# Verification scope

Offline verification on 2026-09-21:

- Python syntax compilation and Node `--check`: passed.
- `python -P test_motion.py`: nine tests passed. They exercise a complete mocked state machine and PNG receipt/hash analysis, camera divergence restoration, restoration after animation freeze mutates then raises, nested camera float comparison tolerance, mandatory keyword-only UE Rotator construction, game-view divergence, setter failure and exact-restoration failure. The complete run also checks legacy game-view metadata produces LIMITED comparison and known mode mismatch produces INVALID. Cross-run authored-scene comparison rejects changed hidden flags or density while permitting serialization roundoff.
- Mock PNGs are generated in temporary directories solely to exercise the analyzer. They are never reported as UE captures or visual acceptance.

Root executed the first real baseline with the installed older renderer. Native screenshot capture worked, but the harness stopped after three captures because Windows denied replacement of a request file while the worker was reading it. Failed evidence remains under `FogMS_Motion_20260921/motion-baseline-01`. The request/ack protocol was changed to immutable per-sequence files and the worker no longer reads mutable receipts. Re-run and renderer acceptance are owned by root; no real runtime PASS is claimed by this document.

Source-backed capture API: installed `UE_MCP_Bridge/Private/Handlers/EditorHandlers.cpp:759–794`, editor target calls viewport Invalidate plus `FScreenshotRequest::RequestScreenshot`, without HighResShot. This API does not expose exact capture render frame; reported intervals remain approximate.

The second real baseline exposed a harness bug in camera restoration: positional `unreal.Rotator(pitch,yaw,roll)` permuted the three UE Python constructor fields. All calls now use explicit keywords, the camera setter immediately validates round-trip values, and the mock rejects positional Rotator calls. Baseline01 screenshots therefore must not be assumed to use the requested rotation. Baseline02 preserves the last correct user camera in its `original.json`; explicit restoration must use that durable snapshot, not the corrupted live rotation. No failed run is accepted as visual evidence.
