# Async compute Step 3 verification

Implemented and verified on 2026-09-20. Stopped before Step 4 for user verification.

## Implemented behavior

Compute descriptors and transfer recorders now expose `SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute)`. The default remains `eDefault`. Graphics descriptors do not expose this setting.

The hint survives descriptor copies, recorded nodes, compiled passes, compiled nodes, execution-plan refresh, and RDG metrics. New graph builds invalidate previous execution plans through the existing build generation; released shader-pass storage resets its preference before reuse. Invalid enum values and stale recorders produce explicit validation errors. A preference does not make a culled pass live.

RenderDevice resolves eligibility using startup policy, selected queue capabilities, command requirements, and resource contracts. Resolution runs before transient materialization. Metrics print `queue_preference` and `async_eligibility`, with the following outcomes:

| Outcome | Meaning |
| --- | --- |
| `eligible` | The selected compute queue supports this pass and its resource contracts. |
| `not_requested` / `graphics_pass` | Default placement, or a graphics pass. |
| `policy_disabled` | Startup policy disables async compute. |
| `compute_unavailable` / `shares_graphics_queue` | No supported distinct compute queue. |
| `dependencies_unavailable` | Required GPU dependency support is unavailable. |
| `unsupported_commands` | The selected compute queue cannot execute every recorded operation. |
| `resource_queue_unsupported` | A physical resource does not permit the selected compute queue without ownership transfer. |
| `external_state_contract` | An explicit external-state import has no additional-queue ownership/synchronization protocol. |
| `viewport_resource` | The pass accesses a viewport color/depth resource. |

`RHIResource::IsAsyncComputeAccessible()` defaults to false for unknown resource wrappers. Engine-created Vulkan buffers and textures opt in because their allocation sharing includes the selected graphics/compute families. Importing an engine resource into RDG does not by itself make it an unsupported external allocation. Explicit external-state imports remain conservative.

Transfer passes accumulate separate transfer-queue and compute-queue support. Color clears can qualify for compute; mipmap blits require a queue with graphics capability. Copy checks include the selected queue's image granularity and existing depth/stencil restrictions. A later operation cannot erase an earlier restriction. Default upload/transfer routing remains unchanged. These command requirements follow the Khronos references for [vkCmdClearColorImage](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdClearColorImage.html) and [vkCmdBlitImage](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdBlitImage.html).

Eligibility is preparation for the scheduler, not a submission claim. Step 3 creates no RDG compute submissions, and startup still reports `scheduling=disabled (implementation pending)`. Multi-queue groups, synchronization, actual queue assignment, and voxelizer annotations belong to later steps.

## Verification results

MSVC x64 debug builds passed for `ConfigLoaderTest`, `VulkanRHITest`, `RenderCoreTest`, `VulkanRHIIntegrationTest`, and `scene_renderer_demo`.

| Coverage | Result |
| --- | --- |
| ConfigLoaderTest | 7 passed |
| VulkanRHITest | 35 passed |
| RenderCoreTest | 300 enabled tests passed; 7 existing benchmarks disabled |
| VulkanRHIIntegrationTest | 256 passed; no skips |
| Demo smoke matrix | 8 runs passed, 64 frames each: voxelizer `auto`/`comp` × RHI thread `0`/`1` × async policy `0`/`1` |

Total: **598 enabled tests passed**. Native integration and all eight demo runs reported zero `VUID-` messages, zero `SYNC-HAZARD` messages, and no memory leaks. Both RHI and RenderCore unit binaries also reported no leaks. The existing external GLI macro-redefinition warning remains.

The 32 new cases in [RDGQueuePreferenceTests.inl](../ZenSamples/RenderCoreTest/RDGQueuePreferenceTests.inl) cover both inline and threaded execution:

- Descriptor copy isolation; default, preferred, and graphics metadata; formatted metrics.
- Raw and logical color clears, mipmap fallback, and accumulating command restrictions.
- Default transfer routing and correct buffer-copy output, with preferred copies eligible for future compute scheduling.
- Engine imports, unavailable resource contracts, explicit external states, viewport resources, and compute buffer bindings.
- Plan refresh, stale-plan rejection, reused compiled storage, invalid enum values, stale recorders, and culling.
- Disabled policy, missing compute/dependency support, a shared graphics queue, compute-only versus graphics-capable queues, and copy granularity fallback.
- Preparation/refresh without submission-progress queries and no compute submissions from the current graph execution path.

The native allocation-sharing test now also checks the new accessibility contract. Existing deterministic compute handoff/readback, queue-selection, lifetime, upload, and failure tests remained green. Native coverage used an NVIDIA GeForce RTX 5080 with graphics `0:0`, compute `2:0`, and transfer `1:0`; unavailable queue topologies remain covered by controlled fixtures.

The smoke matrix covered mode switching, resize, minimize/restore, and shutdown with synchronization validation enabled. The temporary voxelizer setting was restored byte-for-byte; matching SHA-256 hashes are recorded in the smoke evidence. Actual GPU overlap, repeated async voxel updates, and performance comparisons await the scheduler steps.

Formatting with the repository `.clang-format`, `git diff --check`, and the RDG backend-access audit passed. No direct `GDynamicRHI`, submission-progress getters, or submission waits were introduced into `RDG*` or `RenderGraph*` implementation files.

## Reproduce and inspect

After entering the x64 MSVC developer environment, run from the repository root:

```powershell
cmake --build build/x64-windows-msvc-debug --target ConfigLoaderTest VulkanRHITest RenderCoreTest VulkanRHIIntegrationTest scene_renderer_demo -j 8
.\bin\RenderCoreTest.exe --gtest_filter=*RDGQueuePreference*
.\bin\ConfigLoaderTest.exe
.\bin\VulkanRHITest.exe
.\bin\RenderCoreTest.exe
python build/rhi-phase4/run_validation.py ../async-compute-step3-integration.log --gtest_output=xml:build/async-compute-step3-integration.xml
python build/async-compute-step3-smoke.py
```

The local native helper isolates the executable from RTSS without changing persistent overlay settings or disabling validation. The smoke helper temporarily selects each voxelizer and restores the original configuration.

Local evidence: `build/async-compute-step3-build.log`, `build/async-compute-step3-{focused,config,rhi,rendercore,integration}.{log,xml}`, `build/async-compute-step3-scene-{auto,comp}-{0,1}-{0,1}.log`, and `build/async-compute-step3-smoke.json`. Other platforms were not run.
