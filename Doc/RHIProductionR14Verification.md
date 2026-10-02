# RHI production readiness: R14 verification

Status: platform-gated, 2026-10-02. Separate presentation queues remain unsupported.

Section 8's order makes R10-R16 conditional on their gate or a consuming need. The available Windows AMD graphics queue supports the actual surface. No supported target requiring another presentation family was supplied. The README records this requirement and viewport creation returns null for unsupported presentation rather than publishing a usable-looking viewport.

Adding another family requires queue selection, ownership transfers, semaphores and retirement validation on that target. The existing surface-rejection, resize, semaphore-retirement and timeline/fence suites pass; they do not validate separate-family presentation. See the [shared verification record](RHIProductionVerification.md).
