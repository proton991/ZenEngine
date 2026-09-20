# Async compute Step 5 verification

Implemented and verified on 2026-09-20. Stopped before Step 6 for user verification.

## Recording and ownership

`RDGExecutor::ExecuteGroups()` provides a CPU recording harness that returns the recorded schedule alongside caller-owned command lists. Lists are indexed by group ID and must have the assigned logical context. Duplicate lists and incorrect contexts are rejected before callbacks. RenderDevice supplies separate graphics, compute, and transfer pools; recycling preserves the context type, even when native queues alias.

Single-list and group execution share `ExecuteTransaction()`. All passes are prepared before recording. All lists receive checkpoints before callbacks, and callbacks run on the calling RenderCore thread in both CPU execution modes. Failure in any group restores every list's commands, resource references, and submission-dependency metadata, together with the private resource tracker and metrics. Recording alone never publishes extractions. The existing context-free single-list CPU harness remains supported.

The returned schedule carries exact predecessor group IDs. `semaphorePredecessors` contains only predecessors on different native queues; ordinary predecessors remain for aliased contexts. External state uses physical resource identity, logical queue, and an opaque dependency ID supplied by RenderDevice. `externalPredecessors` retains those IDs, including aliases, and `externalSemaphorePredecessors` identifies required waits. RDG contains no native submission serials, progress queries, or guessed future points. Missing provenance, invalid IDs, unsupported source/destination stages, or an incompatible external sharing contract reject recording before commands are appended.

## Synchronization

Barrier planning keeps access history per native queue equivalence class and image layout order across the schedule. Native aliases share access history. Different queues use schedule dependencies for memory availability while retaining any independent local hazards.

- An aliased transfer write followed by compute access retains an ordinary buffer barrier and image transition. The producer order remains in the schedule.
- A foreign buffer producer requires a semaphore boundary, without importing its stages into the consumer's barriers.
- Image transitions occur once, in the consumer prologue after its waits. The layout edges introduced in Step 4 order all old-layout users and subsequent consumers behind the transition owner.
- `RHITextureTransition` can express an explicit source-access scope independently of its old layout. This preserves local accesses while excluding accesses supplied by a foreign semaphore. Vulkan translates that scope directly and retains ignored queue-family indices.
- Consumer image transitions use an `ALL_COMMANDS` source stage to follow the initial `ALL_COMMANDS` semaphore waits with the legacy barrier API. The destination includes the actual consumer stages.
- Queue capability checks reject unsupported stage bits. Preference fallback and state-driven queue reassignment rebuild the barrier plan before recording.

