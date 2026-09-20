# Async compute Step 4 verification

Implemented and verified on 2026-09-20. Stopped before Step 5 for user verification.

## Implemented behavior

RDG preparation now builds a schedule from the live pass graph before transient materialization. [RDGSchedule.h](../ZenCore/Include/Graphics/RenderCore/V2/RenderGraph/RDGSchedule.h) defines groups containing stable IDs, logical queue placement, native-queue equivalence IDs, pass preferences and eligibility reasons, resource access summaries, predecessor group IDs, and logical boundary requirements. These values contain no native handles or completion serials.

`RenderGraph::GetSchedule()` exposes the prepared schedule for inspection. Execution plans own a snapshot, so another preparation cannot silently change an existing plan's group data. Existing build-generation, preparation-serial, and resource-state revision checks still reject stale plans or require refresh. Metrics now include planned queue/group per pass, group count, multiple-queue status, and allocation-reuse policy.

Queue placement uses Step 3 eligibility. Eligible passes select the compute queue; default placement preserves whole-graph graphics/transfer compatibility. Group construction uses deterministic pass ordering and sorted predecessor lists. It preserves separate graphics groups around compute dependencies, allowing the independent graphics prefix to remain free of a compute dependency.

Coalescing requires compatible incoming dependencies and the same first consumers on other queues. This prevents both moving a new wait ahead of existing work and delaying a producer's signal by merging unrelated ready work. Groups whose signals have already been referenced are closed. Queue-order predecessors remain present for logical contexts sharing a native queue.

The schedule retains version-based RAW, WAR, and WAW dependencies. It adds whole-allocation image-layout dependencies: every reader of the old layout precedes its change, and subsequent readers wait for the pass establishing the new layout. Readers with an already compatible initial layout remain independent. Initial boundaries and placement refresh after materialization and after external/upload state changes.

`GetDependencies()` retains the resource-version graph, while `GetSchedule()` includes layout and submission-group requirements. Side-effect passes remain live through `NeverCull` or `allowCulling=false`; required GPU ordering comes from resource declarations. The manual pass-ordering API added during Step 4 was removed after review because production rendering does not use it. See the [API cleanup verification](AsyncComputeStep5Verification.md#pass-ordering-api-cleanup) for the updated scope and results.

## Allocation safety

The materializer uses the prepared schedule's reuse policy. Any schedule containing multiple logical queue classes disables physical-object reuse based on linear first/last-use intervals. This is deliberately conservative even when two logical queues alias one native queue. Single-queue schedules retain the existing optimization and the caller's `SetOptimizations()` setting.

The decision precedes physical allocation. A pooled object's prior graphics state can change default transfer placement after materialization, but a compute-plus-default schedule already has reuse disabled. The refreshed plan verifies that a multiple-queue schedule has no linear reuse. Reset/rebuild recomputes this policy; a prior multiple-queue graph does not permanently disable later single-queue reuse. Between-build pool retirement still uses Step 2's completion and pending-submission gates.

## Verification results

The results below record the original Step 4 run. The later API cleanup removed two handle-specific tests (four parameterized cases); the linked follow-up records the current test counts.

MSVC x64 debug builds passed for `ConfigLoaderTest`, `VulkanRHITest`, `RenderCoreTest`, `VulkanRHIIntegrationTest`, and `scene_renderer_demo`.

| Coverage | Result |
| --- | --- |
| ConfigLoaderTest | 7 passed |
| VulkanRHITest | 35 passed |
| RenderCoreTest | 346 enabled tests passed; 7 existing benchmarks disabled |
| VulkanRHIIntegrationTest | 256 passed; no skips |
| Demo smoke matrix | 8 runs passed, 64 frames each: voxelizer `auto`/`comp` × RHI thread `0`/`1` × async policy `0`/`1` |

Total: **644 enabled tests passed**. Native integration and all eight smoke runs reported zero `VUID-` messages, zero `SYNC-HAZARD` messages, and no memory leaks. RHI and RenderCore unit binaries also reported no leaks. The existing external GLI macro-redefinition warning remains.

The remaining 42 cases in [RDGScheduleTests.inl](../ZenSamples/RenderCoreTest/RDGScheduleTests.inl) run in both CPU execution modes and cover:

- Acyclic graphics → compute → graphics groups, resource summaries, and boundaries.
- A shared reset/compute group, independent graphics without a compute wait, and a dependent graphics consumer.
- A producer whose signal must not be delayed by unrelated ready graphics work.
- Distinct physical buffers and textures for independent multiple-queue allocations; retained reuse for graphics-only and compute-only schedules.
- RAW, WAR, and WAW edges across repeated reads/writes, plus all-reader layout changes and initial-layout establishment.
- Owned plan snapshots, external-state refresh, default transfer-to-graphics fallback, and resetting reuse policy across graph builds.
- Indirect-command access scopes, produced-element contracts, extraction roots, and bindless scene-texture producer dependencies.
- Policy/capability fallback before allocation and compute/transfer queue aliasing without losing order or boundary requirements.
- A second allocation failing with rollback and no extraction publication.
- A pooled texture with prior graphics state changing placement after allocation without unsafe reuse.

The full native suite retained deterministic compute handoff/readback, allocation sharing, upload, lifetime, queue-selection, and failure coverage. Smoke runs exercised mode switching, resize, minimize/restore, and shutdown with synchronization validation enabled. Temporary voxelizer settings were restored byte-for-byte; matching SHA-256 hashes are recorded in the smoke evidence.

Formatting, whitespace checks, and the RDG backend-access audit passed. Compilation and planning introduce no direct `GDynamicRHI`, submission-progress queries, or submission waits into `RDG*`/`RenderGraph*` implementation files.

## Step boundary and reproduction

The new groups are planning data. Their boundary requirements are not executable native barriers. The current recording/submission path still executes the graph through the existing graphics/transfer list, and startup still reports native async scheduling as pending. Step 5 supplies group recording and queue-valid synchronization; later steps connect submission ownership and voxelizer annotations. This verification establishes schedule/allocation correctness and regression coverage, not GPU overlap or performance gains.

From the repository root, after entering the x64 MSVC developer environment:

```powershell
cmake --build build/x64-windows-msvc-debug --target ConfigLoaderTest VulkanRHITest RenderCoreTest VulkanRHIIntegrationTest scene_renderer_demo -j 8
.\bin\RenderCoreTest.exe --gtest_filter=*RDGSchedule*
.\bin\ConfigLoaderTest.exe
.\bin\VulkanRHITest.exe
.\bin\RenderCoreTest.exe
python build/rhi-phase4/run_validation.py ../async-compute-step4-integration.log --gtest_output=xml:build/async-compute-step4-integration.xml
python build/async-compute-step4-smoke.py
```

The local native helper isolates RTSS without changing persistent overlay settings or disabling validation. The smoke helper restores the original configuration after temporarily selecting each voxelizer.

Evidence is under `build/async-compute-step4-*`: `build.log`, `{focused,config,rhi,rendercore,integration}.{log,xml}`, `scene-{auto,comp}-{0,1}-{0,1}.log`, and `smoke.json`. Other platforms were not run.
