# Reader source-download boundary

Verified 24 October 2026. The standalone Reader now builds from a bundled,
reviewed decoder subset instead of fetching the full meshoptimizer and Zstandard
archives. Fresh Debug and Release builds fetch only bcdec, with no source
importer or encoder dependency download. This resolves the source-download gap
identified by the [installed-runtime checkpoint](ZenAssetProcessorInstalledRuntimeVerification.md).

The change is in `E:/Dev/ZenAssetProcessor`. Its
[dependency notes](../../ZenAssetProcessor/docs/ReaderDependencies.md) describe
the 39-file subset, upstream archive provenance, license retention, hash checks
and repeatable export procedure. Decoder logic is unchanged; the generated
meshoptimizer files use LF instead of the previous Windows CRLF line endings. Both
Processor and Reader compile the same bundled files; Processor configuration
checks them against its full pinned dependency sources.

## Verification

| Check | Result |
| --- | --- |
| Fresh Ninja Release Reader build/tests | 4/4 pass |
| Fresh Ninja Debug Reader build/tests | 4/4 pass |
| Full Processor Release rebuild/tests | 33/33 pass |
| Full Processor Debug rebuild/tests | 33/33 pass, 452.60 seconds |
| Installed Debug/Release public consumers | 16/16 checks pass |
| Re-export from verified upstream archives | All 39 files and hash manifest identical |
| Modified decoder source in isolated copy | Next build fails with the expected integrity diagnostic |
| Restore that isolated source | Build succeeds |

The new build directories are standalone `build/reader-source-release` and
`build/reader-source-debug`. Each compiler database has 31 translation units:
27 production sources and four tests. Fourteen production units are the bundled
decoders. The only directories under each fresh `_deps` are bcdec's source,
build and population directories. Inspection of the compiler input lists finds
no importer, writer, encoder, compressor or dictionary-builder translation unit.
The existing `build/processor-port` configuration supplies the full Processor
Debug and Release results. The Debug cook and fixture tests take about 214 and
223 seconds; the process remained live and completed normally.

Each Reader build was installed into its own prefix and consumed through
`find_package(ZenAsset ... COMPONENTS Format Reader)`. The installed packages
contain the same five Format/Reader/decoder archives. `/MT` Release and `/MTd`
Debug are paired with their matching consumers. Each consumer is copied into
an isolated directory with six cooked v3 packages and one truncated package;
the launch environment removes compiler/SDK paths. Checks cover the default
version/BC preview path, geometry, Zstandard, mesh coding, raw arrays, BC arrays,
BC6H and truncated-package rejection. This is CPU decoding and preview evidence,
not another native rendering or upload claim.

The source integrity probe copies only the bundle and CMake verification files
to a temporary project. It configures successfully, changes one copied Zstandard
source, and confirms that `cmake --build` reruns configuration and fails before
compilation. Restoring the bytes makes the next build succeed. Neither actual
checkout is modified by this negative test.

## Evidence and limits

[Results and commands](../../ZenAssetProcessor/build/reader-source-evidence/final/results.json)
record archive hashes, re-export equality, dependency/source lists, the integrity
probe, consumer commands, package hashes and outcomes. Configure/build/test/
install logs are retained beside the standalone build directories. The
[identity manifest](../../ZenAssetProcessor/build/reader-source-evidence/identities.json)
records the changed CMake files, bundled sources, manifests, installed archives,
consumers and completed test results. The later full Processor Debug result is
retained in its own
[checkpoint](../../ZenAssetProcessor/build/reader-source-evidence/processor-debug-checkpoint/identities.json),
including the completed logs and executable identities. Build logs include
upstream Basis language-extension and simdjson unused-parameter warnings.

No engine renderer source, public API or package schema was changed. Existing
installed-engine evidence remains valid for its frozen binaries; this checkpoint
verifies the new standalone source and consumer builds. Other-host support,
remaining profile/atlas gates, final release packaging and the engine's release
pin are still open.
