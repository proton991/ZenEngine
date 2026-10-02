# RHI production-readiness verification

Status: implementation and local acceptance completed for R1-R6, R9, R12, R15 and R16, 2026-10-02. R7 diagnostics and R8 tooling are implemented, with manual/hardware acceptance still open. R10, R11, R13 and R14 retain their plan gates. This is not a claim that all desktop production certification is complete.

## Scope and environment

Continued section 8 of [RHIImprovementPlan.md](RHIImprovementPlan.md), preserving the existing uncommitted R5 and deferred-work changes at base `e79660ecdef230a90d72ec4c4606ece5da04b15d`. No files were staged or committed. Windows, AMD Radeon RX 7900 XT, driver raw `8388981`, MSVC 19.44, local Vulkan SDK 1.4.304.0 and clang-format 19.1.5. The user chose an internal RHI and confirmed that no NVIDIA/Intel test machines are available.

Each item has its own implementation/evidence record: [R1](RHIProductionR1Verification.md), [R2](RHIProductionR2Verification.md), [R3](RHIProductionR3Verification.md), [R4](RHIProductionR4Verification.md), [R5](RHIProductionR5Verification.md), [R6](RHIProductionR6Verification.md), [R7](RHIProductionR7Verification.md), [R8](RHIProductionR8Verification.md), [R9](RHIProductionR9Verification.md), [R10](RHIProductionR10Verification.md), [R11](RHIProductionR11Verification.md), [R12](RHIProductionR12Verification.md), [R13](RHIProductionR13Verification.md), [R14](RHIProductionR14Verification.md), [R15](RHIProductionR15Verification.md), [R16](RHIProductionR16Verification.md).

## Validation

Debug and Release default builds passed. Full suites passed in both configurations; the final task-cancellation regression brings RenderCoreTest to 537 passes. GPU programs ran sequentially. Correctness used Khronos validation, `VK_LAYER_VALIDATE_SYNC=1`, `VK_LOADER_LAYERS_DISABLE=~implicit~` and RTSS exclusion. Expected fault-injection error messages are permitted; VUID and synchronization hazards fail the runner. Native fixtures reported no memory leaks.

| Suite | Passed per configuration | Skipped |
| --- | ---: | ---: |
| CommonTest | 105 | 1 |
| RenderCoreTest | 537 | 0 |
| VulkanRHITest | 45 | 0 |
| VulkanRHIIntegrationTest | 329 | 6 |
| SmartPtrTest | 34 | 0 |
| FlatHashMapTest | 6 | 0 |
| LRUCacheTest | 12 | 0 |
| InputControllerTest | 7 | 0 |
| ConfigLoaderTest | 21 | 0 |
| UIDrawPacketTest | 8 | 0 |
| RuntimeUIIntegrationTest | 10 | 0 |
| SceneModelSwitchTest | 8 | 0 |
| ConeVoxelGIIntegrationTest | 4 | 0 |
| ThreadPoolTest | Passed (standalone executable) | 0 |

The CommonTest skip is unavailable directory-symlink creation. Native skips remain four swapchain-maintenance fence paths, the EXT surface-maintenance dependency and D24S8. Skips are not passes.

All 24 default smoke combinations passed: Debug/Release × modes 1/2/3 × RHI thread 0/1 × async compute 0/1. All 12 additional Release smoke combinations with `--device-loss-diagnostics` passed. No validation or synchronization-validation errors occurred. Native suites exercise both timeline and fence paths. AMD and forced-portable breadcrumb tests run real GPU markers, then inject the device-loss report; no actual GPU loss was caused.

All three final captures are byte-identical to the R5 baseline:

| Mode | SHA-256 |
| --- | --- |
| 1 | `c5533a5797fa9437766cccdd25e5a9cf86dfb71b66347ed44634e5ee22d891fa` |
| 2 | `3f1d60b1b0f553a4aa14f7058fe1c1f946f4ed5c9f71703a7421521d0d7b46f2` |
| 3 | `1b468e3890ec3a569ce9b64e94d963ff7194f5ad6e5f2aa41ebee082008d6a3b` |

The original `Data/engine.cfg` is unchanged, SHA-256 `c09a51bce3cc624cd1b66d3d90fb26072089bf22aa8c6c4ad0ac5aa483bf7871`. Whole-file clang-format checks cover 65 changed C++ files; Python compilation, workflow YAML parsing and `git diff --check` pass. Compiler warnings remain the GLI macro redefinition and two nodiscard warnings in swapchain exception tests.

## Frame cost

