# Async compute Step 6 verification

Implemented and verified on 2026-09-20. This step replaces resource submission history and prepares it for the owned multi-queue handoff in Step 7.

## Resource history and exact producer points

`RenderDevice` now owns `RenderSubmissionHistory`, indexed by physical resource stable ID. Texture views use their backing texture identity. Each entry retains its last writer or layout transition and the latest outstanding reader on each logical queue. Compatible readers can overlap; an overwrite or layout change depends on every outstanding reader. Successive readers on one logical queue accumulate their stages and access scopes, including different image usages sharing one layout.

Native queue equivalence controls semaphore requirements and ordinary barriers. It does not merge logical timeline values: compute serial 19 and transfer serial 2 remain separate producer points even when their wrappers share a native queue. The group recorder accepts multiple initial scopes for one resource, merges local scopes for aliases, and keeps foreign accesses out of local barriers. Metrics retain these scopes and the ordered image layout across groups.

`RHISubmissionPoint` represents either an accepted queue/serial pair or an owned `RHISubmissionState` plus group ID. Preparation adds exact current-frame group dependencies and retains earlier frame references. The RHI worker resolves queued predecessors before translating their consumer. Neither the history nor the recorder queries backend counters or guesses future serials. The new points reject `kLatestSubmitted`.

The existing single-list frame batch now owns its submission state and predecessor references. Its producer serial is captured after the graph submission and before the separate presentation copy. Unrelated later submissions cannot change that producer point. Same logical queue dependencies still resolve and validate the producer, while avoiding an unnecessary native self-wait.

## Publication, invalidation, and uploads

History preparation produces a private delta. Recording alone publishes nothing. Publication requires a queued owned batch and the same history revision; rejected handoffs leave previous history intact. Accepted group results remain referenced until the whole batch succeeds, so a later failure cannot erase the provenance needed to invalidate speculative history. Successful results then become concrete queue/serial points. Failed queued producers stop dependent native execution and clear the speculative history.

External invalidation and destroyed-resource cleanup remove the new records. External invalidation also removes the resource from pending frame snapshots, preventing a later frame confirmation from restoring invalidated state. Texture-view and backing-texture accesses share the same invalidation boundary.

An uploaded resource adds a dependency only to groups using it. An independent graphics group receives no upload wait merely because a compute consumer needs one. A subsequent upload overwrite waits for both graphics and compute readers. The conservative single-list production path uses the same exact history, retaining read/read chaining while it still records barriers with one global tracker.

## Verification

All five MSVC x64 Debug targets built successfully: `ConfigLoaderTest`, `VulkanRHITest`, `RenderCoreTest`, `VulkanRHIIntegrationTest`, and `scene_renderer_demo`.

| Coverage | Result |
| --- | --- |
| ConfigLoaderTest | 7 passed |
| VulkanRHITest | 35 passed |
| RenderCoreTest | 394 enabled tests passed; 7 existing disabled benchmarks |
| VulkanRHIIntegrationTest | 265 passed; no skipped tests |
| Total | **701 enabled tests passed** |
| Scene smoke matrix | **8 runs × 64 frames passed** |

This adds 24 CPU cases and 4 native cases to the previous 673-test baseline. The scene matrix covers voxelizer `auto`/`comp`, RHI thread `0`/`1`, and async policy `0`/`1`, including the existing mode-switch and resize/minimize sequence. Final logs contain zero VUIDs, zero `SYNC-HAZARD` reports, and no reported leaks. Formatting and whitespace checks passed; added C++ lines introduce no `std::array`, and the new history implementation uses explicit types and final returns. Direct backend/progress-query audits passed for RDG, its resource manager, and history preparation.

New CPU coverage includes:

- Independent readers, reader scope accumulation, last-writer retention, layout-transition ordering, and compatible image usages.
- Exact current-frame and earlier-frame references, separate logical timelines on aliased native queues, and local barriers alongside foreign waits.
- Group-specific upload dependencies and a later real upload waiting on both graphics and compute readers.
- Private preparation, rejected/stale deltas, accepted-prefix failure, and a queued dependent batch stopped before native execution.
- Physical texture-view identity, external invalidation, and pending frame confirmation after invalidation.
- No backend progress queries during history preparation or group recording, and no host completion wait for timeline-based dependencies.

Four new native cases cover inline/threaded execution with timeline dependencies enabled/disabled. A compute-queue copy produces deterministic bytes, an unrelated compute submission advances its timeline, and graphics consumes the first producer through an owned point. Native wait capture verifies the exact earlier value, with zero host timeline waits on the asynchronous path; readback matches the source bytes. These cases require a distinct native compute queue and explicitly skip when unavailable. Existing native dispatch, image, indirect-command, queue-alias, same-family, lifetime, and failure tests remain part of the full run. Native progress-failure tests now also verify that the owned producer point becomes failed after submission.

Layout transitions remain ordered after all old-layout accesses and before new-layout consumers, following the [Vulkan synchronization specification](https://docs.vulkan.org/spec/latest/chapters/synchronization.html). Native validation and deterministic readbacks provide the verification evidence; successful submission alone is not treated as proof.

Evidence is under `build/async-compute-step6-*`: build log; `config`, `rhi`, `rendercore`, and `integration` logs/XML; `native-focused` log/XML; eight scene logs and `smoke.json` with configuration hashes. The original configuration bytes are restored after the scene matrix. The isolated native launcher keeps synchronization validation enabled without changing persistent overlay settings.

## Next boundary

At the Step 6 verification boundary, the production renderer still recorded one graphics list per frame, while the group harness exercised parallel history and barrier inputs. [Step 7 verification](AsyncComputeStep7Verification.md) documents the subsequent owned multi-queue handoff and production scheduler connection. Production voxelizer queue preferences remain in Step 9. Step 6 did not claim production compute overlap or complete the later repeated-voxelization and performance gates.
