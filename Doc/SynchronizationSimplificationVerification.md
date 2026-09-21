# Synchronization simplification verification

Date: 2026-09-20, with follow-ups through 2026-09-21. Baseline: `7171a23d` on `main`, committed and pushed to `origin/main` before the refactor at the user's request. Scope: the five core phases of [SynchronizationSimplificationPlan.md](SynchronizationSimplificationPlan.md) and the follow-ups documented below.

## Implemented phases

| Phase | Result |
| --- | --- |
| 1. Remove unused state | Removed `PendingFrame::scheduledState`, `m_confirmedResourceState`, unused schedule resource/boundary types and arrays, and `BuildScheduleResources()` / `GetScheduleAccess()`. Kept active tracking, execution rollback, compiled barriers, and transfer eligibility's `GetInitialScheduleAccess()`. Moved the prepared schedule into the execution plan. |
| 2. Unify queue metadata | Removed `RDGQueue`; schedule, history, metrics, and RHI use `RHICommandContextType`. Capabilities derive from `RHIQueueCapabilities`, without duplicate executor flags or legacy virtual overrides. Native queue representative lookup is shared. Metrics derive semaphore classification from actual predecessor lists; native submission and metrics use the fixed `RHISubmissionDependency::kWaitStage`. |
| 3. Share graph submission | Single-list graphs adapt into `SubmitRecordedGroups()` and the owned executor path. Removed `SubmitRecordedFrame()`, manual RenderCore acceptance publication, and the obsolete immediate-transfer helper. Preserved synchronous CPU processing for standalone graphs, transfer fallback waits, frame stamping, and frame/presentation ownership. |
| 4. Reduce retirement | Replaced RHI's vector-based `RHIRetirementRequirement` with RenderCore `ResourceRetirement`: required serials plus one optional ticket. Unexpected extra pending frames block reuse in release builds. Shared nonblocking resolution folds accepted serials and retains fatal work. `Wait()` exposes the ticket-owned result without copying its group array. Removed the `StagingCompletion` alias. |
| 5. Clarify contracts | Renamed CPU state completion to `FinishSubmission()` / `IsSubmissionFinished()`, GPU waits to `WaitForCompletion()`, and serial requirement fields to `requiredSerials`. Aggregate completion getters were subsequently refined into explicit cached reads and queries in the API follow-up below. Updated the active plan, metrics guide, RHI README, and historical-report annotations. |

The earlier command-list pool-policy consolidation and `CreateBuffer` / `CreateTexture` naming remain intact. Async compute remains opt-in, and queue placement, sharing, conservative allocation reuse, and CPU submission depth remain unchanged.

## Final synchronization vocabulary

| Concept | Purpose |
| --- | --- |
| `RHICommandContextType`, `RHIQueueCapabilities` | Logical queue identity and immutable backend queue relationships. Equal native queue IDs do not merge logical serial timelines. |
| `RDGSchedule` | Pass groups, their queue assignments, exact dependencies, and allocation reuse constraints. Compiled access/transition data remains the source for synchronization decisions. |
| `ResourceStateTracker` | Active resource visibility/layout/content state and execution-local rollback. |
| `RenderSubmissionHistory` | Exact writers/layout transitions and concurrent readers across graphs, with transactional preparation and publication. |
| `RHISubmissionPoint` / `RHISubmissionState` | A concrete accepted queue/serial or an exact pending group; native acceptance and later batch failure are observable separately. |
| `RHISubmissionTicket` / `RHIBatchResult` | CPU processing/result ownership, including errors and presentation. A ready ticket does not establish GPU completion. The borrowed `Wait()` result requires a live ticket. |
| `RHICompletionSet` | Per-logical-queue serial values, used as submitted/completed snapshots or required serials according to the owning field. |
| `ResourceRetirement` | RenderCore reuse protection before and after its one pending frame acquires serials. Pool checks do not block; frame reuse can wait. |
| `VulkanLifetimeTracker` | Native recording counts and lifetime epochs for backend-owned allocations and descriptors; preserved separately from RenderCore retirement. |

