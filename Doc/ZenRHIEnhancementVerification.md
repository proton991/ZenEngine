# ZenRHI enhancement verification

Measurements on 2026-11-24 used the working checkouts based on ZenEngine `5309ec20` and ZenRHI `7b8ccd1`. These are MSVC x64 builds on Windows 11, build 26300. The application timings below use the release ZenApp. Each experiment retained its own executable fingerprint. The pre-review enhancement acceptance manifest records the test binaries, SwiftShader ICD, setup and checkout revisions. [Acceptance manifest](../build/enhancement/acceptance-inputs.json)

## Three-round review fixes

The November 24 Release run after the three-round review passed 150 Core and 83 Vulkan unit tests. The native suite passed 419 tests on NVIDIA (5 skipped), 410 on AMD (14 skipped) and 376 on SwiftShader (48 skipped). Each native suite observed the expected synchronization hazard once and recorded zero unexpected validation errors. The runner records the executable SHA-256 alongside each result. [NVIDIA](../build/three-round-fixes-20261124-142048/nvidia/results.json) · [AMD](../build/three-round-fixes-20261124-142048/amd/results.json) · [SwiftShader](../build/three-round-fixes-20261124-142048/software/results.json)

Engine acceptance passed all 97 Vulkan integration tests and all four ZenApp smoke configurations. Disabling synchronization validation through either layer override caused the runner to reject the job during the probe. A 4K ZenApp trace recorded the SHA-256 of the resolved relative environment, and a second trace recorded the HDR override selected by the renderer. [Engine results](../build/three-round-fixes-20261124-142048/engine-final/results.json) · [Canonical override rejected](../build/three-round-fixes-20261124-142048/disabled-engine/results.json) · [Legacy override rejected](../build/three-round-fixes-20261124-142048/legacy-disabled-engine/results.json) · [Relative environment](../build/three-round-fixes-20261124-142048/relative-env-trace/inputs.json) · [HDR override](../build/three-round-fixes-20261124-142048/override-env-trace/inputs.json)

The condition-variable watchdog implementation was compiled and exercised on Windows. Delaying a waiter beyond the deadline after signaling its event produced a false stall before the fix. Blocking the finalizer also produced no shutdown diagnostics. The same tests after the fix observed a quiet completed wait and both a stall and recovery for the blocked shutdown. The Windows debugger run skipped the watchdog-report test as intended. [Before](../build/three-round-fixes-20261124-142048/before-portable.log) · [After](../build/three-round-fixes-20261124-142048/after-portable.log) · [Debugger](../build/three-round-fixes-20261124-142048/debugger.log)

ZenApp rejected direct, relative and hard-linked cache aliases and preserved the launch inputs. A separate cache file survived two launches. The reflection fuzzer selected the vertex stage for the ordinary, matrix and array seeds, and the compute stage for the compute seed. The ASan fuzz run completed 177,420 inputs in 61 seconds. The relocated SDK, embedded package and public API policy checks also passed. [Cache regression and RenderCore tests](../build/three-round-fixes-20261124-142048/final-engine.log) · [Cache reuse](../build/three-round-fixes-20261124-142048/cache-positive-1.log) · [Seed results](../build/three-round-fixes-20261124-142048/seed-coverage.log) · [Fuzz run](../build/three-round-fixes-20261124-142048/fuzzer.log) · [Package and policy checks](../build/three-round-fixes-20261124-142048/final-package-policy.log)

## Original validation and checked startup

The table and counts below belong to the original enhancement run. The subsequent review-fix qualification recorded 150 Core, 83 Vulkan unit and 419 native NVIDIA Release passes, with five native skips. Each result applies to its own build. [Review-fix qualification](../build/enhancement/RHIReviewFixVerification.md) · [NVIDIA Release results](../build/enhancement/review-qualified-release-nvidia/results.json)

