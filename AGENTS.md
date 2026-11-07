# C++ preferences

1. Use one return statement per function, at the end. Void functions may omit it.
2. Use explicit types; `auto` is allowed only for iterator types.
3. Use lambdas only when necessary and keep them short. Use named functions for long logic.
4. Prefer project containers, including `HeapVector`, over standard-library equivalents where suitable.
5. Format C++ with the project's `.clang-format`.
6. Reuse existing code or extract shared logic to reduce duplication.
7. Do not throw exceptions for error handling. Use status results, logs and assertions as appropriate, and stop invalid execution paths explicitly.

# Profiling workflow

- Run all application performance profiling through the standalone **ZenApp** executable with explicit startup arguments.
- Do not substitute `scene_renderer_demo`, an editor preview, or a renamed executable for ZenApp.
- Supply the scene/camera/render configuration using `--setup=<file.zenrender>` and use ZenApp startup arguments for run overrides. Do not temporarily edit `Data/engine.cfg` for profiling.
- Retain the executable identity, exact argument list, setup and input hashes with the capture. Record actual framebuffer and render extents, fullscreen state, backend and async-compute state.
- Use `tools/profile_hybrid_gi.py` for Nsight Graphics GPU Trace captures. Historical demo captures are demo measurements and must not be presented as ZenApp results.

# ZenAssetProcessor submodule workflow

- Make ZenAssetProcessor changes in the standalone repository at `E:\Dev\ZenAssetProcessor`, then commit and push from that repository.
- ZenEngine's `ZenAssetProcessor/` directory consumes published commits only. Fetch and update its submodule revision; do not edit files or copy unpublished changes into it.
