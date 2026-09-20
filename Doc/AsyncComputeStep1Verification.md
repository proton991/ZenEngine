# Async compute — Step 1 verification

Date: 2026-09-20. Scope: **Step 1 — Capability selection and startup controls** in [the implementation plan](AsyncComputeImplementationPlan.md). Stop here for user verification; Step 2 has not started.

## Implemented behavior

- Vulkan selects explicit queue family/index pairs. It prefers a separate compute-capable family, uses index `1` in the graphics family when needed and available, and aliases graphics when only one queue is available. Device creation requests enough queues for the selected indices. Transfer uses a dedicated transfer family when available, otherwise the selected compute queue.
- RHI exposes compute support, GPU submission-dependency support, and native queue equivalence IDs. IDs derive from actual queue handles; serials retain their logical queue/timeline identity. The executor caches the snapshot in both inline and threaded modes.
- `async_compute=off|auto` loads into `RenderConfig` before RenderDevice initialization. Missing/invalid values fall back to off. `--async-compute=0|1` overrides the config independently of `--rhi-thread`; invalid CLI values fail parsing.
- RenderDevice resolves startup policy and logs capability, queue sharing, and fallback reason. **Compute passes remain on graphics** at this stage. No first-use message is emitted because the multi-queue scheduler has not been implemented.

The local `Data/engine.cfg` was preserved. The loader's generated default configuration now includes `async_compute=off`.

## Acceptance evidence

Windows x64, MSVC Debug, Vulkan SDK 1.4.357.0, NVIDIA GeForce RTX 5080. The ordinary device selected graphics `0:0`, compute `2:0`, and transfer `1:0`.

| Check | Evidence | Result |
| --- | --- | --- |
| Config, CLI override, invalid values, fallback reasons | `ConfigLoaderTest` | 7 passed |
| Queue-selection topologies and existing Vulkan unit coverage | `VulkanRHITest` | 35 passed, including 7 new selection tests |
| Cached capabilities, startup policy in both CPU modes, existing RenderCore coverage | `RenderCoreTest` | 252 passed, including 2 new capability tests; 7 existing disabled benchmark cases were not run |
| Native queue creation, existing Vulkan integration coverage, synchronization validation | `VulkanRHIIntegrationTest` | 256 passed; zero failures/skips, synchronization errors, or reported allocator leaks |
| Native capability suite in isolation | `VulkanCapabilityIntegrationTest.*` | 14 passed; no allocator leaks |
| Renderer startup, mode switches, resize, shutdown | 32-frame smoke run for each combination of `--rhi-thread=0|1` and `--async-compute=0|1` | All 4 exited 0; no validation hazards or allocator leaks |
| Invalid demo argument | `--async-compute=2 --frames=1` | Exited 1 with usage text |

The new native tests mask advertised topology only within each test, forward device creation to the real driver, and inspect the resulting queue handles and requested queue counts. They exercised a single queue, two distinct queue indices within the graphics family, and compute/transfer aliasing without skips. Capability reporting was also checked against the normal separate-family topology and with timeline support masked off. Synthetic selection tests cover absent/empty families and a separate compute-capable family that also supports graphics.

The first full integration run passed all 256 tests but reported 32 bytes retained at shutdown. The earlier baseline log (`build/rhi-review-20260920-integration.log`) has the same report. An isolated upload-suite run traced it to the fixture's static `waitValues` capture buffer. Its teardown now releases that buffer before the allocator report; this is test cleanup, with no engine lifetime behavior change. The final full integration run passed all 256 tests with no reported leaks. Together with the other three suites, 550 tests passed; the focused 14-test run is a subset, not an additional 14 distinct tests.

Native runs used the existing [overlay-isolation procedure](RenderCoreRHIThreadingVerification.md): process-local implicit-layer exclusions, Khronos synchronization validation enabled, and an identical temporary `bin/7zFM.exe` copy for integration tests. The temporary copy was removed after each run. No persistent overlay or validation settings were changed.

## Reproduce and review

From an x64 MSVC developer shell at the repository root:

```powershell
cmake --build build/x64-windows-msvc-debug --target ConfigLoaderTest VulkanRHITest RenderCoreTest VulkanRHIIntegrationTest scene_renderer_demo -j 8
.\bin\ConfigLoaderTest.exe --gtest_color=no
.\bin\VulkanRHITest.exe --gtest_color=no
.\bin\RenderCoreTest.exe --gtest_color=no
$env:VK_LAYER_VALIDATE_SYNC = '1'
$env:VK_LOADER_LAYERS_DISABLE = '~implicit~'
$env:DISABLE_RTSS_LAYER = '1'
.\bin\VulkanRHIIntegrationTest.exe --gtest_color=no
```

On this machine, use the linked isolation procedure for native integration tests to avoid RTSS injection. The verification run used the existing local helper:

```powershell
python build/rhi-phase4/run_validation.py ../async-compute-step1-integration-final.log --gtest_output=xml:build/async-compute-step1-integration-final.xml
```

From `E:\Dev\ZenEngine\bin`, inspect the startup log with:

```powershell
.\scene_renderer_demo.exe --rhi-thread=1 --async-compute=1 --frames=32 --smoke-test
.\scene_renderer_demo.exe --rhi-thread=0 --async-compute=0 --frames=32 --smoke-test
```

On the tested GPU, the first command reports `requested=auto`, `supported=yes`, `policy=available`, and `scheduling=disabled (implementation pending)`. The second reports `requested=off` and `policy=disabled by configuration`. These runs validate startup controls and existing rendering behavior; they do not demonstrate async compute overlap or a performance improvement.

Local logs and XML reports use `build/async-compute-step1-*`: `build.log`, `config.*`, `rhi.*`, `rendercore.*`, `native-focused.*`, `integration-final.*`, `scene-{0,1}-{0,1}.log`, and `invalid-cli.log`. `clang-format --dry-run --Werror`, `git diff --check`, and local document-link/code-fence checks passed. Validation was performed on Windows/MSVC only; other platforms were not run.
