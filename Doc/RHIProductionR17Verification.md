# RHI production readiness: R17 verification

Status: implemented 2026-10-03. Option B is the initial product policy.

A rejected required frame stops the executor in both inline and threaded modes, with async compute enabled or disabled. It preserves the first structured cause, emits one primary terminal diagnostic, cancels dependent jobs, and permits ordered cleanup. Submission receipts and retained GPU ownership survive a later failure. The interactive Windows demo reports the cause through a native message box and exits its render loop. Optional resource requests and explicitly discarded standalone transactions retain their local failure contracts; automatic replay/device recreation is deferred.

`FrameTicketPreservesFinalizationCauseAndBlocksNewResources` checks the native code, source line and resource ID through the ticket, progress result and terminal state, then checks that new resource creation is rejected. Existing uniform/descriptor/bindless fault-injection, accepted-prefix retirement and dependent-batch cancellation tests remain active. The native window tests check required-frame rejection in all four execution modes.

Builds, suite counts, smoke configurations and limitations are recorded in [RHIErrorHandlingVerification.md](RHIErrorHandlingVerification.md). The [README](../ZenCore/Include/Graphics/RHI/README.md) and [R12 verification](RHIProductionR12Verification.md) distinguish optional allocation failure from terminal required-frame failure.
