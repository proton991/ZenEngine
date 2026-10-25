# M7 manifest builds and installed build integration

Verified 24 October 2026. The standalone library provides strict versioned JSON
manifests, deterministic setting/content identities, bounded job admission,
cancellable workers and per-package atomic publication. The public API and its
limits are documented in `E:/Dev/ZenAssetProcessor/docs/BatchBuilds.md`.

The Release suite passed 29/29 tests (`build/processor-port/m7-full-tests.log`
in the standalone repository). After fixing CLI report ordering, the batch API,
batch CLI and existing CLI tests passed 3/3 (`m7-report-tests.log`). The batch suite
has 57 checks, including malformed manifests, aliases, corruption before reuse,
same-size/time dependency edits, missing preferred inputs, force, budgets,
cancellation and preservation before publication. A waiting test cancels while a
second batch is actually waiting for the process-wide gate. CLI coverage verifies
that redirected stdout/stderr contains complete JSON records.

## Installed consumption

The installed `ZenAsset::Compiler` and `zenasset_add_manifest_build` helper were
used by a separate CMake project with spaces in its manifest and output paths.
The first build cooked an OBJ/MTL fixture. A second invocation reused identical
package bytes without changing its write time. Editing the MTL while preserving
its size and time recooked the output. Evidence is in the standalone
`build/m7-installed-compiler-consumer/`, including `results.json` and per-run logs.
The earlier compiler hash in that report predates the report-order-only fix.
The final installed compiler is SHA256
`4bc2a45b9bb453f265094df42af719f9aab479af40a6f28af408038be93f8285`.
Its `validate` command succeeded with both source files temporarily absent;
the files were restored afterward (`source-free.log`).

A fresh Reader-only Release configure/build/install passed 4/4 tests. Its fetched
source directories contain only meshoptimizer, zstd and bcdec, and its installed
archives contain only Format, Reader and the three decoder libraries. A fresh
external consumer required Reader, rejected any Compiler/Processor/encoder target,
used the helper with an explicit host compiler, and read the cooked package with
both source files absent. All checks passed. Evidence is in
`build/m7-reader-only-*.log` and `build/m7-reader-consumer/`; `evidence.json`
records executable hashes and the actual dependency inventory. Its merged build
log also parsed as complete JSON records.

## Editor agreement

The actual installed CLI batch and editor source-recook fixture matched canonical
runtime scene bytes, profile/settings and fallback diagnostics in both inline and
threaded modes. It includes vertex remapping, BC7 pixels and a retained RGBA normal
fallback. Both packages loaded and rendered. The broader ten native editor tests
and all 707 RenderCore tests passed after fixing the debug cube's remaining source
dependency. See [editor verification](ZenAssetProcessorM7EditorVerification.md).

## Limits

Preflight imports changed jobs to discover all dependencies before publication;
execution imports them again. Small-fixture timing in the standalone batch docs
records this cost and does not establish large-scene throughput. Admission uses
requested-storage reservations, not an RSS cap; glTF/FBX canonical geometry
expansion still lacks an equivalent import-time preallocation bound. Independent
process writers require external coordination. Publication is atomic per package,
not across the entire batch. Batch outputs are explicit build products; automatic
cache eviction remains the engine tool layer's policy.

These checks close the implemented M7 batch/editor tasks. Atlas, remaining
compression visual/production-upload gates, portable-profile decisions and the
M8 release are separate outstanding work.