64 Release profiles compare the saved R5 executable against the final executable, and final diagnostics off against on. Each comparison uses A-B-B-A ordering, Sponza at 1280 × 720 and voxel resolution 256, 60 warm-up frames followed by 600 measured frames, modes 2/3, both RHI-thread modes and both async-compute settings. VSync and validation are disabled; fixed-step is enabled. No build or other GPU test ran concurrently with profiling. These 64 profiles precede the final command-context pool ownership counter and teardown assertion; that final change affects context creation/release and shutdown, and is covered by subsequent native/renderer checks. The measured executables are hashed in `performance-binaries.json`.

Each table value is the median of the two runs' respective median/p95 statistics, shown before → after. These are whole-change results, not isolated per-item speedups. Default median CPU frame changes range from -0.72% to +0.73%; GPU medians change from -0.50% to +0.02%. The larger variation in RHI CPU time is within the previously recorded CPU noise and does not increase frame time. Submission and descriptor counter medians/p95 remain unchanged. This supports neutral default cost on this AMD system.

### R5 baseline → final, diagnostics off

| Mode | RHI thread | Async compute | CPU median / p95 ms | RHI median / p95 ms | GPU median / p95 ms |
| --- | ---: | ---: | --- | --- | --- |
| 2 | 0 | 0 | 1.1857 / 1.4548 → 1.1845 / 1.4452 | 0.5958 / 0.8640 → 0.5900 / 0.8470 | 1.1280 / 1.1993 → 1.1278 / 1.1989 |
| 2 | 0 | 1 | 1.1867 / 1.4688 → 1.1843 / 1.4721 | 0.5040 / 0.7790 → 0.4962 / 0.7610 | 1.1291 / 1.2015 → 1.1278 / 1.1989 |
| 2 | 1 | 0 | 1.2004 / 1.5106 → 1.1918 / 1.4917 | 0.4850 / 0.7950 → 0.5183 / 0.7965 | 1.1343 / 1.2350 → 1.1345 / 1.2152 |
| 2 | 1 | 1 | 1.2352 / 1.4830 → 1.2442 / 1.4573 | 0.4430 / 0.6790 → 0.4475 / 0.6475 | 1.1330 / 1.2050 → 1.1273 / 1.1897 |
| 3 | 0 | 0 | 1.5111 / 1.8338 → 1.5074 / 1.8085 | 0.8918 / 1.2025 → 0.8735 / 1.1775 | 1.4509 / 1.5484 → 1.4487 / 1.5407 |
| 3 | 0 | 1 | 1.5040 / 1.8185 → 1.5033 / 1.8242 | 0.7840 / 1.0960 → 0.7780 / 1.1040 | 1.4476 / 1.5392 → 1.4471 / 1.5490 |
| 3 | 1 | 0 | 1.5024 / 1.8034 → 1.4970 / 1.8235 | 0.7585 / 1.0585 → 0.7548 / 1.0705 | 1.4415 / 1.5502 → 1.4414 / 1.5564 |
| 3 | 1 | 1 | 1.5137 / 1.8898 → 1.5128 / 1.8872 | 0.6815 / 1.0725 → 0.6800 / 1.0495 | 1.4477 / 1.5725 → 1.4463 / 1.5729 |

### Final diagnostics off → on

| Mode | RHI thread | Async compute | CPU median / p95 ms | RHI median / p95 ms | GPU median / p95 ms |
| --- | ---: | ---: | --- | --- | --- |
| 2 | 0 | 0 | 1.1889 / 1.4802 → 1.1889 / 1.4975 | 0.5955 / 0.8905 → 0.5910 / 0.8860 | 1.1282 / 1.2158 → 1.1324 / 1.1952 |
| 2 | 0 | 1 | 1.1852 / 1.4714 → 1.1932 / 1.4992 | 0.5208 / 0.7960 → 0.5175 / 0.8085 | 1.1299 / 1.1996 → 1.1357 / 1.2107 |
| 2 | 1 | 0 | 1.1953 / 1.5064 → 1.2015 / 1.5090 | 0.4890 / 0.7800 → 0.4597 / 0.7570 | 1.1345 / 1.2370 → 1.1352 / 1.2263 |
| 2 | 1 | 1 | 1.2489 / 1.5032 → 1.2515 / 1.5017 | 0.4475 / 0.7030 → 0.4520 / 0.6820 | 1.1285 / 1.1976 → 1.1395 / 1.2170 |
| 3 | 0 | 0 | 1.5168 / 1.8503 → 1.5187 / 1.8393 | 0.8970 / 1.2095 → 0.8890 / 1.2075 | 1.4521 / 1.5552 → 1.4564 / 1.5411 |
| 3 | 0 | 1 | 1.5056 / 1.8074 → 1.5122 / 1.8582 | 0.7738 / 1.0700 → 0.7948 / 1.1425 | 1.4495 / 1.5450 → 1.4537 / 1.5495 |
| 3 | 1 | 0 | 1.5059 / 1.8466 → 1.5071 / 1.8262 | 0.7588 / 1.0825 → 0.7612 / 1.0680 | 1.4449 / 1.5595 → 1.4513 / 1.5465 |
| 3 | 1 | 1 | 1.5057 / 1.8097 → 1.5165 / 1.8622 | 0.6678 / 0.9655 → 0.6815 / 1.0230 | 1.4505 / 1.5680 → 1.4521 / 1.5624 |

