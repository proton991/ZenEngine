# RHI production readiness: R23 verification

Status: implemented and validated, 2026-10-03. Based on `8b59a3d0` plus this change. Part of [RHIProductionTODO.md](RHIProductionTODO.md#r23-releases-after-teardown-are-not-detected).

## Result

| Done-when condition | Implementation |
| --- | --- |
| Admission stays closed from the finalizer until a new backend or executor starts, in both modes. | `RHIThread::Stop` preserves closed admission after a backend finalizer. Inline finalizers establish the same current-thread scope as the worker so their own cleanup can finish. `Enqueue` rejects external work after closure; `IsCurrentThread` also checks inline admission so `Invoke` cannot bypass it. `Start` and the `DynamicRHI` constructor reopen admission. Stopping a worker without retiring a backend preserves standalone inline execution. |
| Late release reports ownership failure without touching the backend. | `RHIResource::ReleaseReference` and `IRHICommandContext::OnFinalRelease` report rejected cleanup through the shared `VerifyTeardownOwnership` helper in `RHIError.h`. Strict checks abort; non-strict checks log and leave the resource/context allocated without calling `Destroy` or the context destructor. |
| Backend globals are cleared on destruction. | `DynamicRHI` and `VulkanRHI` destructors clear `GDynamicRHI` and `GVulkanRHI` only when the global still points to that object. |
| A test releases a resource after executor destruction in both modes. | Parameterized strict and non-strict resource tests cover inline and threaded execution. Additional tests cover command contexts, synchronous calls, the finalizer boundary, cancellation and restart. |

The generic constructor/destructor definitions live in `DynamicRHI.cpp`, included in both `ZenCore` and `RenderCoreTest`. The fake-backend tests retain their own factory and do not link the Vulkan factory. The shutdown contract is documented in the [RHI README](../ZenCore/Include/Graphics/RHI/README.md#rhi-worker-shutdown).

## Teardown failures resolved

Enforcing the boundary exposed real ordering mistakes in the fake-backend fixtures. `TestRHI` retained attachment references through its recorded rendering layouts until its C++ destructor, which ran after the backend finalizer. It now releases those references in `Destroy`. Stack viewport storage also outlived backend teardown in several fixtures; they now release the viewport's owning reference before shutdown, while keeping its storage alive until retained submissions are drained. Tests that explicitly destroy an executor release their producer command lists first.

`RenderCoreEnvironmentTest.EnvironmentReplacementRetainsAndRetiresAllOutputs` now passes with strict teardown checks. No ownership checks were disabled to make the tests pass.

## Tests

The six `ExecutionModes/RHILateRelease*` cases each run in inline and threaded modes (12 tests total):

- `NonStrictReleaseAfterExecutorDestructionIsLoggedAndLeaked`: verifies no `Destroy` call, the diagnostic, and successful cleanup after a new raw backend starts.
- `NonStrictContextReleaseIsLoggedWithoutRunningItsDestructor`: verifies no context destruction and the diagnostic.
- `FinalizerClosesExternalAdmissionUntilRestart`: verifies finalizer-local cleanup, rejection from another thread during finalization, persistent closure after repeated `Stop`, exactly one cancellation, and reopening through `Start`.
- `StrictReleaseAfterExecutorDestructionStops`: verifies the resource death diagnostic.
- `StrictContextReleaseAfterExecutorDestructionStops`: verifies the context death diagnostic.
- `SynchronousInvocationAfterExecutorDestructionStops`: verifies synchronous calls cannot bypass closed admission.

`VulkanProductionFailureTest.BackendDestructionClearsGlobalPointers` checks both globals with a real Vulkan backend. Existing standalone Vulkan tests exercise backend lifetimes alongside executor-backed integration tests.

## Validation

Environment: Windows 10, AMD Radeon RX 7900 XT, MSVC 2022 Ninja presets. GPU tests use `7zFM.exe` aliases to exclude the RTSS overlay, `VK_LAYER_VALIDATE_SYNC=1`, and disabled implicit layers. Logs are under `build/rhi-production/r23/`.

The counts below come from the final tree, which also contains the error-handling work ([verification](RHIErrorHandlingVerification.md)), after clean rebuilds of both presets. When R23 was first finished, before that work, the counts were 554 RenderCore, 45 VulkanRHI and 334/335 integration tests, with the same results.

| Check | Debug | Release |
| --- | --- | --- |
| Full preset build, including all five acceptance targets | Pass | Pass |
| `CommonTest` | 112 passed, 1 skipped | 112 passed, 1 skipped |
| `RenderCoreTest` | 557 passed | 557 passed |
| `VulkanRHITest` | 48 passed | 48 passed |
| `VulkanRHIIntegrationTest` | 336 passed, 6 skipped | 337 passed, 6 skipped |
| Smoke: modes 1–3 × RHI thread off/on × async compute off/on, 44 frames including resize/minimize/shutdown | 12 of 12 passed | 12 of 12 passed |
| Validation and synchronization-validation errors | 0 | 0 |

Release includes the existing Release-only non-strict teardown test. The six Vulkan skips are four presentation-fence cases, the EXT surface-maintenance instance dependency, and the D24S8 fallback. The CommonTest skip concerns directory symlink creation. No new R23 test was skipped. Existing build warnings remain: the gli macro redefinition and Draco's CMake policy warning. The H1 ignored `FinalizeCommandLists` results are now checked.

Commands:

```text
cmake --build --preset x64-windows-msvc-debug --parallel 4
cmake --build --preset x64-windows-msvc-release --parallel 4
python tools/verify_rhi_production.py --build-dir build/x64-windows-msvc-debug --output build/rhi-production/r23/debug --smoke --executable-alias 7zFM.exe
python tools/verify_rhi_production.py --build-dir build/x64-windows-msvc-release --output build/rhi-production/r23/release --smoke --executable-alias 7zFM.exe
```

MSVC builds ran in the VS 2022 x64 developer environment. The final formatting-only spacing pass was followed by rebuilds of both presets; the semantic code tested above is unchanged. All modified C++ files pass `clang-format --dry-run --Werror --style=file --fallback-style=none`; `git diff --check` passes.

Release captures used `--mode=1/2/3 --frames=8 --fixed-step --vsync=0 --capture=...`. The saved pre-R23 Release binary (from the original R22 verification) and the rebuilt binary produce byte-identical PPM files. Baseline and current commands, logs, images and hashes are under `baseline/` and `current/` in the results directory:

| Mode | SHA-256, identical before/after |
| --- | --- |
| 1 | `2f9890cdfddebd64dfb1da77d9f032236d219f5d2b5a9135e900090608f4cb4c` |
| 2 | `08b5aa62513fe6385e82f667ead5a684cd4fd307cee724f58106585dad37a4f8` |
| 3 | `8d81caff45069cc4be5a700367112287472cff34da69ff5fc21d30b0f9dbf83d` |

No dedicated performance measurement was performed; this is a shutdown correctness change with no performance gate. The live inline ownership check adds an atomic admission read. No submission algorithm or rendering workload changed.

## Scope

This enforces the existing shutdown ownership contract. It does not recover resources from a retired backend or implement device recreation. Start/stop remain controlled lifecycle operations: producers must stop before teardown. Hardware coverage remains limited to the RX 7900 XT; R8 still applies.