| Vulkan device | Driver reported by Vulkan | Debug native passes / skips | Release native passes / skips |
| --- | ---: | ---: | ---: |
| NVIDIA GeForce RTX 5080 | 2605236224 | 410 / 5 | 411 / 5 |
| AMD Radeon(TM) Graphics | 8388939 | 401 / 14 | 402 / 14 |
| SwiftShader LLVM 10 | 20971520 | — | 368 / 48 |

These runs also passed 137 Core and 80 Vulkan unit tests per configuration. The Khronos validation layer reported specification version 4211045 and implementation version 1, with synchronization validation enabled and the duplicate message limit disabled. The overlapping clears produced exactly one write-after-write hazard. The synchronized recording produced zero additional messages. The hardware runs used adapter selection by name. Intel was excluded at the user's request. Each runner report lists individual skip reasons. [NVIDIA Debug](../build/enhancement/shipping-debug-NVIDIA/results.json) · [NVIDIA Release](../build/enhancement/shipping-release-NVIDIA/results.json) · [AMD Debug](../build/enhancement/shipping-debug-AMD/results.json) · [AMD Release](../build/enhancement/shipping-release-AMD/results.json) · [Software](../build/enhancement/shipping-software/results.json)

The original SwiftShader revision advertised BC4/BC5 support but returned zero for the uploaded channels in a compute shader. Its draw path called descriptor preparation before sampling compressed images. Its dispatch path omitted the call. The CI build applies the same preparation to compute dispatch. The unmodified readback test passed on that driver and on both hardware vendors. [Stock driver result](../build/enhancement/verified-swift/results.json) · [CI driver patch](../ZenRHI/.github/actions/software-driver/compute-sampling.patch)

The relocated SDK executable survived missing validation manifests and a profiles layer hiding dynamic rendering, restored the environment, then created a device in the same process. The package gate built and executed seven programs/checks against the relocated installation, including four SDK examples that verified GPU readback. Triangle presented on the RTX 5080. [Package gate](../build/enhancement/shipping-package.log) · [Triangle](../build/enhancement/triangle-current.log)

MSVC AddressSanitizer passed 137 Core, 80 Vulkan unit and 368 native tests on SwiftShader. The native suite reported 48 capability skips. Hardening initially found malformed shader data reaching SPIRV-Reflect. Production reflection now validates module semantics with the SDK's SPIRV-Tools before reflection. A two-minute libFuzzer run completed 282,349 inputs after the change. [Sanitizer results](../build/enhancement/shipping-asan/results.json) · [Fuzzer run](../build/enhancement/verified-fuzzer.log)

The watchdog test held an RHI task until the waiter reported it, checked the first worker stack on Windows x64, released the task and observed recovery. A death test exercised an explicit fatal deadline. Windows message-service tests covered synchronous invocation, full-queue admission and shutdown. Device-loss tests combined a real submitted GPU breadcrumb with an injected device fault and preserved the accepted queue prefix. By agreement, a real driver loss with pass attribution remains an open release gate.

## Worker time at 3840 × 2160

The Nsight GPU Trace used New Sponza, direct draws, threaded RHI, hardware GI and async compute available, with validation off. ZenApp reported a fullscreen 3840 × 2160 framebuffer and render target, and a 1920 × 1080 GI target. The run requested GI at scene frame 20 after starting in PBR. Worker counters accumulate elapsed time in completed scopes. A frame delta can include work submitted by an earlier CPU frame. Caller-blocked time runs on a separate thread. The upload interval is contained within startup. [Arguments](../build/enhancement/entry-4k/application-command.json) · [Executable and input hashes](../build/enhancement/entry-4k/inputs.json) · [Nsight summary](../build/enhancement/entry-4k/summary.json) · [Worker samples](../build/enhancement/entry-4k/worker.rdg.log)

