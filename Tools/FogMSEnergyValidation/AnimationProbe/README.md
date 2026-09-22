# FogMS animation GPU probe

This suite is explicit and asynchronous. It uses only the existing editor map `/Game/FogMS_Test/FogMS_Box`, outside PIE, and the existing `FogMS - Live Box`. It creates no actors or maps and never saves the scene or camera. It temporarily changes the Box scattering mode, animation controls and transform; switches the two Transport fixture CVars to zero; and disables editor throttling/autosave. Every mutation is restored from a fresh durable snapshot, not from a historical baseline.

## Run

1. Install a package containing the animation properties and the new Transport dump metadata listed below. Compile shaders and wait for the scene to render normally. The existing density must be enabled, world-aligned, visible and nonzero. Keep the viewport still throughout the test.
2. Edit `animation_config.json`: choose a new immutable `id`, an absolute `evidence_root`, and `TRANSPORT` (B2, cheaper) or `ANGULAR_TRANSPORT` (B3). The probe preserves the authored iteration count and angular quality.
3. Execute `animation_probe.py` explicitly with the UE console `py` command or the existing MCP `run_python_file` bridge. Importing the entry does not start a probe.
4. Wait for `<evidence_root>/<id>/receipt.json` to report `COMPLETED` and `restoration_ok: true`. Bridge acceptance only means the callback was scheduled.
5. Outside Unreal, run `python -P analyze_animation.py <absolute-receipt-path>`. Review the sibling `analysis.json`; acceptance requires PASS and complete restoration.

All paths written by the probe are under the new id directory. It refuses reused ids or existing readback prefixes. `original.json` and a PREPARED receipt are written before any scene mutation. Each case is recorded durably; the native readback JSON and raw RGBA32F payload are retained with SHA-256 evidence.

To stop a running probe, set config `action` to `stop` and execute the entry again. To recover after a Python/editor restart, set `action` to `restore` with the original `id` and `evidence_root`, reopen the original editor map, and execute the entry. The recovery receipt is updated; no new run overwrites its original snapshot. Restore each failed property explicitly if `RESTORATION_INCOMPLETE` remains. Set `action` back to `run` with a new id for the next test.

## Cases and acceptance

The 11 cases each capture a barrier and a measurement at least two fresh render/producer frames later:

- Static animation disabled versus enabled at manual time zero: coefficient slab must agree.
- Positive wind at manual time T: density must move; translating the Box by `wind*T` must recover the static coefficient slab. This tests physical world-space advection without requiring an exported texture CPU decoder.
- Nonuniform Box resize: texture frequencies and phases stay unchanged at the same world center/time. The density coefficients may change because grid sample positions and clipping bounds change.
- Independent detail/evolution velocities: CPU phase formula must match native MID, and the coefficient shape changes when authored detail strength is nonzero.
- Two live snapshots: time, phases and density progress while packet revision stays stable and global history reset remains false after settling.
- Freeze and delayed frozen snapshot: coefficient slab stays identical. Resume maintains the same immediate phase, then moves again.

Phase tests use `MaterialInstanceDynamic.get_vector_parameter_value`, whose ScriptName is exported in UE's `Materials/MaterialInstanceDynamic.h`. World time is read through `GameplayStatics.get_time_seconds`; the actor's `Density Animation Time` supplies the actual CPU phase snapshot. No private/reflection-only guessed APIs are used.

Required top-level `FogMS.DumpSpatial` metadata: `revision`, `densityPhase0`, `densityPhase1`, `densityPhase2` (three floats each), `animationActive` and `historyReset`. The regular four-slab `transport` layout, Test0 authored coefficients and valid render/source frame stamps are also required. Missing metadata fails the probe and triggers restoration; it is not silently called a pass.

The analyzer asserts exact MID/packet phase agreement only for static/manual/frozen cases. Exact live MID/producer time alignment is explicitly **NOT_RUN** because the dump does not include its CPU time snapshot; the async producer and Python reads can describe adjacent frames. Rendered trails/flicker and the quality of local reactive rejection require a separate viewport test. Readbacks stall rendering, so this is not a performance benchmark. Map file bytes must remain unchanged; the unsaved editor dirty state can change when temporary properties are restored.

## Harness checks without Unreal

`python -P test_animation_probe.py` runs cleanup fault injection (14 setter failure positions, seven verification failure positions), atomic receipt checks, synthetic full-analysis acceptance, wrong-phase detection, continuous-history-reset rejection, incomplete restoration/case rejection and payload tamper detection. These validate the harness and analyzer; they do not substitute for the GPU run above.
