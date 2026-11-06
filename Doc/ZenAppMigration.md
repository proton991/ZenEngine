# Standalone rendering uses ZenApp

`scene_renderer_demo` was removed on 2026-11-06. ZenEditor owns interactive scene
preparation, and **ZenApp** is the standalone application for rendering, captures,
and performance profiling. The display arrangement was left unchanged at the
user's request.

The removal includes the demo's source and CMake target in ZenTest, its private
capture/profiling/window-test helpers, the dedicated `SceneModelSwitchTest`, and
the unused `ZenRuntimeUI` settings/model browser with its integration suite.
ZenApp's `ZenSessionUI`, shared ImGui/RDG renderer, engine renderers, and editor
regression tests remain.

## Supported commands

ZenApp needs a `.zenrender` setup, which records the package, camera and render
configuration. ZenEditor does not save one yet: `MakeRuntimeFixture` writes a setup for
its small test scene, and another scene needs a copy of an existing setup edited to
point at its processed package and camera. Startup arguments override run settings
without editing `engine.cfg`.

`--gpu-culling=0` forces direct GBuffer drawing for comparison or a driver workaround;
`--gpu-culling=1` requests GPU frustum culling and indirect drawing (the default).
Profiles and capture JSON sidecars record both the request and the selected path,
including any fallback reason. The Nsight runner accepts the same option as
`--gpu-culling 0|1`; use the same executable/setup for both sides of a comparison.

```powershell
cmake --build --preset x64-windows-msvc-release --target ZenApp zen_editor --parallel 8
./build/x64-windows-msvc-release/bin/ZenApp.exe --setup=scene.zenrender --fullscreen
python tools/profile_hybrid_gi.py --setup=scene.zenrender --backend=compute --output=build/nsight-compute
python tools/profile_hybrid_gi.py --setup=scene.zenrender --backend=hardware --output=build/nsight-hardware
python tools/verify_rhi_production.py --build-dir=build/x64-windows-msvc-release --output=build/zenapp-smoke --smoke --setup=scene.zenrender
```

Nsight uses the actual ZenApp executable, explicit startup arguments, and retained
input hashes. Its default fullscreen 4K capture requires a native 4K display.
See [ZenEditor launch settings](../ZenEditor/README.md), [profiling](EngineProfiling.md),
and the [verified 4K captures](ComputeGI/ZenAppNewSponza4KNsightProfile.md).

`verify_rhi_production.py --smoke` now runs a supplied ZenApp setup in a 640×360
window across inline/threaded RHI and async off/on with validation. It copies the
setup into each result folder and records exact commands and logs. Executable
aliasing and demo-only switches are removed. `check_pr.py` builds and runs the
registered Debug/Release CTest suites.

## Retired automation and historical evidence

Demo-specific benchmark, image-validation, dynamic-capture and production-profile
runners were removed, including `run_voxel_gi_performance.cmd` and `check_full.py`.
Their special lighting/voxel/motion capture switches and historical CSV schema
are not implemented by ZenApp; renaming the executable in those commands would
not produce equivalent measurements. The old frozen-binary image qualification
stage was also removed from `check_pr.py`.

Offline analysis remains: archived-profile validation, linear capture readers and
comparisons, Mitsuba/Embree references, fixture generation, and voxel reference
calculations. The voxel oracle now lives in `tools/voxelization_reference.py`.
These tools analyze existing captures and do not launch the retired application.

Historical reports, screenshots, frozen captures and their recorded executable
identities remain evidence of the original runs. Their reproduction commands
refer to the old revisions, not the current build. Retired source is available in
[ZenEngine history](https://github.com/proton991/ZenEngine/tree/3ea1e7f9a2f74e40bb9fbfcdcbc2f1b2b219f324)
and [ZenTest history](https://github.com/proton991/ZenTest/tree/be7ec93435f0144f4c07143e15b22a401eefcabd).

## Verification

The Release builds of ZenApp, ZenEditor and the selected test targets passed.
Eight CTest suites passed: launch settings, native windows, Python tools, UI packet
and rendering contracts, and editor model/rendering tests. The additional
CommonTest, RenderCoreTest and VulkanRHITest runs passed 907 tests in total.
Retained offline analysis modules import successfully, and the generated Release
build contains none of the removed targets.

All four ZenApp smoke combinations requested 44 frames, rendered, and exited with code zero.
With RTSS running and `VK_LAYER_VALIDATE_SYNC=1`, each reported
`SYNC-HAZARD-PRESENT-AFTER-WRITE`, so the strict smoke check failed. The engine is not
the source: RTSS hooks `ZenApp.exe` by name and draws its overlay into the swapchain image. The
hazard names `vkCmdEndRenderPass`, which the engine never calls, and one run of the
same binary reported 10 hazards as `ZenApp.exe` and none under an RTSS-excluded file
name. That rename was only this diagnostic; profiling must still launch `ZenApp.exe`.

Exclude `ZenApp.exe` in RTSS or close RTSS before running the smoke check.
`verify_rhi_production.py` prints this when a step fails. `profile_hybrid_gi.py`
warns and records `rtss_running`, because a hooked capture may include overlay work.

While RTSS runs, the same hook makes `check_pr.py` fail. In Debug, `VulkanRHIIntegrationTest` crashed
and timed out in all 3 runs under its real name, including one under CTest, inside a
test that fakes presentation. The Vulkan error is an invalid queue family in a
`vkCreateCommandPool` call, which the engine's pools cannot produce. Both runs under an
RTSS-excluded file name passed all 492 tests. `check_pr.py` warns when RTSS is running;
close RTSS before relying on its result.
No synchronization suppression or renderer change was added for this removal.
After the review fixes, with RTSS still running, Release CTest passed all 21 suites
with `VulkanRHIIntegrationTest` left out, and that suite passed 493 tests with 5 skips
under an excluded file name. Debug CTest passed 21 of 22; that suite crashed under its
real name in both Debug and Release, and passed 492 tests in Debug under the excluded
name. ZenApp rejected `--backend=hardware --disable-rt` and reported the expected
backend, ray-query and async state for both backends in a 640×360 window.

With RTSS closed, both checks passed with every executable under its real name.
`check_pr.py` passed all 22 CTest suites in Debug and in Release. The
`verify_rhi_production.py --smoke` run passed CommonTest (50), RenderCoreTest (771),
VulkanRHITest (86) and VulkanRHIIntegrationTest (493, with 5 skips), then all four ZenApp
combinations with no hazards, VUIDs or `[error]` lines. A rerun with `--profile` confirmed
that each combination rendered 44 frames at 640×360 with no hazards or VUIDs. The moved
reflectance test passed all 4 variants in Debug and Release. This confirms RTSS as the
cause of both failures. The smoke runs use a small generated scene, and
`profile_hybrid_gi.py` has not yet completed a full Nsight capture with its new checks.

Evidence: [build log](../build/remove-scene-demo-build.log),
[CTest results](../build/remove-scene-demo-tests.log),
[native smoke commands and validation errors](../build/remove-scene-demo-smoke/results.json),
[clean `check_pr.py` run](../build/checks/pr-20261106-220311/summary.json),
[clean smoke results](../build/migration-review-20261106/smoke-clean/results.json), and
[smoke frame counts](../build/migration-review-20261106/smoke-frames).
