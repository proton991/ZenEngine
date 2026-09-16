# Active C++ cleanup

Scope: RenderCore V2 headers and matching sources, RHI/VulkanRHI, shared engine
code, the current scene-renderer demo, and their tests. Legacy RenderCore, Val,
older sample applications, external dependencies, and disabled implementations
are excluded.

## Changes

- Consolidated `HeapVector` append and erase paths, preserving aliased append
  sources and nontrivial object lifetimes. Added filled construction and front
  access for migrated render-graph metrics arrays.
- Moved V2-owned dynamic arrays and worker-pool storage to `HeapVector`. Existing
  public scene/asset interfaces and third-party containers retain their types.
- Replaced inferred non-iterator types in the active Vulkan backend, scene
  accessors, platform callbacks, and fast glTF loader with explicit types.
- Extracted named GLFW callbacks, worker execution, texture batches, swapchain
  cleanup, extension lookup, and resource destruction. Consolidated texture
  loading, default texture creation, model-path construction, and array-view
  element deduction.
- Made worker shutdown join removed threads, supported resizing to zero, and
  used atomic reference counters for shared tasks and stop flags. Bound task
  arguments now use the same queue path, and enqueueing after shutdown throws.
- Corrected scene bounds for rotation/reflection, repeated bounds updates,
  transform cache invalidation, and repeated ancestor multiplication. Mesh
  equality now compares both operands; material equality handles absent textures.
- Corrected SPIR-V byte/element accounting, rejected incomplete reads, preserved
  supplied environment-map names, and avoided repeating material-data entries.
- Consolidated glTF image decoding and gave each asynchronous texture batch
  ownership of its results. Invalid glTF parse results no longer reach asset
  extraction; missing standalone images return empty texture data. Texture batch
  collection reads indices before moving ownership. Non-renderable glTF nodes
  retain their transforms, and descendants see them during loading.
- Shared the identical renderer sampler setup and shortened the scene-demo
  resize callback. Removed repeated light initialization.
- Migrated GPU test fixtures to descriptor set 1 for local resources; set 0 is
  reserved by the current global bindless layout. Removed CommonTest's hardcoded
  allocator-only filter so the complete executable runs by default.

## Build and formatting

The default CMake build contains the active renderer and tests. Historical
targets remain available through `-DZEN_BUILD_LEGACY=ON`; those targets still
require migration to the current RHI APIs.

From a Visual Studio developer shell:

```powershell
cmake --preset x64-windows-msvc-debug
cmake --build build/x64-windows-msvc-debug --parallel 8
ctest --test-dir build/x64-windows-msvc-debug -L unit --output-on-failure
$env:VK_LAYER_VALIDATE_SYNC = '1'
ctest --test-dir build/x64-windows-msvc-debug -L integration --output-on-failure
python tools/format_active.py --check
```

`python tools/format_active.py` applies the repository's `.clang-format` to the
219 active C++ files. `--check` verifies formatting without writing files.
Vulkan integration tests require a compatible GPU and validation layer.

VSCode can apply the same style on save with the Microsoft C/C++ extension:

```json
{
  "C_Cpp.clang_format_style": "file",
  "[cpp]": {
    "editor.defaultFormatter": "ms-vscode.cpptools",
    "editor.formatOnSave": true
  }
}
```

The global C++ preferences also record `.clang-format` usage and reducing
duplicated functions and logic, alongside the existing return, type, lambda,
and container rules. Error handling uses status/error codes, logs, and assertions
instead of throwing exceptions.

## Validation

- MSVC Debug: complete default build passed.
- Unit tests: 333 tests passed across eight executables, including 14 new
  regression tests for containers, scene data, glTF/image/file loading, and
  worker lifetime.
- Vulkan integration: all 230 tests passed with Khronos synchronization
  validation enabled; no validation VUIDs or synchronization hazards were reported.
- Sponza scene smoke: 32 frames each in inline and threaded RHI modes, including
  PBR/voxel mode switching, resize, and minimize/restore; both exited successfully
  with no validation errors or tracked leaks.
- Thread-pool sample: passed; allocator reported no leaks.
- Formatting and `git diff --check`: passed.

Native integration validation used the existing RTSS isolation procedure described
in [RenderCoreRHIThreadingVerification.md](RenderCoreRHIThreadingVerification.md):
process-local implicit-layer exclusions and an identical temporary `bin/7zFM.exe`
copy to avoid the installed overlay hook. The copy was removed afterward. Scene
smoke runs used the process-local exclusions. No overlay settings were changed;
Khronos validation remained enabled. Normal CTest integration runs on this machine
can fail or stall if the separate RTSS injection hook remains active.

Detailed local logs are in `build/cleanup-unit-details-final.log`,
`build/cleanup-integration-isolated-final.log`, `build/cleanup-integration.xml`,
and `build/cleanup-scene-mode-{0,1}.log`.

This run validates Windows/MSVC; macOS and Linux were not executed.

## FileSystem error-handling follow-up

`LoadSpvFile` and `LoadTextFile` now log failures, return empty data, and expose an
optional `FileLoadError` output. Successful reads reset the error to `eNone`;
empty text files succeed, while empty SPIR-V files are rejected. Shader creation
returns null on a file-load failure before reflection or native module creation.

The follow-up MSVC Debug build, all 336 unit tests, and all seven Vulkan pipeline
integration tests passed, including the new missing-shader-file case. Formatting
and diff checks passed. Logs are `build/filesystem-error-unit-details.log` and
`build/filesystem-error-vulkan.log`.

## Shared ownership follow-up

Active RHI command contexts, batches, completion events, and RenderCore extraction
state now use `RefCountPtr`. `RefCounted` supplies atomic reference counting and a
final-release hook; command contexts use that hook to preserve RHI-thread
destruction through `ZEN_DELETE`. `RHIResourcePtr` adapts `RefCountPtr` to the
existing `AddReference`/`ReleaseReference` protocol, preserving its counter and
destruction path without adding another control block.

`RefCountPtr` now handles self-assignment, replacement, and converting moves
correctly. Conversions require compatible pointer types and the same reference
policy. Final release acquires writes from previous releasing owners. `Reset`,
`Adopt`, and `Detach` make ownership changes explicit; unsafe implicit raw-pointer
conversion and the writable address-of overload were removed. Use `Get()` for
borrowed access. Each handle remains one pointer in size.

`SharedPtr` with `MultiThreadCounter` remains appropriate for `std::packaged_task`,
which does not implement intrusive reference counting. The remaining active
`std::shared_ptr` uses belong to spdlog's public logger/sink APIs.

`SharedPtr` now supports custom deleters and retains the original allocation
through aliases and type conversions. Self-assignment and move-assignment lifetime
bugs were fixed, and casts preserve the selected counter type. Seven regression
tests cover these ownership cases and concurrent reference counting. Six further
`RefCountPtr` regressions cover assignment, type conversion, explicit ownership
transfer, concurrent final release, and the RHI resource adapter. A RenderCore
regression verifies detached lists keep their context alive and the final owner
destroys it on the RHI thread.

The complete MSVC Debug build, all 350 unit tests, and all 231 Vulkan integration
tests passed. Integration tests used the RTSS isolation procedure above with
Khronos synchronization validation enabled. Scene smoke tests passed for 32 frames
each in inline and threaded RHI modes, with no validation errors or tracked leaks.
Formatting and diff checks passed. Logs are `build/refcount-unit.log`,
`build/refcount-integration-isolated-final.log`, `build/refcount-integration.xml`,
and `build/refcount-scene-{0,1}.log`.