The implementation follows the queue-stage requirements in the [Vulkan synchronization specification](https://docs.vulkan.org/spec/latest/chapters/synchronization.html#VUID-vkCmdPipelineBarrier-srcStageMask-06461) and the consumer-side transition model in the [Khronos semaphore synchronization examples](https://docs.vulkan.org/guide/latest/synchronization_examples.html#interactions-with-semaphores). Native validation also exercises the legacy barrier scopes directly.

The diagnostic barrier validator now maintains separate local histories for native queues during group recording, retains ordered image layouts, and honors explicit source-access scopes. It does not mistake a semaphore-supplied foreign access for an omitted local barrier.

## Verification results

All five targets built with MSVC x64 Debug: `ConfigLoaderTest`, `VulkanRHITest`, `RenderCoreTest`, `VulkanRHIIntegrationTest`, and `scene_renderer_demo`.

| Coverage | Result |
| --- | --- |
| ConfigLoaderTest | 7 passed |
| VulkanRHITest | 35 passed |
| RenderCoreTest | 374 enabled tests passed; 7 existing disabled benchmarks |
| VulkanRHIIntegrationTest | 261 passed; no skipped tests |
| Total | 677 enabled tests passed |
| Scene smoke matrix | 8 runs × 64 frames passed |

The 28 new CPU cases run in inline/threaded modes and distinct/aliased queue fixtures. They cover graphics → compute → graphics, local hazards alongside foreign waits, unique image-layout transitions and reader fan-out, queue pool recycling, clear → storage access, shader writes → shader/indirect reads, graphics vertex/indirect scopes, later-group rollback, context/list validation, missing/invalid initial provenance, graphics fallback, and barrier rebuilding after a state refresh. They also exercise the public recording harness, confirm no extraction publication, and check that prepared group recording performs no backend progress query.

Five new native cases passed with synchronization validation:

1. A producer is submitted before recording a consumer-side image transition from a graphics layout on a compute queue. The emitted source access is empty, queue-family indices remain ignored, and deterministic compute readback is `(11, 47)`. Covered in timeline and fence modes.
2. Separate contexts sharing a native compute queue perform transfer uploads followed by ordinary buffer/image barriers and a dispatch. The existing dependency API emits no semaphore wait for the alias, and readback is `(5, 47)`. Covered in timeline and fence modes.
3. A controlled second device exposes two real queue indices in one family: graphics `0:0`, compute `0:1`, transfer `0:1`. Distinct graphics/compute queues use one semaphore wait; aliased transfer/compute contexts use an ordinary barrier and no wait. Both copy/readback cases produce `0x12345678`, with ignored ownership-transfer indices. The isolated device owns its native command pools directly; this checks the selected native topology and synchronization protocol, while the group recorder is tested through the fake RHI. This case explicitly skips if the hardware lacks two queues in that family.

The regular device used graphics `0:0`, compute `2:0`, and transfer `1:0`. Final test and smoke logs contain zero VUIDs, zero `SYNC-HAZARD` reports, and no reported leaks. The smoke matrix covers voxelizer `auto`/`comp`, RHI thread `0`/`1`, and async policy `0`/`1`, including the existing mode switches and resize/minimize sequence. The smoke runner restored the exact configuration bytes it read before starting (`voxelizer=auto`).

Formatting, whitespace, documentation-link, and direct RDG backend/progress-query audits passed. Validation remained enabled; the native suite used the existing isolated launcher without changing persistent overlay settings.

Evidence is under `build/async-compute-step5-*`: build logs; `config`, `rhi`, `rendercore`, and `integration` logs/XML; the five-case `native-focused` log/XML; and eight scene logs plus `smoke.json` containing configuration hashes.

## Container preference follow-up

Replaced the async-compute additions' `std::array` storage with `SmallVector` from `Templates/SmallVector.h`: queue completion/capability tables, per-queue group resource states and validators, group scheduling indices, and the lifetime test payload. Fixed queue counts are explicitly initialized, and reset restores the completion vector's size. Updated affected test comparisons to compare elements.

The executor now keeps its atomic submitted/completed counters in owned per-queue objects stored in a `SmallVector`. Construction creates three owned objects before starting the RHI worker; their addresses remain stable and the existing acquire/release ordering is preserved. The plan now explicitly requires `SmallVector` instead of `std::array`.

Rebuilt all five targets and reran the complete verification matrix: 677 enabled tests passed (7 existing disabled benchmarks, no skipped native tests), plus all eight 64-frame smoke runs. Logs contain zero VUIDs, zero synchronization hazards, and no reported leaks. The smoke runner restored the exact configuration bytes. Formatting and whitespace checks passed, and an audit of added C++ lines and new C++ files found no `std::array` uses. Evidence is under `build/async-compute-smallvector-*`. This follow-up does not advance Step 6.

## Pass-ordering API cleanup

Removed `GetPassHandle()` from RenderGraph and both command recorders, together with `RDGPassHandle`, `AddPassDependency()`, manual dependency storage, and the explicit-order reason. Production rendering had no callers. Scheduler test helpers now add passes directly; the two tests dedicated to manual ordering and handle validation were removed (four parameterized cases).

Scheduling continues to derive RAW, WAR, WAW, and image-layout dependencies from declared resource uses. `NeverCull` and `allowCulling=false` retain side-effect passes. Existing resource-version cycle rejection, culling, stale-recorder checks, queue grouping, and transactional recording coverage remains. The plan and Step 4 documentation now describe this scope.

All five targets rebuilt. Verification passed: 7 configuration tests, 35 RHI tests, 370 RenderCore tests (including all 42 remaining scheduler cases), and 261 native Vulkan tests, for **673 enabled tests**. Seven existing benchmarks remain disabled; no native tests were skipped. All eight 64-frame smoke runs passed with the original configuration restored byte-for-byte. Logs contain zero VUIDs, zero synchronization hazards, and no reported leaks. Formatting and whitespace checks passed; removed API symbols have no remaining code references, and no new `std::array` uses were introduced. Evidence is under `build/async-compute-pass-api-*`.

This follow-up stopped before Step 6. Step 6 was subsequently implemented; see [Step 6 verification](AsyncComputeStep6Verification.md).

## Next boundary

At the Step 5 boundary, production `ExecuteRenderGraph()` and frame submission used the established single-list path, and initial-state recording represented an ordered prior producer. [Step 6](AsyncComputeStep6Verification.md) now supplies physical-resource writer/reader history, exact owned producer points, and multiple initial access scopes. Step 7 must connect the group recorder and these dependencies to an owned multi-queue batch.

No production voxelizer queue preferences were added in this step. Those remain in Step 9, after the submission and failure paths are ready. The smoke results verify current renderer behavior; they do not claim production compute overlap or complete the later repeated-voxelization/performance gates.