## Behavior preserved and test adjustments

- The inline presentation lifecycle adapter is retained. `FrameAndPresentationSubmissionFailuresDoNotPresentUnsignaledWork` requires retry after a rejected presentation copy even when the graph was already accepted. Moving that adapter wholesale into a batch whose later-group rejection is fatal would change behavior. Its graph dependency resolution, acceptance, and history publication now use the same implementation as grouped graphs.
- Owned standalone batches retain command resources until GPU completion. The repeated-extraction test now checks that in-flight work holds references, followed by exactly one extraction owner after completion. The recording-overlap test measures completed batches relative to its warm-up baseline because warm-up is now an owned batch too.
- Queue-sharing test doubles now configure the authoritative snapshot before device creation. Aliased native queues keep independent logical serial counters. Removed assertions about deleted summaries were replaced with live dependency, barrier, metrics, and retirement checks.
- The existing pending-compute test explicitly resolves/releases a ready ticket and still rejects reuse before compute completion. The existing partial-failure test checks that even maximal completed serials cannot retire a fatal pending requirement.
- The RHI executor's multiple-queued-batch tests remain. The one-pending-frame restriction applies only to RenderDevice retirement policy.

## Validation

Build configuration: Windows x64 MSVC Debug. All affected targets built: `RenderCoreTest`, `VulkanRHITest`, `VulkanRHIIntegrationTest`, and `scene_renderer_demo`. Native validation selected NVIDIA GeForce RTX 5080 with separate graphics/compute/transfer queues; the integration suite exercised timeline and forced-fence paths.

| Checkpoint | RenderCore | RHI | Vulkan integration |
| --- | --- | --- | --- |
| Phase 1 | 455 passed | — | — |
| Phase 2 | 455 passed | 35 passed | 271 passed |
| Phase 3 | 455 passed | 35 passed | 271 passed |
| Phase 4 | 455 passed | 35 passed | 271 passed |
| Final Phase 5 | 455 passed | 35 passed | 271 passed |

All 761 enabled tests passed in the final run; seven existing RenderCore benchmarks remain disabled. Phase 2–5 native runs used synchronization validation, completed without `VUID-` or `SYNC-HAZARD` diagnostics, and reported no tracked memory leaks. Local evidence is in `build/simplify-phaseN-{build,rendercore,rhi,integration}.log` and corresponding test XML files. Diff checkpoints are in `build/simplify-phaseN.patch`; build artifacts are untracked evidence, not part of the source delivery. Project `clang-format --dry-run -Werror`, `git diff --check`, and obsolete-symbol searches passed.

Original renderer regression: the automated checks reported all eight runs passed, but did not check engine error logs; the follow-up below corrects this validation gap. The matrix is `voxelizer=auto|comp` × `--rhi-thread=0|1` × `--async-compute=0|1`, 64 frames per run with `--smoke-test`. It exercised repeated voxel updates, PBR/voxel mode changes, resize/minimize/restore, and shutdown. Every run exited successfully with zero Vulkan validation/synchronization diagnostics and no tracked leaks. Compute voxelization with async enabled submitted eight compute groups in both CPU modes; disabled policy and the automatically selected graphics voxelizer submitted none. Every run reported `peak_pending=1`.

Evidence: `build/simplify-final-smoke.json` and `build/simplify-final-scene-*.log`. The runner restored the exact original `Data/engine.cfg` bytes; before/after SHA-256 is `af36be6241f743364fc7a97ca19b2ec09f872a06278049d67dff49d85ad12de7`.

### Follow-up: staging release error logs

The user reported repeated `Staging allocation does not belong to this manager` errors after the refactor. The final style adjustment changed `StagingBufferManager::Release()` from an early return to a loop break while leaving its final error check unconditional. Valid releases updated their retirement state correctly but then logged a false ownership error. The original smoke logs contain these messages; the checks above did not reject engine `[error]` / `[critical]` messages.

