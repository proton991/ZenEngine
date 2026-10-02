# RHI production readiness: R7 verification

Status: Implemented; manual acceptance outstanding, 2026-10-02. Part of [section 8](RHIImprovementPlan.md#8-production-readiness-todos).

## Result

An opt-in startup switch enables per-command-buffer start/end breadcrumbs and VK_EXT_device_fault queries. AMD buffer markers have distinct addresses; the portable path uses ordered fills. Reports identify queue, native buffer, last observed started/completed label, device and operation. Device loss blocks submissions globally. Bounded fault arrays avoid allocating the report after loss.

## Verification and limits

Both native-AMD and forced-portable marker paths submit real GPU work and report its completed label under synchronization validation. The driver-fault query is injected. This is not an actual device loss. See the shared measurement record for diagnostics cost; a controlled real-loss run is still required.

The [shared production verification record](RHIProductionVerification.md) contains the exact environment, full Debug/Release counts, smoke/capture results and paired frame measurements. Local build/test artifacts are under [build/rhi-production/](../build/rhi-production/). These results apply to the complete working tree, including the user's pre-existing R5 and deferred work; they do not attribute a measured cost to this item alone.

The diagnostic query follows the [Khronos device-fault contract](https://docs.vulkan.org/refpages/latest/refpages/source/vkGetDeviceFaultInfoEXT.html); the default tests substitute that call instead of querying fault information on a healthy device.
