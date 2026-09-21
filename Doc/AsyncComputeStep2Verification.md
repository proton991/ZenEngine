# Async compute — Step 2 verification

Historical report: the test results below describe the original step. Later synchronization representations and APIs are superseded where noted in [SynchronizationSimplificationVerification.md](SynchronizationSimplificationVerification.md); the original measurements are preserved.

Implemented and verified on 2026-09-20. Stop here for user verification before Step 3 of [the implementation plan](AsyncComputeImplementationPlan.md).

## Implemented behavior

- `RHICompletionSet` has one slot per `RHICommandContextType`, sized from `eMax`. Requirements extend by per-queue maximum; completion never compares unrelated timelines. Frame retirement, RDG pools, staging, and RHI batch retirement use it.
- `RHIRetirementRequirement` retains pending CPU submission tickets. Until a ticket resolves, resources cannot be recycled. Its accepted queue serials must then complete; fatal results remain protected until terminal device cleanup.
- RenderDevice frame reuse and deferred releases include compute, even without a final graphics consumer. Pending tickets protect both frame submissions and resources retired while an earlier frame is still queued.
- RDG pool entries cannot be acquired until their completion and pending-submission requirements are satisfied. Eviction accounting includes all queues and pending CPU work. Within-graph sequential reuse remains unchanged; the multi-queue reuse policy belongs to Step 4.
- RDG buffer/texture creation uses RenderDevice services backed by its owned RHI executor. The services transfer the initial resource owner to RDG and do not register it in RenderDevice's persistent buffer collection. Allocation failure follows the existing transactional rollback path.
- RDG retirement reads published executor snapshots in both RHI modes. Pool preparation, recording, and metrics do not poll native completion or submit work. Copy-capability validation also routes through RenderDevice; declaration-time capability services use the active RHI facade because declarations precede graph/device binding.
- Staging supports all queue requirements. An upload retains queues advanced by its submission attempt, plus accepted work from earlier failed attempts. Unrelated compute using the destination does not unnecessarily pin staging blocks; command and destination owners retain their own lifetime requirements.

GPU async scheduling remains disabled. The selected compute queue can be exercised directly through RHI in tests; RDG pass placement and frame scheduling belong to subsequent steps.

## Lifetime and boundary audit

| Site | Result |
| --- | --- |
| `RenderFrame`, `CompleteFrame`, `StampOutgoingFrameSerials` | Accepted compute serials and pending tickets contribute to the retirement gate. |
| `BeginFrame`, `ProcessPendingFreeResources` | Reuse/release waits for each queue's own requirement. Failed waits keep the slot unavailable. |
| `WaitForPreviousFrames`, resize, shutdown | Existing CPU drain and device-idle paths cover every queue; ordinary frame reuse retains scoped waits. Terminal cleanup can release uncertain failed submissions after device teardown synchronization. |
| RDG pool reuse, trim, estimated retired bytes | Completion sets and CPU tickets protect entries and retirement accounting. A pending entry causes a pool miss instead of unsafe physical-object reuse. |
| Extracted resources | Initial ownership stays with RDG, publication stays transactional, and final owner release uses RenderDevice's all-queue gate. No duplicate persistent owner. |
| Staging blocks and retained upload destinations | All queue slots are checked; outstanding unsubmitted allocations still prevent block reuse. Retry requirements merge rather than replacing prior accepted work. |
| Detached RHI command arenas | Existing owned batch storage remains until every queue in `RHIBatchResult::completion` completes. Queued batches own resources before native serial assignment. |
| Rendering layouts and shader parameters | Existing commands copy rendering layouts and parameter bytes. Referenced textures/views, buffers, pipelines, samplers, and bindless epochs stay retained by recordings/batches. No new retirement subsystem. |
| Uniform blocks, descriptor pools, bindless epochs | Existing `VulkanLifetimeTracker` tracks recording counts plus per-queue serial pairs, including compute. Reused unchanged. |
| Native command pools | Existing queue completion marks submitted command buffers reusable. Recording, rejected work, and transferred workload ownership stay covered by native lifetime tests. |
| RDG backend access | No direct `GDynamicRHI`, submission-progress getters, or submission waits remain in `RenderGraph*` or `RDG*` implementation files. Materialization requires RenderDevice for cold allocation. |

## Verification results

Windows/MSVC debug build passed for `ConfigLoaderTest`, `VulkanRHITest`, `RenderCoreTest`, `VulkanRHIIntegrationTest`, and `scene_renderer_demo`.

