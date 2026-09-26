# FogMS — D3D12 crash, 2026-09-21

## Diagnosis

The field crash is the same native CPU access violation as the earlier startup crash. Delaying Apply did **not** fix it. Three crash contexts have identical normalized 21-frame stacks (portable call-stack hash `9B0B49E73E1A6791BF44902E4B8E0704DE8B3E40`), with process lifetimes 53, 16876 and 255 seconds. The long run disproves a startup-only explanation. The native breadcrumb is `FogMS - Sun / ShadowBatch / DirectLighting / RenderDeferredLighting`.

The installed UE 5.8.2 D3D12RHI binary was examined read-only with VS `dumpbin`. At module RVA `0x743a6`, the failing instruction reads `[rax+0x10]`, after loading `CurrentViewHeap`. Adjacent embedded source/assert strings and line constants identify `D3D12DescriptorCache.cpp::SetDescriptorHeaps`. Caller RVA `0x7a38c` is `UnsetExplicitDescriptorCache`; RVA `0xbb754` follows that call in native ray-tracing dispatch cleanup.

Source chain under `D:/PersonalProjects/UE5/UE_5.8/Engine/Source/Runtime`:

1. `D3D12RHI/Private/D3D12DescriptorCache.cpp:53–67`: `BindlessAll` leaves `CurrentViewHeap == nullptr`.
2. `D3D12DescriptorCache.h`: a fresh context starts with `bLastSetHeapsBindless=false`. Opening its command list assigns heap objects, but does not establish the remembered bindless binding.
3. `D3D12DescriptorCache.cpp:855–868`: native RT's explicit cache saves that previous binding flag.
4. `D3D12RayTracing.cpp:6224` / `D3D12DescriptorCache.cpp:877–884`: cleanup restores according to the saved flag. False selects the absent bindful heap at line 94.
5. `RHI/Private/RHICommandList.cpp:427–429`: parallel translation obtains a separate graphics context. Such a fresh context can first execute RT even late in an editor session.

The crash instruction and restore path are identified. The dump has no matching private PDB, so the exact allocation/first-use history of the individual context is inferred from the source and stack, not reconstructed as a full symbolic debugger trace. This is not a demonstrated FogMS texture use-after-free or GPU device-removal error.

## Plugin-only compatibility workaround

`Source/FogMSRender/Private/FogMS_RHICompatibility.{h,cpp}` is owned by the early FogMSRender module. It applies only to UE **5.8.2**, Windows **D3D12 / SM6 / BindlessAll / single GPU**.

- At `ShaderTypesReady`, after RHIInit/RenderUtilsInit and before global shaders/scene rendering, set `r.RHICmd.ParallelTranslate.Enable=0`. Ordinary graphics command lists then use the persistent default context. Parallel RDG recording stays at its existing value (2 in the tested project).
- Prime that default context once through the public `ID3D12DynamicRHI::RHIGetGraphicsCommandList` and `RHIFinishExternalComputeWork` APIs, in an immediate graphics RHI lambda. This binds the correct heaps before any first RT dispatch, with no dummy shader or resource allocation. In this UE source, `IsUsingBindlessHeap()` includes `bFullyBindless`, so public state restoration selects the correct bindless path even on a fresh default context.
- Set priority explicitly within Code..Console, read back the result, and maintain it through a console-variable sink. The sink is a guard against later settings changes, not a fence capable of cancelling already recorded work. Startup placement is essential.
- The compatibility setting remains for the editor process, including FogMS Off/legacy. Disabling an actor does not remove the native BindlessAll risk. No ini is written. Dynamic reloading of this global-shader module is disabled.

RT shadows, material evaluation, Lumen HWRT, FogMS source terms, shadow filtering and spatial transport remain enabled. Rendering equations and quality settings were not changed in this fix. The tradeoff is reduced **CPU RHI translation parallelism**; a zero performance cost is not claimed. This does not disable the parallel render graph, the RHI thread or ray tracing.

No Engine file, binary or in-memory instruction is patched. Do not re-enable parallel translation in the affected configuration as a visual comparison switch. A future Engine version needs a fresh source audit; the workaround is deliberately not silently applied to all versions.

## Build, installation and runtime evidence

