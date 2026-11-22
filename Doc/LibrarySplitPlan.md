# ZenFoundation, ZenRHI and ZenAssetProcessor library split plan

Local implementation is based on `hybrid-gi` at `887fa9da`. Publication follows local verification and repository creation by the owner. See [the local verification record](LibrarySplitVerification.md). This plan replaces the rule in [ZenAssetProcessor plan §3](ZenAssetProcessorImplementationPlan.md#3-repository-and-dependency-boundary) that kept the engine memory subsystem out of the processor.

## Goal

- **ZenFoundation:** a new standalone library and repository for the engine's containers, utilities, memory and verification code. ZenEngine, ZenRHI and ZenAssetProcessor all build on it.
- **ZenRHI:** a new standalone library and repository for the RHI interface, command lists, executor, RHI thread, GPU timing and the Vulkan backend. Other projects can use it without ZenEngine, the way nvrhi is used without Donut.
- **ZenAssetProcessor** uses ZenFoundation internally.
- Each library takes its own tests when its code moves. ZenEngine owns tests that verify behavior across component or repository boundaries. Review all first-party suites, including the single-component tests that stay in ZenTest.
- ZenEngine's behaviour and performance stay unchanged after every phase.

Not in scope: new backends, more than one device per process, a stable DLL ABI, and renaming the `zen` namespace or the `RHI*` type prefixes.

## Current state

Measured on 2026-11-21 at `887fa9da` (`hybrid-gi`).

The RHI is about 30K lines compiled into ZenCore: `Graphics/RHI` (18 headers, 8 sources) and `Graphics/VulkanRHI` (about 20K lines). Its boundary with the code above it is already clean. No RHI file includes RenderCore, SceneGraph or AssetLib, RenderCore and the applications never use Vulkan types, and only [RHIFactory.cpp](https://github.com/proton991/ZenEngine/blob/887fa9da0cc55d5a797345dcd2bf30fe96741a3d/ZenCore/Source/Graphics/RHI/RHIFactory.cpp) includes the backend.

The RHI depends on these parts of the engine:

| Dependency | Where | Resolution |
| --- | --- | --- |
| Templates, Utils, Memory | Throughout. 13 of the 18 public headers use `VectorView` (67), `BitField` (51), `HeapVector` (44), `NameID` (29), `RefCountPtr` (22) and `SmallVector` (16). They are also private members of public classes such as `RHIResource::m_resourceTag` and the arena of `RHICommandList` | ZenFoundation (Phase 1), boundary rules (Phase 4) |
| SDL3/GLFW surface creation on `platform::NativeWindow` | [VulkanWindowBridge.cpp](https://github.com/proton991/ZenEngine/blob/887fa9da0cc55d5a797345dcd2bf30fe96741a3d/ZenCore/Source/Graphics/VulkanRHI/Platform/VulkanWindowBridge.cpp); [`CreateViewport(void*)`](https://github.com/proton991/ZenEngine/blob/887fa9da0cc55d5a797345dcd2bf30fe96741a3d/ZenCore/Include/Graphics/RHI/DynamicRHI.h#L112) | `RHIWindowHandle` (Phase 3) |
| SPIR-V loading by file name under `SPV_SHADER_PATH` | [RHIResource.h:674](https://github.com/proton991/ZenEngine/blob/887fa9da0cc55d5a797345dcd2bf30fe96741a3d/ZenCore/Include/Graphics/RHI/RHIResource.h#L674), [VulkanPipeline.cpp:385](https://github.com/proton991/ZenEngine/blob/887fa9da0cc55d5a797345dcd2bf30fe96741a3d/ZenCore/Source/Graphics/VulkanRHI/VulkanPipeline.cpp#L385) | The RHI takes bytes; RenderCore loads files (Phase 3) |
| `platform::Timer` | Two calls in `VulkanCommandList.cpp` | `std::chrono` |
| glm through `Math/Math.h` | 3 `Rect2`, 9 `Vec3i` | RHI-owned types |
| `Graphics/Common/Format.h`, `Color.h` | Included only by `RHICommon.h` | Move into ZenRHI |
| `Graphics/Shared/Bindless.h` | `RHICommon.h`, `VulkanDescriptorPool.cpp`, `Data/Shaders/Common/bindless_heap.glsl` | Move into ZenRHI as a C++/GLSL header |
| `ZEN_WIN32`, `ZEN_MACOS` | Set on ZenCore by the root `CMakeLists.txt` | Defined by ZenFoundation |
| `ZEN_DEBUG` | Set PUBLIC on ZenCore in every configuration ([ZenCore/CMakeLists.txt:349](https://github.com/proton991/ZenEngine/blob/887fa9da0cc55d5a797345dcd2bf30fe96741a3d/ZenCore/CMakeLists.txt#L349)). It changes the layout of `VulkanSemaphoreManager` and turns on per-site allocation tracking | Defined unchanged on the ZenFoundation target |
| spdlog through `Utils/Errors.h` | About 110 log calls | Through ZenFoundation |

Other facts the plan depends on:

- **Globals:** `GDynamicRHI`, `GRHIFrameState`, `GVulkanRHI` (269 uses), `GVkMemAllocator` and the `RHIOptions` singleton.
- **Lifetime:** the [RHI README](https://github.com/proton991/ZenEngine/blob/887fa9da0cc55d5a797345dcd2bf30fe96741a3d/ZenCore/Include/Graphics/RHI/README.md) calls the RHI an internal layer of RenderCore. GPU-safe retirement lives in RenderCore's `ResourceRetirement` (decision R9, 2026-10-02), and that type depends only on executor types.
- **Tests:** the [ZenTest target list](../ZenTest/CMakeLists.txt) mixes owners within executables and source files. RenderCoreTest compiles RHI sources against a fake backend through `ZenRenderCoreTestSources`. The [SwiftShader job](../.github/workflows/rhi-software.yml) runs `CommonTest`, `RenderCoreTest` and `VulkanRHITest` through the runner's `--unit-only` path. Hardware integration runs separately.
- **Include rewrite size:**

  | | ZenCore | ZenEditor | ZenTest | ZenUI | ZenApp | ZenAssetProcessor tests |
  | --- | --- | --- | --- | --- | --- | --- |
  | Files with foundation includes | 114 | 15 | 22 | 2 | 2 | 6 |
  | Files with RHI includes | 77 (including the RHI itself) | 1 | 47 | 1 | 0 | 4 |

- **ZenAssetProcessor** has 104 source files and about 27K lines:
  - Containers: it uses `std` containers, with `std::vector` 74 times in `include/` and 383 times in `src/`.
  - Public boundary: `std::span`.
  - Dependencies: it reuses existing targets with `if(NOT TARGET …)` and exports an installed package (`ZenAssetConfig.cmake`).
  - Diagnostics: it has no logging library and 16 `assert` calls.
- **Allocation tracking cost:** with `ZEN_DEBUG`, every `ZEN_MEM_ALLOC` and free takes one global spin lock for the per-site statistics ([Memory.cpp:81](https://github.com/proton991/ZenEngine/blob/887fa9da0cc55d5a797345dcd2bf30fe96741a3d/ZenCore/Source/Memory/Memory.cpp#L81)).

## Target

### Repositories

```text
ZenEngine/                     main repository
  ZenFoundation/               submodule, proton991/ZenFoundation (new)
  ZenRHI/                      submodule, proton991/ZenRHI (new)
  ZenAssetProcessor/           submodule (existing)
  ZenTest/  ZenSample/         submodules (existing)
  tests/integration/           cross-boundary tests tracked directly by ZenEngine
  ZenCore/  ZenUI/  ZenApp/  ZenEditor/  Data/  tools/  External/
```

ZenAssetProcessor and ZenRHI depend on ZenFoundation. ZenEngine consumes all three. Once published, their development checkouts are `E:\Dev\ZenFoundation`, `E:\Dev\ZenRHI` and `E:\Dev\ZenAssetProcessor`. Publish dependency and companion ZenTest or ZenSample changes before recording their revisions in ZenEngine. Keep companion changes compatible with the previous engine main until the engine switches.

### ZenFoundation

```text
include/ZenFoundation/Templates/   HeapVector, SmallVector, VectorView, FlatHashMap, NameID, ...
include/ZenFoundation/Utils/       Errors, Mutex, RefCountPtr, SharedPtr, UniquePtr, ...
include/ZenFoundation/Memory/      Memory, PagedAllocator, PoolAllocator, LinearAllocator
src/                               Memory.cpp
tests/                             containers, ownership, allocators, verification and threading
```

- **Target:** a static library, `ZenFoundation::Foundation`. Its only third-party dependency is spdlog, which also supplies fmt. An existing `spdlog` target is reused.
- **Macros:** its headers define only prefixed macros: `ZEN_ASSERT`, `ZEN_VERIFY`, `ZEN_VERIFY_MSG`, `ZEN_VERIFY_MSG_F`, `ZEN_LOG_ERROR` and so on. ZenCore keeps `Utils/Errors.h` as an engine-only compatibility header that defines `ASSERT`, `VERIFY_EXPR`, `LOGE` and the rest on top of them, so engine code does not change.
- **Configuration:** the settings that affect layout are PUBLIC definitions on the target, so every consumer compiles the same layouts:
  - `ZEN_WIN32` and `ZEN_MACOS`, derived from the platform.
  - `ZEN_DEBUG`, defaulting to today's always-on behaviour.
  - `ZEN_FOUNDATION_ALLOCATION_TRACKING`, defaulting to on, as today.
- **Platform code stays in ZenCore:** `Platform/`, `Math/` and the window, input and file-system code.
- **Tests:** `ZENFOUNDATION_BUILD_TESTS` defaults to `PROJECT_IS_TOP_LEVEL`. Register the standalone suite with CTest when enabled. Embedded consumers can use the library without building GoogleTest.

### ZenRHI

```text
include/ZenRHI/          DynamicRHI.h, RHICommandList.h, RHIResource.h, RHIFormat.h, RHIColor.h, ...
include/ZenRHI/Shader/   Bindless.h (shared by C++ and GLSL)
include/ZenRHI/Vulkan/   VulkanRHIFactory.h, VulkanInterop.h, VulkanSurface.h
src/Core/                command lists, executor, RHI thread, GPU timing, deferred release
src/Vulkan/              backend sources and private headers
src/Vulkan/Platform/     Win32 and Metal surfaces from native handles
tests/  examples/Triangle/  cmake/ZenRHIConfig.cmake.in  .github/workflows/
```

| Target | Contents | Dependencies |
| --- | --- | --- |
| `ZenRHI::Core` | Interface, command lists, executor, RHI thread, GPU timing | ZenFoundation (PUBLIC); no Vulkan |
| `ZenRHI::Vulkan` | Vulkan backend | Core; Vulkan headers, volk, VMA, spirv-reflect (PRIVATE; existing targets reused) |
| `ZenRHI::ZenRHI` | Umbrella target and `CreateRHI` | Core, Vulkan |

`ZENRHI_BUILD_TESTS` and `ZENRHI_BUILD_EXAMPLES` default to `PROJECT_IS_TOP_LEVEL`. Only the tests and examples need glslangValidator.

### Public boundary rules

1. **No ZenFoundation type appears in a public signature.** Parameters, return values and public descriptor fields use `std` types or types that ZenRHI owns. Users never write code against ZenFoundation.
2. **Private members of public classes may keep foundation types.** Examples are `RHIResource::m_resourceTag`, the arena of `RHICommandList` and the executor queues. Hiding them would need pimpl through `RHIResource` and the recording path. So public headers include ZenFoundation headers, and ZenRHI links ZenFoundation PUBLIC. CMake then adds its include path and library for users automatically, and users fetch nothing themselves.
3. **Public headers expand no unprefixed macros.** Inline code that verifies or logs moves to source files or uses the `ZEN_` macros.
4. **Vulkan internals are private** (`src/Vulkan/`). Public Vulkan headers cover only creation, access to native handles, and surface creation.
5. **ZenAssetProcessor applies rules 1 and 3.** Its public headers already use `std` types, so it links ZenFoundation PRIVATE.

Rule 1 replaces these types in public signatures:

| Today | Public boundary |
| --- | --- |
| `VectorView<T>` | `std::span<T>` (no runtime cost; `HeapVector` converts implicitly) |
| `HeapVector&` out-parameters | A caller-provided `std::span` with a returned count, or `std::vector` where ownership passes |
| `SmallVector` fields in descriptors | Fixed arrays with a count, like nvrhi's `static_vector` |
| `BitField<Flags>` | Flag enums with operators, in a small ZenRHI header |
| `NameID` | `std::string_view` arguments, `std::string` stored names |
| `HashMap` of specialization constants | An array of `{id, value}` |
| `RefCountPtr` and `SharedPtr` | The existing `RHIResourcePtr<T>`, or an RHI-owned handle |

`std::vector` stays out of recording paths, which take spans and fixed arrays.

### Use inside ZenEngine

```cmake
include(External/ZenFoundation.cmake)      # defines ZenFoundation::Foundation
include(External/ZenAssetProcessor.cmake)  # reuses it
include(External/ZenRHI.cmake)             # reuses it
add_subdirectory(ZenCore)                  # links ZenFoundation::Foundation and ZenRHI::ZenRHI PUBLIC
```

- **Integration files:** `External/ZenFoundation.cmake` and `External/ZenRHI.cmake` follow [External/ZenAssetProcessor.cmake](../External/ZenAssetProcessor.cmake):
  - By default each uses its submodule, kept at GitHub main.
  - `ZEN_FOUNDATION_SOURCE_DIR` and `ZEN_RHI_SOURCE_DIR` build against the development checkouts, so a change to the RHI and the renderer can be tested before anything is pushed.
  - `ZEN_FOUNDATION_USE_INSTALLED` and `ZEN_RHI_USE_INSTALLED` select installed packages.
- **One ZenFoundation per build:** ZenRHI and ZenAssetProcessor add their own copy only when `ZenFoundation::Foundation` does not exist yet.
- **ZenCore links both libraries PUBLIC,** because 20 of its public headers include RHI headers.
- **Shader include path:** engine shaders compile with ZenRHI's include directory, for `ZenRHI/Shader/Bindless.h`.
- **Build provenance:** a generated build-information header records the commit of ZenEngine and of each submodule, each with a flag for uncommitted changes. ZenApp writes these into capture metadata.

### Use outside ZenEngine

```cmake
FetchContent_Declare(ZenRHI GIT_REPOSITORY https://github.com/proton991/ZenRHI GIT_TAG v0.2.0)
FetchContent_MakeAvailable(ZenRHI)        # fetches its pinned ZenFoundation when none exists
target_link_libraries(MyApp PRIVATE ZenRHI::ZenRHI)
```

An installed package works as well: `cmake --install` installs both libraries, and `find_package(ZenRHI CONFIG)` finds ZenFoundation through `find_dependency`. Users need:

- **The Vulkan SDK.** ZenRHI fetches everything else privately.
- **A window handle** from any window library, passed as an `RHIWindowHandle`.
- **SPIR-V bytes** for shaders.

The README and `examples/Triangle` document the API. Version 0.x supports one device per process.

### Versioning

Every Zen repository is released together under one version, starting with v0.2.0: ZenFoundation, ZenRHI, ZenAssetProcessor, ZenEngine, ZenTest and ZenSample all carry the same tag, including repositories that did not change. A fix in one repository is a patch release of the whole set.

| | Development inside ZenEngine | Releases |
| --- | --- | --- |
| Source | Every submodule follows GitHub main | One `vX.Y.Z` tag in every repository, 0.x while the APIs are unstable |
| ZenFoundation | The engine's submodule | ZenRHI and ZenAssetProcessor fetch the ZenFoundation tag of their own version (`v${PROJECT_VERSION}`) |
| Order | Push ZenFoundation, then ZenAssetProcessor or ZenRHI, then ZenTest, then ZenEngine | Tag ZenFoundation, then ZenRHI and ZenAssetProcessor, then ZenEngine, ZenTest and ZenSample |
| History | Engine commits that need newer submodule commits update the submodule pointers | A release is rebuilt from its ZenEngine tag with `-DZEN_UPDATE_SUBPROJECTS=OFF`, which keeps the pinned submodules |

- **One version per repository:** each repository sets it once, in `project(VERSION …)`. The ZenFoundation requirement, the fetch tag, the package dependency, the ABI check and ZenAssetProcessor's `LibraryVersion()` derive from it. A release changes that line in every repository, plus the tag in the two compatibility workflows.
- **Version checks:** ZenRHI and ZenAssetProcessor reject a ZenFoundation of another major.minor. ZenEngine rejects a ZenFoundation, ZenRHI or ZenAssetProcessor checkout or package from another release.
- **File formats:** ZenAssetProcessor's `PackageSchemaVersion` and `ProcessorVersion` describe the package files. They change only with the format, not with releases.
- **Additive changes:** a change that spans repositories adds the new API, moves callers over, and removes the old API later, so main builds in every repository between pushes. A version bump is the exception: bump every repository in one sitting.
- **Version records:** each library generates a `Version.h` with its version and commit, and ZenApp logs the version and commit of every repository.
- **CI:** ZenRHI and ZenAssetProcessor build against ZenFoundation main and against the release tag. ZenEngine's CI builds with every submodule at main.

### Test ownership and refactor

Use the assertion to decide ownership. A test that verifies the handoff between two components belongs in `ZenEngine/tests/integration/`, even when both components are standalone libraries. An allocator or helper used only for fixture setup does not create a second owner. Split mixed files by case before moving their fixtures or changing their link targets.

| Owner | Cases and fixtures |
| --- | --- |
| ZenFoundation, Phase 1 | Move the container, cache, smart-pointer, memory, paged allocator, lock and verification cases from ZenTest. Split the `HeapVector` and thread-pool regressions out of [CleanupRegressionTests.cpp](https://github.com/proton991/ZenTest/blob/44f991895f3d4215285c0ef370b28257e8fecf4e/CommonTest/CleanupRegressionTests.cpp). Use the existing drain and restart assertions for the thread pool. Its standalone `ThreadPoolTest` program only logs a result and is absent from CTest. |
| ZenRHI, Phase 5 | Move RHI executor, command recording, GPU timing, resource lifetime and Vulkan backend cases. Keep Vulkan-private helpers inside the backend tests. |
| ZenEngine, with the relevant split | Move cross-boundary cases and their registration into the main repository. Examples include Foundation pointer ownership interacting with RHI-thread destruction in [RefCountPtrTests.cpp](https://github.com/proton991/ZenTest/blob/44f991895f3d4215285c0ef370b28257e8fecf4e/SmartPtrTest/RefCountPtrTests.cpp), RenderCore frame retirement against RHI completion in [RHIThreadingTests.inl](https://github.com/proton991/ZenTest/blob/44f991895f3d4215285c0ef370b28257e8fecf4e/RenderCoreTest/RHIThreadingTests.inl), and engine resize callbacks during RHI waits in [RHIWindowThreadingTests.cpp](https://github.com/proton991/ZenTest/blob/44f991895f3d4215285c0ef370b28257e8fecf4e/CommonTest/RHIWindowThreadingTests.cpp). Generic pointer cases go to Foundation and executor-only cases go to RHI. |
| ZenTest, alongside Phases 1 and 5 | Review remaining single-component engine suites, such as input, editor model and Python tool tests. Move any app, renderer, editor or UI cases that verify a handoff to the main repository. |
| ZenAssetProcessor, Phase 9 | Keep processor-only format, import, CLI and package cases in the processor repository. Move the [ZenEngine consumer cases](../ZenAssetProcessor/tests/integration/zenengine/README.md) that verify the runtime adapter, Vulkan upload or editor rendering into the main repository. |

During each move, consolidate cases that exercise the same failure through the same path. Preserve cases with distinct triggers, such as CPU ownership transfer versus destruction after GPU completion. Use the smallest fixture that reproduces each failure. Keep benchmarks and manual probes separately runnable. An existing suite can stay as written when its cases already meet these criteria.

## Decisions

Agreed on 2026-11-21:

1. ZenFoundation is a separate repository, used by ZenEngine, ZenRHI and ZenAssetProcessor.
2. ZenRHI ships with ZenFoundation. Users get it automatically and never write code against it.
3. The five subprojects sit side by side at the ZenEngine root.
4. ZenAssetProcessor uses ZenFoundation internally.
5. Development follows main, and releases pin versions.

Adopted from the 2026-11-21 recommendations; change them here if needed:

6. Include paths are prefixed (`ZenFoundation/...`, `ZenRHI/...`), and macros are prefixed with `ZEN_`, with an engine-only compatibility header.
7. ZenRHI gains opt-in deferred release. This partly reverses R9; RenderCore keeps its frame-slot and render-graph policy on top.
8. The RHI globals remain for 0.x, which limits a process to one device. Removing them is later work.
9. The engine keeps its SDL3/GLFW surface creation through a custom surface provider, so its behaviour does not change. Other users pass native handles.
10. Foundation types may remain as private members of public classes (boundary rule 2). This makes ZenFoundation a PUBLIC dependency of ZenRHI. Users still never name its types.

Agreed on 2026-11-22:

11. ZenFoundation and ZenRHI ship their own tests. ZenEngine tracks the sources, fixtures specific to the integration, and CTest registrations for cross-boundary cases directly in its main repository.

## Phases

Each phase lands as one or more commits and leaves main buildable. After Phase 2, the RHI track (Phases 3–8) and the asset-processor track (Phase 9) can run in parallel.

### Phase 0: Baseline

- Use the latest fetched `hybrid-gi` as the local baseline.
- Inventory first-party suites from CMake targets, CTest registrations and runner scripts, including opt-in suites, benchmarks and unregistered executables. In the verification record, track each suite's destination and which behavior a surviving case covers when cases merge or are removed. Review mixed suites at case level.
- Record into `Doc/LibrarySplitVerification.md`:
  - Results of the registered Debug and Release tests (`tools/check_pr.py`).
  - The hardware RHI suites (`tools/verify_rhi_production.py`, including `VulkanRHIIntegrationTest`), and the SwiftShader job.
  - A ZenApp smoke run (`verify_rhi_production.py --smoke --setup=<file.zenrender>`).
  - ZenApp frame timing, with an A/A repeat to measure noise, recorded under the AGENTS.md profiling rules (executable hash, arguments, setup hash, extents, backend, async-compute state).
  - The `EncoderAllocationBenchmark` result.
  - A New Sponza import with `zenassetc`: time and package hashes.

### Phase 1: ZenFoundation inside the engine repository

- Create a top-level `ZenFoundation/` folder with its own CMake project, which also configures standalone. Move into it:
  - `Templates/`, `Utils/` (including `ThreadPool`) and `Memory/`. Check whether `MetricsLogger` is engine-specific.
  - `ObjectBase.h` and `Memory.cpp`.
- Rewrite the includes to `ZenFoundation/...` with a script, across ZenCore, ZenUI, ZenApp, ZenEditor, ZenTest, ZenSample and the ZenAssetProcessor integration tests.
- Add the prefixed macros and the engine compatibility `Utils/Errors.h`.
- Move `ZEN_WIN32`, `ZEN_MACOS` and `ZEN_DEBUG` onto the ZenFoundation target, and add `ZEN_FOUNDATION_ALLOCATION_TRACKING` with its default set to today's behaviour.
- Move the Foundation cases in the ownership table into `ZenFoundation/tests` in this phase. Link them to `ZenFoundation::Foundation` and run them in a standalone build. Update ZenTest sources and registrations in the same cutover. Replace the direct `Memory.cpp` entry in `ZenRenderCoreTestSources` with the Foundation target.
- Register `tests/integration` from ZenEngine's root CMake under `BUILD_TESTING`. Move the Foundation boundary cases here, starting with the smart-pointer/RHI lifetime case. Preserve each suite's platform and feature conditions.
- Add ZenFoundation to `tools/format_active.py` and `tools/check_no_exceptions.py`.
- Push the ZenTest changes and the ZenAssetProcessor integration-test changes before the engine commit.
- **Done when:**
  - A standalone ZenFoundation configure, build and CTest run passes without an engine checkout. Engine CTest discovers and runs the moved boundary cases from the main repository.
  - The engine passes the Phase 0 tests.
  - ZenApp timing is within A/A noise.

### Phase 2: Publish ZenFoundation

- Extract `ZenFoundation/` with its history using `git filter-repo` on a copy of the repository. Add path renames so the history from `ZenCore/Include/Templates` and the other old locations is kept.
- Record the source ZenTest commit and paths for transferred tests. The engine's history records a ZenTest submodule pointer, so filtering ZenEngine alone cannot recover that test history. Apply the same provenance rule to ZenRHI in Phase 8.
- With the user's approval:
  - Create `proton991/ZenFoundation` and push it.
  - Add it as a submodule with `External/ZenFoundation.cmake`, and register it with `zen_update_subproject`.
  - Add an AGENTS.md rule like the ZenAssetProcessor one.
  - Run the standalone Foundation suite in the new repository's Windows CI.
- Remove the copy inside the engine repository.
- **Done when:**
  - A fresh recursive clone builds.
  - The source-folder override and installed-package modes work.

### Phase 3: Remove the RHI's engine dependencies, inside ZenCore

- **Shaders:**
  - `RHIShaderCreateInfo` carries SPIR-V bytes and per-stage debug names.
  - File loading moves to `rc::ShaderProgram`, which already fills the create info, and the RHI stops using `SPV_SHADER_PATH`.
  - Check every user of `RHIShader::GetCreateInfo()`.
- **Window:** `RHIWindowHandle` holds one of:
  - Win32 `hinstance` and `hwnd`.
  - A Metal `CAMetalLayer*`.
  - A custom surface provider: the required instance extensions and a create-surface callback.

  The engine passes a custom provider that wraps today's SDL3/GLFW calls and the `NativeWindow` thread check, so its behaviour is unchanged. Implement and test the native Win32 path here. Implement the Metal path too, and verify it when a Mac is available.
- **Small removals:**
  - Replace `Timer` with `std::chrono`, and `Rect2`/`Vec3i` with RHI types.
  - Move `Format.h`, `Color.h` and `Bindless.h` under the RHI, and add its include path to the GLSL compile command.
  - Remove `ZEN_WINDOW_*` from RHI code.
- Tell other sessions before landing. The changed shader include path rebuilds every SPIR-V file in the shared `Data/SpvShaders`.
- **Done when:**
  - No RHI file includes `Platform/`, `Math/`, `Graphics/Common/`, `Graphics/Shared/`, SDL or GLFW.
  - The Phase 0 tests pass and ZenApp timing is within noise.

### Phase 4: Public boundary types

- Apply boundary rule 1 one header at a time, in this order:
  1. `VectorView` → `std::span`.
  2. Descriptor fields (`NameID`, `HashMap`, `SmallVector`, `BitField`).
  3. Out-parameters.
  4. Smart pointers in signatures.
- Move inline verification and logging out of the public headers, or switch it to the `ZEN_` macros (rule 3).
- Update the RenderCore call sites. Most compile unchanged, because `HeapVector` converts to `std::span`.
- Check public RHI declarations against types and aliases declared by the selected Foundation SDK. Cover public descriptor fields, parameters, return types and aliases.
- **Done when:**
  - The check passes.
  - `EncoderAllocationBenchmark` and ZenApp timing are within noise.

### Phase 5: ZenRHI as a top-level project inside the engine repository

- Create `ZenRHI/` with the target layout and the targets `ZenRHI::Core`, `ZenRHI::Vulkan` and `ZenRHI::ZenRHI`. Make the Vulkan headers private.
- Rewrite `Graphics/RHI/...` includes to `ZenRHI/...`.
- Find dependencies with `find_package(Vulkan)`, and with `if(NOT TARGET …)` for volk, VMA and spirv-reflect.
- Replace `DynamicRHI::Create` with `CreateRHI` in `ZenRHI::ZenRHI`.
- Move and refactor the RHI cases into `ZenRHI/tests` with Core and Vulkan. Split `VulkanRHITest`, `VulkanRHIIntegrationTest` and the mixed RenderCore translation units by the ownership table. Port library fixtures that use engine file loaders or window helpers for setup to SPIR-V bytes or native handles. Move their shader build rules and resolve inputs within ZenRHI.
- Move the remaining renderer/RHI, window/RHI and other cross-component cases from ZenTest into `ZenEngine/tests/integration/`. Exercise the handoff through the components' supported interfaces. Keep engine shader-layout tests with the engine shaders they validate and generic SPIR-V reflection cases in ZenRHI.
- `ZenRenderCoreTestSources` drops the RHI sources and links `ZenRHI::Core`. Core owns the RHI globals once. Adapt the fake factory and globals currently defined in [RenderCoreTests.cpp](https://github.com/proton991/ZenTest/blob/44f991895f3d4215285c0ef370b28257e8fecf4e/RenderCoreTest/RenderCoreTests.cpp) so engine tests can supply a fake without linking Vulkan or defining Core symbols twice.
- Split the current SwiftShader job and update the hard-coded suite lists in [verify_rhi_production.py](../tools/verify_rhi_production.py) and [check_quick.py](../tools/check_quick.py) at cutover. Run RHI suites from the library build and engine suites and ZenApp smoke from the engine build. Preserve the validation setup from `ZenVulkanTestMain` for native suites in both builds. Keep the software-driver and hardware runs separately selectable.
- Export the fake backend and scoped Vulkan hook as the optional `ZenRHI::TestSupport` package component. Keep engine probes that require private backend headers available only in source builds. Verify the installed configuration with tests enabled and a nonexistent RHI checkout path.
- Add ZenRHI to the format and no-exceptions tools.
- **Done when:**
  - ZenRHI builds with its declared dependencies and runs its CPU and applicable Vulkan suites without ZenCore, ZenTest or engine shader assets. Hardware skips are recorded separately from passes.
  - The engine runs its retained integration suites against the new targets. Reconcile the Phase 0 inventory for all of ZenTest, including app, editor, UI and tooling tests, and verify that each retained suite runs from its owner's entry point.

### Phase 6: Example and documentation

- Add `examples/Triangle`; only the example uses SDL3.
- Split the RHI README. The RHI contracts move to ZenRHI's documentation; the RenderCore policy (R9, ZenUI ownership) stays in the engine documentation.

### Phase 7: Library API

- **Deferred release:** add `ReleaseWhenComplete(resource)` and `CollectGarbage()` to Core, built from `ResourceRetirement`. RenderCore keeps its policy on top.
- **Creation:** `CreateRHI(const RHIDeviceDesc&)` carries today's `RHIOptions` fields. The engine fills the description from its launch settings.
- **Message callback:** add one for logs and verification failures, defaulting to standard error.
- **Packaging:** `install(EXPORT)`, and a `ZenRHIConfig.cmake` that calls `find_dependency(ZenFoundation)`. Add a test that builds a consumer from a relocated install prefix, as was done for ZenAssetProcessor.

### Phase 8: Publish ZenRHI

- Extract the folder with its history, as in Phase 2.
- With the user's approval: create `proton991/ZenRHI` and push it, add the submodule and `External/ZenRHI.cmake`, and add the AGENTS.md rule. Transfer the standalone RHI CI job from Phase 5 to ZenRHI's root workflow directory. ZenEngine continues running its cross-boundary suites.
- Remove the copy inside the engine repository.
- **Done when:**
  - A fresh recursive clone builds.
  - The override and installed-package modes work.
  - `examples/Triangle` builds against an installed package outside the engine.

### Phase 9: ZenAssetProcessor uses ZenFoundation (after Phase 2)

- Work in `E:\Dev\ZenAssetProcessor`, in its own session, since asset-processor work is kept separate from rendering sessions.
- **Linking:** link ZenFoundation PRIVATE. Built standalone, it fetches a pinned tag; inside the engine, it reuses the engine's target. `ZenAssetConfig.cmake` calls `find_dependency(ZenFoundation)`.
- **Before migrating hot paths:** time a parallel New Sponza import with `HeapVector` against `std::vector`, because of the global allocation-tracking lock. If the lock costs time, decide whether tracking defaults off in Release.
- **Migration in `src/`,** one step at a time:
  - `std::vector` → `HeapVector` (383 uses).
  - `assert` → `ZEN_ASSERT` or `ZEN_VERIFY` (16 uses).
  - Threads and mutexes → foundation types where they fit.
- **What stays `std`:** the public headers, and calls into fastgltf, draco, meshoptimizer, basis and the other third-party libraries.
- Update its AGENTS.md (rule 4 now applies to ZenFoundation's containers) and its CI (ZenFoundation main and the pinned tag).
- Move processor-to-engine consumer cases, their integration-only fixtures and CMake registrations into `ZenEngine/tests/integration/`. Keep the asset suites opt-in through the engine build and update their CI invocation. Preserve the shared corpus used by standalone processor tests and resolve it through a fixture path for engine integration runs. Retire the old processor registrations once the engine runs their replacements. Make the processor-side changes in its standalone checkout and update the engine's submodule pin to the published commit.
- **Done when:**
  - The processor-only suites pass standalone. The opt-in consumer suites run from the main ZenEngine checkout with the relevant asset tools, Vulkan and editor targets enabled.
  - Package hashes equal the Phase 0 baseline.
  - Import time is within noise.

### Later

- Remove the RHI globals by passing the device to the objects that need it.
- Make the first release: v0.2.0 in every repository.
- Decide whether `ZEN_DEBUG` and allocation tracking stay on in Release.
- Optionally use pimpl for heavy internals, so ZenRHI can link ZenFoundation PRIVATE.
- Replace spdlog in ZenFoundation with fmt or `std::format` plus the message callback.

## Verification rules

- **Builds:** use the check tooling in the Visual Studio environment; a plain `cmake` from Git Bash fails at link.
- **During the split:** run the affected owner's suite and the consumer checks that exercise the changed boundary. Use hardware validation for device-side changes and compare ZenApp timing to Phase 0 for changes on frame or allocation paths. At the library cutovers, compare the relevant Phase 0 results, including native and SwiftShader runs for the RHI. Reuse a result when two runners invoke the same suite with the same configuration, inputs and driver.
- **ZenApp:**
  - Rebuild and reuse `build/x64-windows-msvc-release/bin/ZenApp.exe` with `--setup=<file.zenrender>`, and record the hashes and arguments.
  - Close RTSS before native runs.
  - Run Nsight performance traces at 3840×2160, verify the framebuffer and render extents, compare against A/A noise and repeat deciding runs.
- **Results:** record them in `Doc/LibrarySplitVerification.md`.

## Risks

- **Conflicts with RHI and GI work:** Phases 4 and 5 touch every RHI file. Land them quickly, without parallel RHI feature branches.
- **Two ZenFoundation copies in one build:** this would give two allocators and two `NameID` tables. The `if(NOT TARGET …)` guards prevent it, and the configure-time version check reports a mismatch.
- **Layout mismatches:** settings that change layout are defined only on the ZenFoundation target, never per consumer.
- **macOS:** the Metal surface path cannot be verified on the Windows machine.
- **Conflicting spdlog or fmt copies** in a user's project: existing targets are reused.
- **History extraction:** run `filter-repo` on a copy, and check `git log --follow` and blame on the moved files before publishing.
- **Shared shader output:** the Phase 3 shader rebuild changes the SPIR-V that other sessions run.
