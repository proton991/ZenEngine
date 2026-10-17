# Pipeline compilation follow-up

Status: proposed, 2026-10-17; deliberately not implemented by the RHI review fixes.

The review's O1 probe on the RTX 5080 found a roughly 162 ms `HybridTemporalSP`
creation after startup and about 11 ms for entering voxel debug. Warming
`HybridTemporalSP`, `ResetDrawIndirectSP`, and `VoxelPreDrawSP` through the existing
RenderDevice cache during renderer initialization removes those driver compilation
hitches. See [the verification record](RHIReviewFixVerification.md) for measurements.

A deliberately delayed first entry into GI still creates 22 other pipelines:
about 20 ms of pipeline creation in a 33 ms CPU frame. That remaining case opens
the review plan's separate asynchronous-compilation design step. Broader eager
warmup would compile many settings/provider variants that a session might never use.

## Proposed ownership and execution

1. Keep shader creation, resource factories, ordinary native object creation,
   wrapper publication and destruction on the RHI thread. Add one explicit exception
   to the current invariant: a compiler worker may call `vkCreateGraphicsPipelines`
   or `vkCreateComputePipelines` with immutable, retained shader inputs and an owned
   copy of the complete pipeline description. It must never access command contexts,
   descriptor sets, frame state, or mutable RenderCore objects.
2. The worker uses the device's existing `VkPipelineCache`, created without the
   externally-synchronized flag. Retain the device/cache lifetime until every job
   has finished; shutdown stops admission, drains/cancels jobs, then destroys the
   cache/device. A never-published native pipeline is destroyed on the RHI thread.
3. Return a fixed-size native result internally. On the RHI thread, publish a fully
   initialized `VulkanPipeline` wrapper and deliver `RHIResult<RHIPipeline*>` with
   the original native error on failure. No exceptions, null success values, or
   half-initialized cache entries. Device loss uses the existing terminal path.
4. Extend RenderCore's pipeline-cache entries with explicit pending, ready and
   failed states, retaining the current complete cache key and failure backoff.
   One pending job exists per key. A generation invalidates publication after
   shader replacement, resize/cache invalidation or renderer destruction; an obsolete
   result is retired, not attached to a new key or renderer lifetime.
5. RenderCore selects fallback behavior before graph compilation. For an optional
   GI pass, use the already available PBR path until the entire required pipeline
   set is ready. Skip diagnostic-only passes if their pipeline is pending. Required
   output cannot be silently omitted: initialization may wait explicitly, while
   interactive frames retain a valid prior/fallback rendering configuration.
   RDG records only ready pipelines and keeps its existing resource/dependency rules.

## Acceptance before implementation can ship

- Deterministic fake-backend tests for request deduplication, pending fallback,
  failed compilation/backoff, cancellation, invalidation, late completion and shutdown.
- Native failure injection for host/device OOM and device loss, with original causes
  preserved and every partial native output released exactly once.
- Validation-enabled threaded/inline and async-compute matrix, including shader
  replacement, resizing and destruction while compilation is pending.
- Compare delayed GI entry and settings switches against the synchronous baseline:
  CPU frame p95/max, compilation duration, fallback frames, initialization cost and
  memory. Keep this design only if the hitch reduction justifies the new lifecycle.
- Inspect the Vulkan specification for each enabled cache/device synchronization
  contract before implementing the worker exception. Do not infer thread safety of
  the surrounding engine wrappers from Vulkan's native cache synchronization.
