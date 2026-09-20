# Synchronization simplification plan

Status: Proposed on 2026-09-20 after reviewing the current async-compute changes. This document does not implement the refactor. Complete the five core phases in order; reassess the optional submission-handle redesign afterward.

## 1. Objective and scope

Reduce duplicated synchronization state, representations, and execution paths across RenderCore and RHI while preserving current rendering and failure behavior. Prefer deleting unused data and reusing existing mechanisms over adding abstractions.

The baseline is the current working tree, including the async-compute implementation described in [AsyncComputeImplementationPlan.md](AsyncComputeImplementationPlan.md) and its step verification reports. Some duplication, particularly the confirmed resource-state snapshots, predates that implementation.

The command-list pool policy consolidation and the `CreateBuffer` / `CreateTexture` naming refinement are already present. Preserve those changes.

This work does not change async-compute defaults, queue-placement policy, resource-sharing policy, CPU submission depth, or transient allocation reuse policy. It does not introduce another worker, migrate to Synchronization2, redesign presentation, or implement the separate [RHI error-handling plan](RHIErrorHandlingPlan.md). Existing performance acceptance remains open; simplification alone does not establish a GPU performance improvement.

## 2. Responsibilities to preserve

| Concept | Owner and responsibility | Target treatment |
| --- | --- | --- |
| Queue identity and capabilities | RHI describes logical queue classes, native sharing, and supported operations. | Reuse one queue enum and one authoritative topology snapshot. |
| Graph schedule | RenderGraph assigns passes to groups and preserves their dependency order. | Keep groups, pass membership, required dependencies, and allocation-reuse constraints; remove unused or derived payloads. |
| Resource access state | `ResourceStateTracker` tracks visibility, layouts, and initialized contents. | Keep the active tracker and execution-transaction rollback state. Remove unused frame snapshots. |
| Submission history | `RenderSubmissionHistory` tracks exact writers/layout transitions and concurrent readers across graphs. | Keep its responsibility and transactional prepare/commit behavior. |
| Producer reference | `RHISubmissionPoint` identifies an accepted queue/serial or a pending batch group. | Keep exact references; do not replace them with the latest submitted serial. |
| CPU submission result | `RHISubmissionTicket` reports completion of CPU processing, including failures. | Keep distinct from GPU completion; consider consolidating its shared state only in the optional phase. |
| GPU completion requirement | `RHICompletionSet` holds one required serial per logical queue. | Reuse for frames, pools, uploads, and batch retirement. |
| Pending resource retirement | RenderCore protects reuse before a pending frame has acquired native serials. | Use a completion set plus one pending ticket under the current submission-depth invariant. |
| Native lifetime tracking | `VulkanLifetimeTracker` protects descriptor pools, uniform storage, and recording epochs. | Keep backend-local ownership and recording counts. |
| Native synchronization | Vulkan queue timelines, fences, barriers, and presentation synchronization implement backend mechanisms. | Preserve their distinct lifetimes and semantics. |

The schedule group, borrowed RHI submission input, and owned command-batch group have different ownership roles. Sharing a word such as "group" is not sufficient reason to merge them. Similarly, the Windows-aware `RHIThreadEvent` wait is not redundant with the ticket's future.

## 3. Required invariants

1. CPU handoff, native acceptance, GPU completion, and presentation retirement remain distinct events. A ready ticket does not authorize GPU resource reuse.
2. Compare serials only within their logical queue timeline. Native queue aliases can share barrier history without sharing serial values.
3. Preserve exact producer dependencies. An overwrite waits for all relevant readers; a layout transition establishes an ordered producer for subsequent readers.
4. Same-native-queue dependencies retain required ordinary barriers. Different-native-queue dependencies retain their waits and queue-valid access/stage scopes.
5. Prepare and record privately. Publish resource history and extractions only at their existing successful handoff/confirmation boundaries. Rejected preparation cannot publish a successful producer.
6. A failure after partial or uncertain native execution blocks dependent work and retains potentially used objects through valid completion or terminal teardown.
7. Frame slots, staging storage, and pooled resources cannot be reused while any required queue or pending submission remains unfinished. Compute work without a graphics join still participates in retirement.
8. RenderGraph compilation and recording do not query backend progress, submit native work, or wait for GPU completion. Resource materialization and retirement continue through RenderDevice services.
9. Multi-queue graphs retain the current conservative prohibition on linear transient allocation reuse.
10. Preserve the CPU-only recording harness and its transactional behavior.

Apply the user's C++ rules to every modified function: one final return, explicit types except iterators, short necessary lambdas, engine containers, project formatting, shared logic, and explicit failure handling without new exception-based control flow.

## 4. Core implementation phases

### Phase 1 — Remove unused state and schedule payloads

Evidence:

- [RenderDevice.cpp](../ZenCore/Source/Graphics/RenderCore/V2/RenderDevice.cpp): `ExecuteFrameGraph()` copies the active tracker into `PendingFrame::scheduledState`; `CompleteFrame()` copies that into `m_confirmedResourceState`. Production code only copies, resets, and invalidates these snapshots. It never restores them or reads them to decide execution.
- [RenderGraph.cpp](../ZenCore/Source/Graphics/RenderCore/V2/RenderGraph.cpp): `BuildScheduleResources()` builds `RDGScheduledResource` summaries and `RDGScheduleBoundary` records. Their remaining consumers are construction logic and tests; production barrier generation and history preparation traverse the actual accesses separately.
- `ExecuteScheduledGraph()` copies `update.schedule` into `plan.schedule`, although subsequent history commit uses the resource delta, revision, state, and external points rather than `update.schedule`.

Changes:

1. Remove `PendingFrame::scheduledState`, `m_confirmedResourceState`, their full tracker copies, and their invalidation/reset work. Preserve active tracker invalidation, submission history invalidation, and execution-local rollback copies.
2. Remove `RDGScheduledResource`, `RDGScheduleBoundary`, `RDGSubmissionGroup::resources`, `RDGSubmissionGroup::boundaries`, `BuildScheduleResources()`, and its now-unused `GetScheduleAccess()` helper.
3. Retain `GetInitialScheduleAccess()`: transfer-queue eligibility also uses it. Retain actual compiled transitions, initial access provenance, graph dependency validation, and the allocation-reuse guard.
4. Move `update.schedule` into `plan.schedule` after successful preparation. Retain the private schedule copy made by history preparation until its transactional ownership is deliberately redesigned.
5. Update test inspection helpers to inspect the remaining live state. Replace assertions about deleted summaries with assertions about dependency order, recorded barriers, or resource reuse behavior where those assertions provide meaningful coverage.

Completion gate: existing schedule, recording, external invalidation, failed-frame, and resource-lifetime tests pass. Confirm by production call-site search that no removed field had an execution consumer. Preserve tests proving invalidated resources cannot regain active history after pending work finishes.

### Phase 2 — Unify queue vocabulary and derive dependency metadata

Evidence: [RDGSchedule.h](../ZenCore/Include/Graphics/RenderCore/V2/RenderGraph/RDGSchedule.h) repeats the queue classes from [RHICommandList.h](../ZenCore/Include/Graphics/RHI/RHICommandList.h). The executor caches sharing/dependency flags in addition to `RHIQueueCapabilities`. Native equivalence lookup is repeated in RenderGraph and RDGMetrics. Submission consumes all predecessor IDs, while separate semaphore subsets primarily serve validation and diagnostics.

Changes:

1. Use the existing `RHICommandContextType` as the canonical assigned-queue enum. Remove `RDGQueue` and ordinal casts between equivalent enums. Keep `RDGQueuePreference` and eligibility reasons separate. A broad rename of `RHICommandContextType` is unnecessary for this refactor.
2. Use `RHIQueueCapabilities` as the authoritative immutable topology/dependency snapshot. Remove duplicate executor fields `m_sharedTransfer` and `m_asyncSubmissionDependencies`. Replace redundant virtual capability answers with snapshot-derived answers; retain a convenience method only when it still serves callers without maintaining another source of truth.
3. Update backend implementations and test doubles to populate the complete snapshot consistently. Do not combine a default snapshot with independently overridden legacy flags. Keep per-queue copy/stage legality information; it describes supported operations rather than queue identity.
4. Centralize the representative logical-index lookup for native queue equivalence. Use it in barrier planning and metrics. Never use that representative to merge logical completion serials.
5. Keep `predecessors` and `externalPredecessors` as the stored dependency lists. Remove `semaphorePredecessors` and `externalSemaphorePredecessors`; derive the classification from the producer queue, consumer queue, and captured capabilities where diagnostics need it.
6. Preserve exact external producer IDs and queue provenance so diagnostics can derive external dependency classification without reading native counters. Preserve validation of IDs, topological order, producer status, and resource eligibility; remove only consistency checks for deleted duplicate arrays.
7. Remove the editable per-group `waitStages` field if its sole value remains `ALL_COMMANDS`. Diagnostics should report the implemented RHI wait policy, with coverage that prevents the report and backend behavior from drifting. This phase does not narrow native wait stages.

Completion gate: dedicated queues, same-family distinct queues, and aliased native queues retain their existing routing and synchronization. Metrics still distinguish ordering edges from cross-native-queue dependencies. Unsupported async capability and timeline-disabled fallback tests pass. All backends and test doubles compile against one capability contract.

### Phase 3 — Use one submission path for one or many groups

