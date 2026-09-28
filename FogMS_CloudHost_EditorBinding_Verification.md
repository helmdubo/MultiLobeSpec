# Cloud Host: editor frame gaps, 2026-09-28

Scope: camera/sun rotation regression reported in the existing `FogMS_Box` scene. No Engine edits, new authored levels or saves of the owner's dirty map.

## Findings

Two distinct conditions were observed in the original editor:

1. The Box initially fell back to Froxel Fog because its density band reached below altitude zero of the SkyAtmosphere planet (`-0.023–0.277 km`). A temporary 35 m lift admitted the native cloud host again. The lift was restored. The owner's subsequent Box/profile edits left the band above zero; those edits were retained.
2. With that valid band, background throttling still emptied the host with `its Box stopped using it`. The subsystem compared the last actor/view feed with global `GFrameCounter`; editor frames continued while the actor and viewport did not update. The log then restored native defaults: RT Mode `3 → 0`, SampleMinCount `32 → 2`, DistanceToSampleMaxCount `2 → 15 km`. The next rendered view reacquired the host. This is an ownership/quality transition, independent of ordinary native tracing dither.

During a 16 s live probe with background throttling temporarily disabled and density animation frozen, all 64 samples retained Mode 3 / SampleMinCount 32 / DistanceToSampleMaxCount 2 km, across static, camera-rotation, sun-rotation and settling phases. Camera, sun and animation controls were restored. This proves stable settings during continuous rendering; it does **not** establish that every visible shimmer has disappeared.

## Change

`UFogMSCloudHostSubsystem::Tick` collects stale editor owners and calls their normal `UpdateDensity` validation before expiring bindings. A valid idle Box retains its host; hiding, disabling, invalid density and destruction still release it. Collection happens before updates because they may change the host array. `AcquireHost` also keeps an existing editor binding reserved until this validation releases it, preventing another Box from taking it solely because of a viewport pause. Game/PIE keep the original frame heartbeat.

## Verification

- Fresh staged `BuildPlugin -StrictIncludes`: Editor, UnrealGame Development and Shipping succeeded.
- Live pre-fix reproduction: original project log and `steady_probe_result.json` under `E:/GITHUB/MultiLobeSpec/.codex-build/CloudRotation_20260928`.
- Focused post-fix D3D12/SM6 editor lifecycle check passed (`Build/editor_binding_result2.json`, `Build/EditorBinding2.log` under that artifact directory). With actor ticking and viewport realtime off, the bound Box retained Mode 3 / minimum 32 / distance 2 km and nonzero density. Hiding, disabling, invalid density and deletion each zeroed its material density and restored Mode 0 / minimum 2 / distance 15 km; showing, enabling and valid density reacquired it. The isolated editor exited cleanly. Its DLL hash matches the fresh build package.
- The first harness run stopped on an incorrect assumption that a never-claimed host's authored material default must be zero. That assertion was corrected; it was not a failure of the stale-binding change. Only the successful second run establishes lifecycle acceptance.
- During the initial diagnosis the original editor stayed open and its dirty map was retained. Installation below happened only after the owner's explicit restart/discard approval.

## Installation and original-project check

The owner authorized a restart without saving. The previous plugin code/binaries were copied to `CloudRotation_20260928/Install/Before`; the verified package replaced only Binaries, Source, Shaders, Config, Resources and the descriptor. Plugin Content was retained. All 88 installed files were hash-checked. The saved `FogMS_Box.umap` hash remained unchanged. Installed `UnrealEditor-MultiLobeSpec.dll`: `A0FBC463ECE98C12F1B0BD41671FAC7EA5A51431910BC684B74E6FE90C32CEB9`.

The first launch exited because UE interpreted backslashes in the `-ExecutePythonScript` path as escapes. Relaunching with forward slashes completed the usual startup preset. Editor PID 47896 opened the same map with D3D12/SM6/BindlessAll, active B3 transport and Cloud Host delivery.

A second 16 s probe in the original project rotated the camera and sun by ±10° (density animation frozen), then restored the captured camera/sun and animation controls. All 64 samples held Mode 3 / minimum 32 / distance 2 km. After restoring background throttling, a subsequent snapshot still reported those settings and active transport. Evidence: `Install/rotation_probe_result.json`, `Install/owner_state.json`, `Install/installed_files.json`, `Install/install_result.json`. No test callback remains active.

This accepts installation, binding lifetime and stable settings under rotation/background pauses. Residual visual shimmer still needs the owner's field comparison; these settings samples do not prove its complete removal. The below-ground Cloud Host restriction remains a native-carrier limitation; this fix does not remove it.