| Interval | Translation ms | Pipeline ms | Other creation ms | Submit ms | Wait ms | Idle ms | Caller blocked ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Initialization through first-view preparation | 7.89 | 318.00 | 4289.83 | 5385.77 | 269.83 | 5753.05 | 4627.61 |
| Scene and environment upload | 7.23 | 0 | 31.39 | 5385.53 | 269.73 | 5654.12 | 36.42 |
| First GI frame | 8.03 | 123.34 | 10.43 | 0.14 | 0.14 | 31.97 | 141.69 |
| Steady direct drawing, per-category median after frame 50 | 1.22 | 0 | 0 | 0.05 | 22.26 | 6.38 | 0 |

Initialization took 21.36 s wall time. Package reading took 5.05 s and the subsequent scene and environment upload interval took 11.35 s. The driver submission and work between RHI calls account for most of the upload interval. First GI cost opened the persistent pipeline cache experiment. GPU waits dominate steady drawing. This run gives no critical-path evidence for parallel translation.

## Pipeline cache experiment

Each cell used two fresh ZenApp processes with 58 fixed-step frames, the same setup and a GI request at frame 20. Each cold driver trial used a new empty directory selected by `__GL_SHADER_DISK_CACHE_PATH`. The driver created its own GLCache binary and table of contents there. Each warm trial received a separate copy of the priming run's populated directory. The 2,634,648-byte application seed came from a successful ZenApp snapshot. Each run recorded the seed hash, emitted cache hash, driver directory contents, framebuffer state, exact arguments and a frame-57 image. [Executable and input hashes](../build/enhancement/cache-matrix/inputs.json) · [Priming run](../build/enhancement/cache-matrix/initial/run.json)

| Driver | App seed | Startup ms | Startup pipeline ms | First GI pipeline ms | Worst CPU frame ms | CPU frame p95 ms |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| Cold | Empty | 17357 | 382.53 | 4471.57 | 4530.36 | 42.50 |
| Cold | Present | 16326 | 11.27 | 95.60 | 148.00 | 40.28 |
| Warm | Empty | 16199 | 11.56 | 91.45 | 140.67 | 39.06 |
| Warm | Present | 16530 | 1.31 | 37.58 | 87.19 | 39.59 |

Table values are the means of the two runs in each cell. First GI was the worst frame in each run. With a cold driver, empty-seed pipeline time ranged from 4465 to 4479 ms and seeded time ranged from 95 to 96 ms. With a warm driver those ranges were 91–92 ms and 37–38 ms. Retain opt-in persistence. Warm-driver startup varied between runs and its mean increased by 0.33 s with the seed. [Cold empty](../build/enhancement/cache-matrix/1-cold-empty/run.json) · [Cold seeded](../build/enhancement/cache-matrix/3-cold-initial/run.json) · [Warm empty](../build/enhancement/cache-matrix/4-warm-empty/run.json) · [Warm seeded](../build/enhancement/cache-matrix/2-warm-initial/run.json)

Images varied between repeated hardware-GI runs. Across the entire frame, seeded-versus-empty RMS differences were 0.151–0.170 on the 8-bit RGB scale. Empty-versus-empty differences were 0.163–0.173. A truncated seed and a seed with a different device ID and a valid checksum both selected empty-cache startup and rendered to completion. The warm seeded GI pipeline cost still exceeds a 60 Hz frame budget, opening the off-worker compilation experiment. [Image comparison](../build/enhancement/cache-matrix/image-rms.json) · [Truncation](../build/enhancement/cache-matrix/truncated/run.json) · [Device mismatch](../build/enhancement/cache-matrix/different-device/run.json)

A Windows file handle allowed reading the cache seed and denied replacement. ZenApp rendered three frames with required validation and device-loss diagnostics enabled, reported the failed write, and exited successfully. The original cache SHA-256 was unchanged and the temporary file was removed. [Atomic replacement check](../build/enhancement/locked-cache/run.json)

## Off-worker compute compilation decision