| Coverage | Result |
| --- | --- |
| ConfigLoaderTest | 7 passed |
| VulkanRHITest | 35 passed |
| RenderCoreTest | 268 enabled tests passed; 7 pre-existing benchmarks remain disabled |
| VulkanRHIIntegrationTest | 256 passed; no skips |
| Demo smoke matrix | 8 runs passed, 64 frames each: voxelizer `auto`/`comp` × RHI thread `0`/`1` × async policy `0`/`1` |

Total: **566 enabled tests passed**. Native integration and all demo runs had zero `VUID-` messages, zero `SYNC-HAZARD` messages, and no reported memory leaks. RHI and RenderCore unit binaries also reported no leaks. The build retains the existing external GLI `GLM_ENABLE_EXPERIMENTAL` redefinition warning.

The 16 added test cases in [AsyncComputeLifetimeTests.inl](../ZenSamples/RenderCoreTest/AsyncComputeLifetimeTests.inl) cover:

- independent timeline comparisons and reset;
- compute completion protecting pool reuse, eviction accounting, frame slots, and extracted owners;
- one-frame operation and failed-compute-wait retry in both CPU execution modes;
- staging with an actual compute owner, plus uploads that do not wait for unrelated compute;
- cold buffer/texture materialization with the global RHI pointer temporarily unavailable, proving allocation uses the device-owned service;
- descriptor fields, compiled usage, allocation type, debug names, worker thread, and exactly one extracted owner after graph retirement;
- buffer and texture allocation failure after an earlier allocation, with rollback and no state/extraction publication;
- pooled preparation/recording with unchanged backend progress-query, submission, and allocation counters;
- a CPU-gated compute ticket protecting resource references and detached command bytes before native acceptance and until compute completion.

Updated regression tests cover published fake-backend progress and prevent reuse during overlapping CPU/GPU work. Existing frame-count coverage checks compute completion for two, three, and four slots while leaving newer submissions pending.

The full native suite includes deterministic compute buffer handoffs/readback, delayed compute epoch retirement (`EveryQueueMustCompleteItsRecordedEpoch`), uniform/descriptor/bindless ownership, command-buffer reuse, and failure paths. It ran on an NVIDIA GeForce RTX 5080 with graphics `0:0`, compute `2:0`, transfer `1:0`. Step 1 topology fixtures also remained green.

The smoke matrix exercised mode switching, resize, minimize/restore, and shutdown. `auto` selected geometry voxelization; `comp` selected ComputeVoxelizer. The local `Data/engine.cfg` was restored byte-for-byte after the temporary compute-voxelizer runs. Smoke runs still use graphics for RDG compute passes and do not establish GPU overlap or performance gains. Repeated voxelization requests in the smoke driver belong to the later scheduler/voxel integration work.

## Reproduce and inspect

After entering the x64 MSVC developer environment, from the repository root:

```powershell
cmake --build build/x64-windows-msvc-debug --target ConfigLoaderTest VulkanRHITest RenderCoreTest VulkanRHIIntegrationTest scene_renderer_demo -j 8
.\bin\ConfigLoaderTest.exe
.\bin\VulkanRHITest.exe
.\bin\RenderCoreTest.exe
python build/rhi-phase4/run_validation.py ../async-compute-step2-integration.log --gtest_output=xml:build/async-compute-step2-integration.xml
```

The native helper uses the existing temporary RTSS-isolated executable copy and keeps synchronization validation enabled. It does not change persistent overlay settings.

For manual demo verification, set `voxelizer=comp` in `Data/engine.cfg`, run from `bin`, then restore your preferred voxelizer setting:

```powershell
$env:VK_LAYER_VALIDATE_SYNC = '1'
.\scene_renderer_demo.exe --rhi-thread=1 --async-compute=1 --frames=64 --smoke-test
.\scene_renderer_demo.exe --rhi-thread=0 --async-compute=0 --frames=64 --smoke-test
```

Expected: normal rendering and lifecycle changes, no synchronization errors/leaks, and startup reporting `scheduling=disabled (implementation pending)`.

Local evidence is under `build/async-compute-step2-*`: `build.log`, `test-build.log`, `{config,rhi,rendercore,integration}.{log,xml}`, and `scene-{auto,comp}-{0,1}-{0,1}.log`. Formatting, whitespace, document links, and the RDG backend-access audit passed. Other platforms were not run.
