# OBJ/MTL extraction verification

The standalone OBJ backend and engine source/cache/package integration are
verified for the documented bounded subset. This completes the OBJ item in M7;
batch manifests, editor profile selection and other backend evaluations remain
separate work.

The backend privately consumes pinned tinyobjloader and earcut dependencies.
It preserves points, lines and triangulated planar faces, group/material order,
independent corner indices, smoothing, UV orientation, colors and explicit
normal maps. Material conversion, unsupported features and resource limits are
specified in [ObjImport.md](../../ZenAssetProcessor/docs/ObjImport.md).
Reader-only consumers do not acquire OBJ parsers.

Seven focused Release CTests passed in 1.03 seconds, including 68 OBJ checks,
import control/fixtures/limits, scene serialization and CLI behavior. Logs are
`E:/Dev/ZenAssetProcessor/build/processor-port/m7-obj-build-final.log` and
`m7-obj-tests-final.log`. Regressions cover comment/continuation handling,
optional texture-transform components, normal strength/cancellation, explicit
zero factors, texture-only factors and explicit-only `illum 0` unlit behavior.

The final CLI cooked OBJ+MTL with meshoptimizer and Zstandard, then validated
the package while both source files were renamed unavailable. Evidence is in
`E:/Dev/ZenAssetProcessor/build/processor-port/m7-obj-cli-evidence/`.

The two engine OBJ/discovery tests passed against the final parser within the
38/38 focused run in
`build/asset-integration-release/bc45-common-tests-final.log`, with zero reported
engine leaks. They exercise source/cache/package agreement, MTL invalidation,
format discovery and loading after source removal.

MTL-to-PBR mapping is an approximation, not a claim of equality with Phong
rendering. Nonplanar/projective/free-form inputs are rejected; packed texture
maps require matching extent, UV transform and wrapping. Limits bound declared
input/working storage, excluding parser-private allocator overhead and total
process RSS. No additional importer or portable profile is certified here.