Evidence directory: `E:/GITHUB/MultiLobeSpec/.codex-build/FogMS_Crash_20260921`.

- Final `Build3.log`: BuildPlugin `-StrictIncludes`, no unity/PCH, PASS. Package3 installed and SHA256-verified: **72 files**.
- Author map and previous plugin backup: `D:/PersonalProjects/UE5/MimirHead_portfolio 5.7 5.8 - 3/Saved/FogMS_Backups/FogMS_Crash_20260921_103122`.
- Preliminary Package2 startup succeeded but reported an incorrect constructor-priority use of SetWithCurrentPriority; Package3 corrects the priority range and verifies the CVar value.
- `MainGPU2.log` / `compat-final-01-soak.json`: final Package3 ran the existing scene for **900.016 seconds / 13048 engine frames** under camera near/far/orbit/turn-away motion, 29 scheduled pillar/sun changes. No crash, Python exception or RHI error during the soak. All four FogMS statuses stayed Active at the recorded samples. Monitored CVars and authored volume properties stayed equal to the initial snapshot.
- Camera, sun transform and pillar visibility restored (camera angular error <0.000001 degrees; sun <0.000002 degrees). Autosave restored. Receipt `restoration_ok=true`, no cleanup errors.
- `MainGPU3.log`: second final-Package3 startup passed, heap prime/compatibility guard before scene rendering, normal preset completion, all four FogMS statuses Active. No manual compatibility command on this launch. The existing level is open for the owner, original background-throttle preference restored.
- `delivery-receipt.json`: PASS, 15 existing actors, no differences in checked authored properties/transforms/visibility from the pre-fix snapshot (float tolerance 1e-5). Spatial readback is finite, nonnegative and nonzero: mean RGB J=1.95894, max=8.13803; mean valid fraction=0.834013.
- `files-and-stacks-receipt.json`: 72 installed files match Package3, 48 Source/Shader files match the fresh staged build, 6 Engine shader anchors match the pre-existing baseline. Three crash stacks match each other. No Engine edits were made; no commit/push/merge performed.

All runtime tests use the existing `/Game/FogMS_Test/FogMS_Box`. The soak temporarily moves the camera, rotates the existing sun and toggles one pillar, then restores value-copied originals. It creates no levels or actors. Editor autosave is paused during temporary scene changes and restored afterward.

The first diagnostic CSV request used an absolute filename; UE's console command prepends `Saved/Profiling/CSV` regardless, so that capture failed to open its file. Retried with a basename, successfully. This was a test-harness file-path error, not a renderer failure. CSV results are bounded by editor presentation: `RHIThreadTime` subtracts accounted waits/Present, but multiple Slate windows can make it an inter-window interval rather than full-frame translation cost. Neither low RHI time nor a successful GPU profile proves zero overhead versus the unsafe mode.

Static capture after restoring the owner camera: RTX3070 / 1479×954 viewport, native GPU profile **17.82ms**, Spatial **0.816ms**, TLV Lighting **4.832ms**, VolumetricFog LightScattering **2.590ms**. CSV after removing 16 boundary samples on either side: GPU mean **17.716ms**, RHIThreadTime mean **0.245ms**, p95 **0.332ms**; moving-camera RHI mean **0.309ms**, p95 **0.464ms**. These are observations with the guard, not matched before/after deltas. The background editor's ~68.96ms wall-frame interval must not be reported as RHI translation cost or as foreground game FPS. HDR spatial readback and the restored-view screenshot are saved as `compat-static.*`.

## Scope of confidence

The workaround addresses the identified ordinary-graphics-context RT heap-restore failure. Async-compute contexts and graphics subcommand contexts follow separate native allocation paths; this is not a universal fix for every D3D12 failure. The FogMS supported path uses graphics compute and the existing Lumen async setting remains 0. Multi-view lifetime concerns are separate from this single-view CPU null-heap failure.

A bounded soak and successful starts cannot prove that a crash previously seen after 4 h 41 min is impossible. Field validation remains necessary. The earlier startup delay remains in the launcher for continuity, but must not be described as the crash fix.

The previous indirect stability and camera-distance work remains experimental. This compatibility fix does not complete camera-independent transport, change the RC0/RC1 lighting contract, or claim to solve all temporal artifacts.