Evidence: [RenderDevice.cpp](../ZenCore/Source/Graphics/RenderCore/V2/RenderDevice.cpp) contains separate dependency resolution, submission acceptance publication, and history commit logic in `SubmitRecordedGraph()` and `SubmitRecordedGroups()`. The single-list frame adapter already demonstrates packaging a list as one group.

Changes:

1. Route standalone single-list submission through the existing one-group input and grouped executor path. Remove manual RenderCore calls that queue, accept, and complete `RHISubmissionState` after separately submitting native commands.
2. Keep one place that translates schedule dependencies into RHI producer points, one executor path that publishes accepted group serials, and one RenderDevice path that commits or invalidates history.
3. Preserve synchronous CPU processing for standalone graphs. A standalone submission must not end the native frame. Frame submission continues to own presentation and exactly one successful native `EndFrame`.
4. Preserve the current timeline-disabled transfer completion wait, frame retirement stamping, command-list reset/rollback ownership, and upload retry behavior. Do not remove `SubmitImmediateTransferCmdList()` merely because one caller moves; first account for its remaining callers and fallback work.
5. Migrate the inline frame fallback to the common frame submission machinery only after matching its rejection and presentation behavior. Inline versus threaded execution is an executor mode, not a reason to retain duplicate dependency/history algorithms.
6. Keep the existing single-list and grouped barrier-generation entry points during this phase. Unifying submission does not authorize replacing barrier algorithms, changing read serialization, or making the CPU recording harness depend on native submission.
7. Remove obsolete adapters only when their ownership or fallback responsibilities have been transferred. Keep small frame/standalone adapters when they express a real lifecycle difference.

Completion gate: verify the behavior table below in inline and threaded modes where applicable.

| Case | Required result |
| --- | --- |
| One graphics group, one transfer group, multiple scheduled groups | Same queue routing and resource ordering as the baseline. |
| Invalid input or rejected handoff | No partial command detachment or history publication. |
| Standalone first native rejection with no accepted work | Preserve the existing retryable rejection behavior. |
| Queued frame failure | Consume its result and invalidate speculative state; do not publish extractions. |
| Failure after an accepted group | Preserve accepted serials, block dependent work, retain GPU-owned resources. |
| Timeline-disabled transfer | Preserve the existing synchronous completion fallback. |
| Frame presentation/recreation | Preserve return values, recreation handling, and native frame-boundary ownership. |
| Empty frame schedule | Preserve presentation/frame processing without manufacturing compute work. |

### Phase 4 — Simplify resource retirement

Evidence: `CaptureResourceRetirement()` captures tickets only from RenderDevice's pending frames. `SubmitRecordedGroups()` drains prior pending frames before a new frame handoff, and `ExecuteFrameGraph()` is the sole pending-frame insertion path. The general RHI executor itself can queue more than one batch.

Target representation: RenderCore resource retirement contains the existing `RHICompletionSet` plus one optional `RHISubmissionTicket`. The completion set protects already-submitted work; the ticket protects a pending CPU submission until its serials are known.

Changes:

1. Recheck and enforce the at-most-one pending frame invariant after Phase 3. Include release-build handling for an unexpected violation; never silently discard extra tickets. Keep general RHI multi-batch support unchanged.
2. Replace `RHIRetirementRequirement::pending` with one ticket for this RenderCore policy. Move the retirement policy out of RHI and name it `ResourceRetirement` in `zen::rc`; share its declaration between frames and resource pools without introducing a RenderCore dependency into RHI.
3. Share the logic that resolves a ready ticket into required queue serials and handles failure. Frame reuse may first wait for CPU processing; pool readiness checks remain nonblocking. Avoid repeatedly copying the complete `RHIBatchResult` merely to read its retirement serials where the existing result lifetime permits safe access.
4. Fold a successfully resolved ticket's serials into the completion set before releasing the ticket reference. Failed or uncertain work must remain protected by the terminal-failure gate; an empty ticket must never make it look safe to recycle.
5. Preserve the initial completion snapshot even when a pending ticket exists. Do not replace multi-queue retirement with a frame number, the graphics serial, or ticket readiness alone.
6. Remove the `StagingCompletion` alias and use `RHICompletionSet` directly. Uploads already establish native acceptance before assigning staging retirement serials; do not add pending-ticket machinery to that path.
7. Retain native recording counts and lifetime epochs in `VulkanLifetimeTracker`. They protect different objects and unsubmitted native recordings that RenderCore frame tickets do not describe.

Completion gate: delayed compute with completed graphics/transfer cannot recycle a frame or resource; pending CPU work prevents pool reuse; resolving a ticket still requires GPU completion; failed/uncertain work stays retained; upload overwrites respect earlier readers. Keep the test that the RHI executor can queue multiple batches independently of RenderDevice's single-pending-frame policy.

### Phase 5 — Clarify completion names and reconcile documentation