Breadcrumb-enabled median CPU frame cost ranges from -0.01% to +0.72%; GPU medians increase by 0.06%-0.97% (at most about 0.011 ms). This is approximately neutral at this measurement resolution, not evidence of zero overhead. The portable path's frame cost and other vendors remain unmeasured. Diagnostics stay opt-in. A real device-loss reproduction is still required to close R7; successful marker readback does not prove that a driver preserves the same visibility after loss.


### Final ownership check follow-up

After adding the command-context pool ownership counter/assertion, both native suites passed 329 tests (6 capability skips), all 12 Debug default and 12 Release diagnostic smoke combinations passed, and the three captures remained identical. Another 16 A-B-B-A profiles compare the preserved measured executable with the final executable, using the same 60/600-frame method and async compute enabled:

| Mode | RHI thread | CPU median ms | RHI median ms | GPU median ms |
| --- | ---: | --- | --- | --- |
| 2 | 0 | 1.1924 ? 1.1933 | 0.5240 ? 0.5268 | 1.1273 ? 1.1286 |
| 2 | 1 | 1.2490 ? 1.2231 | 0.4603 ? 0.4597 | 1.1266 ? 1.1335 |
| 3 | 0 | 1.5105 ? 1.5039 | 0.8253 ? 0.8240 | 1.4475 ? 1.4460 |
| 3 | 1 | 1.5095 ? 1.5091 | 0.7020 ? 0.6823 | 1.4471 ? 1.4419 |

This final ownership check also has neutral frame cost: CPU medians change by -2.08% to +0.08%, GPU medians by -0.36% to +0.61%. The raw results and p95 values are in `ownership-performance.json`; `guard-check.py` records the exact commands. These bring the total to 80 paired profiles.

## Reproduction and artifacts

The checked-in runner is [tools/verify_rhi_production.py](../tools/verify_rhi_production.py). Use the repository's MSVC developer environment and build each preset first:

```powershell
cmake --build --preset x64-windows-msvc-debug
cmake --build --preset x64-windows-msvc-release
python tools/verify_rhi_production.py --build-dir build/x64-windows-msvc-debug --output build/rhi-validation/debug --smoke
python tools/verify_rhi_production.py --build-dir build/x64-windows-msvc-release --output build/rhi-validation/release --smoke
```

`--diagnostics` adds diagnostics to smoke runs. `--unit-only` selects the hardware-independent suites for software CI. `--executable-alias 7zFM.exe` was used locally to exclude RTSS hooking. The runner records actual commands, pass/skip counts and validation errors in JSON with per-run logs; it does not change the engine configuration.

Local exact commands, binaries and logs are in [build/rhi-production/](../build/rhi-production/): `queue-drain-build-{debug,release}.log`, `ticket-build-{debug,release}.log`, `ownership-build-{debug,release}.log`, `ownership/`, `final-debug/`, `final-release/`, `final-ticket/`, and `acceptance/`. The last directory contains the 14-suite runs, smoke runs, final capture hashes, 64 raw profile CSV/JSON files, `performance-summary.json` and `paired-summary.json`. `acceptance.py` and `final-captures.py` reproduce the local paired checks. The executable manifest is `final-manifest.json`.

## Remaining acceptance and gates

- **R7:** run an intentional, controlled device-loss reproduction on a dedicated test setup with diagnostics enabled and preserve stderr/driver report. This session did not hang/reset the user's GPU. The last observed queue/pass is diagnostic evidence, not proof of the faulting command.
- **R8:** execute both configurations and the smoke matrix on NVIDIA and Intel, then record the skipped capability paths. The [Windows SwiftShader CI workflow](../.github/workflows/rhi-software.yml) is authored and its YAML parses, but no GitHub run is claimed. The pinned SDK URL is reachable by GET and the SwiftShader revision/build option was checked; installation/build of that driver has not run here. macOS/MoltenVK, Linux and mobile remain unsupported.
- **R10:** collect reproducible cold-driver-cache pipeline-creation spikes across Sponza, glTF assets and GI reconfiguration before choosing warmup or asynchronous compilation. Existing warm profiles cannot close that gate.
- **R11:** provide/profile the heavier scene required by its gate before changing recording ownership/concurrency.
- **R13:** implement features with their consuming renderer changes; ray-query lighting remains in its separate plan.
- **R14:** a supported platform needing another presentation queue must drive implementation and validation. Current supported presentation requires the graphics family.