The fix records whether the block was found and reports the ownership error only when it was not. A zero outstanding count still reports a duplicate release, now without decrementing/underflowing the counter or altering retirement serials. `StagingReleaseLogsOnlyInvalidAllocationsAndPreservesReuse` captures diagnostics, checks valid/duplicate/foreign release behavior, and verifies GPU-gated reuse. It failed against the previous implementation and passes with the fix.

Rebuilt `RenderCoreTest` and `scene_renderer_demo`; all 456 enabled RenderCore tests passed with no tracked leaks. Repeated all eight 64-frame renderer runs with checks for engine errors as well as Vulkan validation and leaks: every run passed with zero engine errors, zero validation/synchronization diagnostics, and no tracked leaks. Configuration bytes were restored. Formatting and whitespace checks passed. Evidence: `build/staging-release-fix-before.log`, `build/staging-release-fix-rendercore.{log,xml}`, `build/staging-release-fix-smoke.json`, and `build/staging-release-fix-scene-*.log`. Unaffected RHI/native integration suites were not rerun for this local release-path fix.

### Follow-up: submission visibility and progress API names (2026-09-21)

`RHISubmissionState::Queue()`, `Accept()`, `FinishSubmission()`, and `Fail()` are now private, with production access limited to `RHICommandListExecutor`. RenderCore can prepare state and inspect producer results, but cannot publish native acceptance or final status. Existing state/history fault-injection tests use an accessor defined only in the test target. A compile-time check rejects public access to any of the four mutators.

Executor snapshot reads are now `GetCachedSubmittedSerials()` / `GetCachedCompletedSerials()`; RenderDevice's completion getter also says `Cached`. RHI completion reads are `QueryLastCompletedSerial()` / `QueryCompletedSerials()`. Queries retain their existing policy: native polling through raw Vulkan/inline execution and published progress reads through threaded execution. Only the `GetCached*` calls promise no backend calls in both modes. `PollGPUProgress()` requests refresh and `WaitForCompletion()` performs the GPU wait; no new worker or GPU wait was introduced. The [RHI README](../ZenCore/Include/Graphics/RHI/README.md) documents the complete contract.

The added parameterized regression checks cached-read query counts, inline/threaded query behavior, publication after an explicit poll, and absence of GPU waits. All four affected targets built; 458 RenderCore, 35 RHI, and 271 Vulkan integration tests passed (764 enabled tests; seven existing benchmarks remain disabled). Native synchronization validation produced no `VUID-` or `SYNC-HAZARD` diagnostics. All eight 64-frame renderer smoke runs passed with zero engine error logs, zero Vulkan validation diagnostics, and no tracked leaks. Configuration bytes were restored. Formatting and whitespace checks passed. Local evidence uses the `build/sync-api-cleanup-*` prefix.

The final staging cleanup computes the upload's advanced queue serials once, immediately after `ExecuteRenderGraph()`, and shares that requirement between the success and failure branches. The standalone comparison helper was removed; the two snapshots still bracket the submission attempt. Rebuilt `RenderCoreTest` and `scene_renderer_demo`; all 458 enabled RenderCore tests passed. Evidence: `build/staging-serial-capture-{build,rendercore}.log` and the corresponding test XML.

## Optional shared submission owner

Reassessed and deferred. Production RenderCore no longer manually calls queue/accept/finish operations to duplicate executor submission. The remaining state supplies provisional producer references before handoff, permits same-batch consumers to resolve earlier accepted groups before the final ticket exists, and lets later failure invalidate those producers. The ticket owns the whole-batch result and message-aware CPU wait. Merging them would require changing those publication and lifetime contracts, beyond the duplication removed by the core phases. No new wrapper was added to hide the distinction.

This is structural and correctness validation. No new GPU overlap/performance comparison or pixel-reference comparison is claimed. The earlier [Nsight performance acceptance](AsyncComputeNsightVerification.md) remains open; native runs cover the available device, while unsupported/shared-queue variants also rely on existing fixtures.