A release ZenApp prototype queued native compute pipeline creation on one compiler thread with retained shader inputs. RenderCore used the dispatch descriptions in the requested GI graph to find missing keys and submitted PBR while they compiled. Both paths used the same executable, scene, fixed timestep, fullscreen 4K configuration and GI request at frame 20. The warm trials used a copy of the primed driver cache and the same application seed, with the order reversed for the second pair. Each cold trial started with both caches empty. [Trial inputs](../build/enhancement/async-matrix/inputs.json) · [Prototype patch](../build/enhancement/async-matrix/compute-worker-experiment.patch)

| Configuration | First GI frame | Startup ms | CPU frame p95 ms | Worst CPU frame ms | Peak device-local bytes |
| --- | ---: | ---: | ---: | ---: | ---: |
| Warm synchronous, two trials | 20 | 15721–16303 | 39.34–39.66 | 95.26–98.12 | 12556990624 |
| Warm off-worker, two trials | 21 | 16437–16526 | 42.89–44.05 | 78.26–78.54 | 12825426080 |
| Cold synchronous, one trial | 20 | 16454 | 27.94 | 4531.97 | 12556990624 |
| Cold off-worker, one trial | 81 | 16705 | 28.54 | 3203.62 | 13093861536 |

The cold runs each submitted 221 frames, the warm pairs 58. Cold off-worker compilation displayed 61 fallback frames, then spent 3166 ms in synchronous pipeline creation on the first GI frame. The warm prototype increased p95 by 3–5 ms and peak allocation by 256 MiB. At equal GI history length, RMS differences were 0.164 on the 8-bit RGB scale, compared with 0.161 between synchronous repeats. All four threaded/inline and async-compute on/off validation runs submitted frames, reached GI, reported the active layer and logged zero errors. [Warm synchronous](../build/enhancement/async-matrix/sync1/run.json) · [Warm off-worker](../build/enhancement/async-matrix/async1/run.json) · [Cold synchronous](../build/enhancement/async-matrix/cold-sync/run.json) · [Cold off-worker](../build/enhancement/async-matrix/cold-async/run.json) · [Image comparison](../build/enhancement/async-matrix/image-rms.json)

The shipping path retains synchronous pipeline creation and opt-in cache persistence. The compute-only prototype missed the first-use frame budget in both cache conditions, increased p95 and memory, and required additional lifetime and fallback handling. The remaining cold hitch was outside the compute work moved by the prototype. Measuring graphics and ray-tracing pipeline costs separately would distinguish the remaining sources.

## Remaining gates

- **Resource creation and translation.** Moving every measured creation call off the worker during scene upload could remove at most 36.42 ms of caller blocking from an 11.35 s interval, about 0.32%. GPU waits dominated steady drawing. The measurements support retaining ordered creation and serial translation.
- **Capabilities and API baseline.** Retain Vulkan 1.2 with the existing profile. No new consumer or capture required a 1.3-only feature, per-resource barrier stages, or any of the optional capabilities listed in section 6. The archive records actual driver and layer versions.
- **Engine ray-query comparison.** The renderer integration suite reported 140 passes, four fixture skips and four failures. In the translated-instance case, pixel 4084 was `0xff6b2828` on the ray-query path and `0xff660505` on the ray-tracing pipeline path. The same four modes, pixel and values reproduced on the pre-enhancement engine. The strict image comparison remains active. [Full suite](../build/enhancement/gate-application/results.json) · [Final-build reproduction](../build/enhancement/confirmed-ray-parity.xml) · [Independent baseline](LibrarySplitVerification.md)
- **Engine acceptance.** CommonTest passed 20 tests, RenderCoreTest 660, VulkanRHITest 15 and VulkanRHIIntegrationTest 96. ZenApp completed 44 frames in each inline/threaded and async-compute on/off combination with required synchronization validation. [Suite commands, results and active layers](../build/enhancement/shipping-engine/results.json)
- **Release qualification.** Intel was excluded from this acceptance matrix. R7 still requires a dedicated machine to produce a real driver loss and verify the queue and pass in the resulting callback. ZenApp exposes `--device-loss-diagnostics` for breadcrumbs and device-fault data. The version remains 0.2.0.
