# Continuous native viewport motion probe

This records **during** W/S travel, comparing `r.FogMS.ViewIntegration 0` and `2` at identical frozen density. It does not decide image quality. No UE/editor calls have been run as part of developing this harness.

The current viewport is the **near endpoint**. The far endpoint is 20,000 cm behind it along the current camera forward vector; rotation and FOV stay fixed. Each mode warms for 60 engine frames at an endpoint, travels forward over 180 engine frames, warms again, and travels back over 180 frames. Four legs therefore use at least 960 engine frames. There are at most 16 screenshot requests per leg. All native and FogMS SSFS is temporarily disabled, game view enabled, animation frozen with the native phase-preserving API, and background throttling/autosave disabled. No map, level or asset is saved or created.

## Invocation (owner operates UE)

Copy `continuous_config.example.json` to an absolute scratch config path. Set a fresh `id`, absolute `evidence_root`, and the exact current editor world/Box/exposure labels. Do not reuse a run directory. Start the existing capture worker externally, before requesting the Python script:

```powershell
node E:/GITHUB/MultiLobeSpec/MultiLobeSpec/Tools/FogMSEnergyValidation/MotionProbe/capture_worker.mjs E:/absolute/scratch/continuous-config.json
```

Send this Python through the existing bridge's script execution mechanism (or put it in a scratch wrapper and ask the bridge to execute that file):

```python
import runpy
runpy.run_path('E:/GITHUB/MultiLobeSpec/MultiLobeSpec/Tools/FogMSEnergyValidation/ContinuousProbe/continuous_probe.py', run_name='fogms_continuous_entry')['run']('E:/absolute/scratch/continuous-config.json')
```

Callbacks live in an isolated imported module; subsequent bridge `runpy` calls cannot overwrite their globals. `run()` returns immediately after registration. A fresh run needs the existing fixed-exposure actor and an unpiloted editor viewport, not PIE. Do not run another camera controller/probe at the same time. To stop the active probe through the same wrapper:

```python
runpy.run_path('E:/GITHUB/MultiLobeSpec/MultiLobeSpec/Tools/FogMSEnergyValidation/ContinuousProbe/continuous_probe.py', run_name='fogms_continuous_entry')['run']({'action': 'stop'})
```

If a direct `runpy.run_path(..., run_name='__main__')` invocation is preferred, place the config as `continuous_config.json` alongside the wrapper first. Explicit external config is preferred because it leaves the repository unchanged.

## Timing and evidence

The existing worker uses `UE_MCP_Bridge.capture_screenshot`, `target=editor`. It does not use HighResShot or SceneCapture. Camera movement never pauses for a pending capture. If readback is slow, scheduled slots are skipped and recorded. A capture completing outside its moving leg is retained but marked invalid for continuous-motion comparison. Two valid captures per leg are only a minimum protocol completeness condition, not a quality acceptance threshold.

`original.json`, `config.json`, `frozen.json`, every request/ack/event, `commanded-poses.json`, and the final `receipt.json`/`finished.json` are immutable. Screenshots remain after a failure. Capture metadata includes request and observation engine counters, both camera poses, timing interval, mode, direction and leg. `commanded-poses.json` contains each actual camera command; frame-counter gaps and an overshoot beyond the requested 180 frames remain visible. The interval is a **CPU scheduling interval, not the exact GPU capture frame/pose**: render-thread lag and async screenshot delivery prevent that claim. The protocol records engine frames, not Slate callback count or wall-clock estimates.

Compare the same mode/path/near endpoint/frozen phase across runs. Inspect moving silhouettes and high-contrast interior features; do not interpret raw differences between successive translated images as flicker. A useful next analysis is image alignment or a same-pose reference trajectory with residual contrast/edge measurements, retaining geometric/parallax limitations. This harness does not synthesize a PASS from image means or post-stop settling.

## Ownership and limitations

The script guards actor transforms/editor hidden states, light intensity/color, selected Box density/lighting properties, Box component extent/transform and wind-arrow transform, fixed-exposure fields, motion state, frozen phases, owned CVars/preferences, current world/viewport, game view and camera/FOV. An external change aborts the run. It never writes authored density, transforms, exposure or lights back. Individual custom show flags and every native cloud/fog/sky material/component property are not enumerated; these must be left unchanged by the operator.

On external camera/FOV movement or a viewport/world/PIE/pilot change, it **does not restore the stale original camera**. Only values still matching the harness's last owned value are restored; externally changed preferences/CVars/motion are preserved and identified in the receipt. The animation controls and reference are restored together only while still owned. Originally live motion resumes its original timeline, including elapsed probe time; this is not a promise to return to the old visual phase. Native setters may dirty the map, but the map file hash must stay unchanged and the harness never saves it.

`COMPLETED` means four legs completed, minimum continuous captures exist, and owned values restored. `visual_acceptance` is always `NOT_EVALUATED`. Missing screenshots, timeouts, changed authoring or restoration failures are explicit. A Python callback cannot restore after an editor crash/process kill or while the editor stops ticking; the durable snapshot supports manual recovery. There is intentionally no automatic stale restore after restart. Console numeric values are restored, but the Python console API cannot restore the original `SetBy` priority flags.

Offline tests exercise scheduling, in-motion pending readbacks, intervention protection, restoration and immutable evidence. They are not native render validation:

```powershell
python -P -m unittest discover -s Tools/FogMSEnergyValidation/ContinuousProbe -p test_continuous_probe.py -v
```
