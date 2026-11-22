# C++ preferences

1. Use one return statement per function, at the end. Void functions may omit it.
2. Use explicit types; `auto` is allowed only for iterator types.
3. Use lambdas only when necessary and keep them short. Use named functions for long logic.
4. Prefer project containers, including `HeapVector`, over standard-library equivalents where suitable.
5. Format C++ with the project's `.clang-format`.
6. Reuse existing code or extract shared logic to reduce duplication.
7. Do not throw exceptions for error handling. Use status results, logs and assertions as appropriate, and stop invalid execution paths explicitly.
8. Keep tests focused on observable behavior, important invariants, and concrete regressions. Extend an existing test when it covers the same behavior. Add a case or test at another layer only when it can catch a distinct failure. Skip trivial checks and assertions that merely restate the implementation.

# Profiling workflow

- Run all application performance profiling through the standalone **ZenApp** executable with explicit startup arguments.
- Reuse `build/x64-windows-msvc-release/bin/ZenApp.exe` for application tests and profiling, rebuilding it in place as needed. Do not create per-run executable copies; retain hashes and capture metadata for provenance.
- Do not substitute `scene_renderer_demo`, an editor preview, or a renamed executable for ZenApp.
- Supply the scene/camera/render configuration using `--setup=<file.zenrender>` and use ZenApp startup arguments for run overrides. Do not temporarily edit `Data/engine.cfg` for profiling.
- Retain the executable identity, exact argument list, setup and input hashes with the capture. Record actual framebuffer and render extents, fullscreen state, backend and async-compute state.
- Capture Nsight performance traces at 3840×2160. Verify the actual framebuffer and render extents in the capture metadata.
- Use `tools/profile_hybrid_gi.py` for Nsight Graphics GPU Trace captures. Historical demo captures are demo measurements and must not be presented as ZenApp results.

# Submodule workflow

- Develop each submodule in its checkout under the ZenEngine root. Switch to its development branch before committing and push the branch to the submodule's own remote.
- Track the intended upstream branch in `.gitmodules`. Update ZenEngine's submodule pointers to the latest published commits on those branches.
- All sessions build from these checkouts. Keep unfinished experiments in a worktree or a separate checkout selected with `ZEN_FOUNDATION_SOURCE_DIR` or `ZEN_RHI_SOURCE_DIR`.
- Push in dependency order: ZenFoundation, then ZenRHI and ZenAssetProcessor, then ZenTest, then the ZenEngine commit that moves their submodule pointers.

# Releases

- Every Zen repository shares one version, set by `project(VERSION …)` in each repository. Bump all of them in one sitting, then tag every repository with the same `vX.Y.Z`, in the push order above.
- Build a release from its ZenEngine tag with `-DZEN_UPDATE_SUBPROJECTS=OFF` so the pinned submodules are used.
