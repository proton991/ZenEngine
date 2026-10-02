# RHI production readiness: R16 verification

Status: Implemented, 2026-10-02. Part of [section 8](RHIImprovementPlan.md#8-production-readiness-todos).

## Result

Index-buffer byte offsets use uint64_t through geometry/graph descriptors, recorded direct and indirect indexed commands, context signatures and native VkDeviceSize binding. Index counts remain 32-bit.

## Verification and limits

A regression constructs offsets beyond 4 GiB in both commands and graph descriptors and checks preservation. Both configurations compile all fake/native context implementations. This does not allocate a physical buffer larger than 4 GiB.

The [shared production verification record](RHIProductionVerification.md) contains the exact environment, full Debug/Release counts, smoke/capture results and paired frame measurements. Local build/test artifacts are under [build/rhi-production/](../build/rhi-production/). These results apply to the complete working tree, including the user's pre-existing R5 and deferred work; they do not attribute a measured cost to this item alone.
