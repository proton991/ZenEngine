# RHI production readiness: R10 verification

Status: Gated; not implemented, 2026-10-02. Part of [section 8](RHIImprovementPlan.md#8-production-readiness-todos).

## Result

Asynchronous pipeline compilation/warmup must follow R10's cold-driver-cache measurements. No global driver cache or driver setting was changed. Warmed steady-state profiles cannot establish this gate, and a new incomplete-pipeline draw policy would change rendering behavior.

## Verification and limits

The earlier deferred-work record reports initial PBR/GI frames of 181/561 ms versus warmed 27.9/189.4 ms, but lacks the required reproducible cold per-frame creationCPUUs runs across glTF assets and runtime GI reconfiguration. That evidence suggests a candidate, not completion of the new gate. Collect those runs before choosing a load-time warmup or asynchronous draw policy.

The [shared production verification record](RHIProductionVerification.md) contains the exact environment, full Debug/Release counts, smoke/capture results and paired frame measurements. Local build/test artifacts are under [build/rhi-production/](../build/rhi-production/). These results apply to the complete working tree, including the user's pre-existing R5 and deferred work; they do not attribute a measured cost to this item alone.