Changes:

1. Keep `RHICompletionSet` as the shared per-queue value type. Give variables and fields directional names: submitted snapshot, completed snapshot, and required serials. For example, `RHIBatchResult::completion` represents submitted serials required for retirement, not observed GPU completion.
2. Rename `RHISubmissionState::Complete()` / `IsComplete()` to explicitly describe finishing CPU submission processing. Preserve the distinction between an individual accepted group and a successfully finished whole batch.
3. Rename the GPU-wait contract `WaitForSubmission()` to `WaitForCompletion()` across declarations, implementations, and callers. Keep ticket `Wait()` documented as waiting for CPU processing/results. Do not collapse snapshot reads, progress polling, and blocking completion waits into one ambiguous method.
4. Use consistent terminology in diagnostics and tests without changing their meaning or silently weakening coverage.
5. Update the active async implementation plan's representation requirements and the affected metrics documentation. Historical verification reports remain records of their original runs; annotate superseded representations and link the simplification verification rather than rewriting old test results.
6. Update [RHI/README.md](../ZenCore/Include/Graphics/RHI/README.md), including its stale description that automatic async scheduling is unimplemented and its older submission-history description.
7. Record what actually changed and which validation ran. Keep unresolved async performance acceptance separate from structural completion.

Completion gate: no obsolete names or removed representations remain in production declarations/callers; current documentation describes the implemented ownership and completion contracts. Changed C++ passes project formatting and whitespace checks.

## 5. Optional follow-up — One shared submission owner

After the core phases, reassess whether exposing both `RHISubmissionState` and `RHISubmissionTicket` still adds enough complexity to justify a broader API change. This phase is not required to finish the core simplification.

The possible target is a ticket/handle referencing one shared submission record. A producer point selects a group in that record, or carries a concrete accepted queue/serial. Executor-only operations publish acceptance and final status; RenderCore cannot manually manufacture accepted state. Reuse existing result types rather than adding another status/result hierarchy.

Design requirements before implementation:

- Preparation needs provisional group references before handoff. Preserve the difference between prepared and queued handles; merely allocating a handle must not make it a committed producer.
- Resolve accepted earlier groups without waiting for the entire batch. Consumers in the same batch cannot wait on its own final ticket.
- Preserve a later batch failure's ability to invalidate earlier accepted producer references until whole-batch success is confirmed.
- Preserve exact queue/serial results, presentation results, and failure propagation. One shared owner does not mean one undifferentiated completion event.
- Keep Windows message-aware waits. Complete every admitted job's result/event exactly once.
- Keep shared submission state independent of command arenas and retained resource lists. Producer history must not retain GPU command storage indefinitely or create an ownership cycle.
- Coordinate result-publication changes with the separate error-handling plan if that work lands first; do not create competing submission result systems.

Proceed only if this removes independently managed state and caller obligations. Hiding the existing implementation behind additional wrappers without deleting duplication is not a useful outcome. Validate provisional references, same-batch dependencies, queued cross-batch consumers, late failure, and teardown before replacing the current public state API.

## 6. Validation and delivery

Use existing coverage first, especially `RDGScheduleTests`, `RDGGroupRecordingTests`, `RDGScheduledSubmissionTests`, `RDGSubmissionHistoryTests`, `RDGSubmissionFailureTests`, `AsyncComputeLifetimeTests`, `AsyncUploadTests`, and `RHIThreadingTests`. Add or adjust tests only for a real invariant or changed behavior, not to reproduce deleted internal structures.

| Scope | Validation |
| --- | --- |
| Each core phase | Build affected targets and run the relevant existing CPU suites; inspect the diff and production callers. |
| Queue/submission/retirement changes | Run `VulkanRHITest` and `VulkanRHIIntegrationTest`, including upload dependencies, queue aliases, delayed compute, and partial failures. Use synchronization validation for native execution. |
| Final regression | Build `RenderCoreTest`, `VulkanRHITest`, `VulkanRHIIntegrationTest`, and `scene_renderer_demo`; run the full applicable suites. |
| Renderer behavior | Exercise voxelizer `auto`/`comp`, RHI thread `0`/`1`, and async policy `0`/`1`, including mode changes, resize/minimize, and shutdown. Check rendered behavior, validation output, and leaks. |
| Unsupported hardware paths | Use existing capability/topology fixtures; report which native configurations were actually available. |

Keep each phase independently reviewable and verify its completion gate before building further changes on it. Preserve the user's working tree and staging. Do not stage or commit as part of this plan.

Core completion means the unused copies and payloads are gone, queue topology has one source, submission acceptance/history publication use one implementation, retirement uses the smallest representation justified by current RenderDevice behavior, and the required ordering/lifetime/failure tests still pass. Report measured performance only if a comparable baseline and changed run were collected.
