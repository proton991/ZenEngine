# RHI production readiness: R21 verification

Status: implemented 2026-10-03.

Required backend initialization and unsupported API/device profiles use release-active verification: write the expression, source and reason, flush stderr, then abort. No exception handler is needed. Device selection includes the rejected GPU names and their unsupported-feature reasons in the diagnostic.

`SetVerificationReporter` lets an application install a CPU/platform reporter without coupling core error handling to the renderer. The interactive Windows scene demo installs `MessageBoxA`; it also displays the first terminal frame cause before normal teardown. Automated smoke/frame-limited runs omit modal dialogs so failures remain observable by the runner. Startup abort and runtime frame shutdown are separate contracts.

`VulkanProductionFailureDeathTest.DeviceSelectionReportsAnUnsupportedProfile` forces an impossible bindless profile and checks the device-selection diagnostic. Existing invariant death tests execute in Debug and Release. The native message-box code was compiled in both builds; an interactive dialog was not manually dismissed during automation.

See [RHIErrorHandlingVerification.md](RHIErrorHandlingVerification.md) for the complete build/test results and remaining hardware limits.
