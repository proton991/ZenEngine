# RHI production readiness: R13 verification

Status: consumer-gated, 2026-10-02. No feature API was added speculatively.

R13 explicitly says to add each feature only when a consumer needs it, together with a test and README contract. Non-indexed/indirect-count draws, public queries, dynamic stencil reference, mesh shaders, HDR and synchronization2 have no new consumer specified by this task. Ray-query lighting has its own [implementation plan](HardwareRayQueryEnvironmentLightingPlan.md); enabling extensions is not an acceleration-structure implementation or a completed consumer.

The [production plan](RHIImprovementPlan.md#r13-missing-api-features) retains the feature list and its condition. Implement each through its consuming renderer change. The [shared verification record](RHIProductionVerification.md) validates the current supported feature set only.
