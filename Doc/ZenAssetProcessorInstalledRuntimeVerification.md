# Installed Reader runtime verification

Verified 24 October 2026 on Windows x64, MSVC 19.51, Ninja Release and NVIDIA
GeForce RTX 5080. A fresh ZenEngine build consumes a relocated installed
`ZenAsset::Reader` package with the development source override empty. Both
runtime executables build, and **18/18 load, upload and rejection checks pass**
from a directory containing only those executables and cooked packages.

This is a current dependency-boundary checkpoint before release. It does not
complete M8, publish a release, or replace the outstanding compression and
spatial-atlas acceptance gates.

## Build and link boundary

The standalone build at `E:/Dev/ZenAssetProcessor/build/installed-boundary-reader`
has Processor, CLI and Processor installation disabled. Its four registered
tests pass. It installs Format, Reader and three private decoder archives:
block decoding, Zstandard and meshoptimizer. Both the library and consuming
engine use `CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded` (`/MT`). This configuration
pair is deliberate; the existing developer builds use a different CRT setting.
Static C++ libraries must be consumed with compatible toolchain/CRT settings.

The install prefix was moved from `reader-install` to `reader-relocated` before
the engine was configured. The original prefix is absent. Exported CMake target
locations resolve relative to the relocated prefix, and contain neither the
original install path nor the standalone source checkout path.

The new engine directory is `build/asset-installed-boundary/engine-runtime`.
Asset tools, editor, runtime UI and tests are disabled. The dependency source
directories already populated under engine `External` are reused with
`FETCHCONTENT_FULLY_DISCONNECTED=ON`; this is a fresh build directory, not an
empty dependency cache or a network-isolation experiment.

The audit checks compiler databases, Ninja's command closure for the two
executables, installed exports, decoder archive public symbols and PE imports:

- The engine database contains 363 translation units. None uses the standalone
  checkout, model parsers, Processor, or block encoder sources/includes.
- The Reader database contains 31 translation units, including four test sources.
  Production files are Format, Reader, block decoding, generated meshoptimizer
  decoders, and Zstandard common/decompression files. No compressor or dictionary
  builder translation unit is compiled.
- Both executable link commands use the five installed ZenAsset archives.
  There are no Processor, importer or encoder archives in their link closure.
  The engine also links its ordinary runtime libraries, including SDL, volk,
  SPIRV-Reflect, spdlog and its image-decoding kernels.
- Decoder archive symbols include meshoptimizer decode and Zstandard decompress
  entry points and exclude their checked encoder entry points.
- `ScenePackageSmoke.exe` imports only `KERNEL32.dll`. The upload executable's
  direct PE imports are Windows system DLLs; neither requires a ZenAsset or
  Visual C++ runtime DLL beside the executable in this `/MT` configuration.

At this checkpoint the standalone Reader source build still fetched complete
pinned meshoptimizer and Zstandard archives, which contain unused encoder source files. Therefore
this does **not** establish the plan's stronger condition of downloading no
encoder source in every processor-disabled source-build configuration. The
installed engine path avoids those fetches entirely. That source-distribution
condition was not established by this run. The later
[Reader source verification](ZenAssetProcessorReaderSourceVerification.md)
resolves it with a reviewed decoder subset and fresh Debug/Release builds;
the original installed-build evidence is retained unchanged.

## Runtime checks

The final run is recorded in
[results.json](../build/asset-installed-boundary/verified-runtime/results.json).
The runner copies two executables and eight valid packages into
`verified-runtime/runtime-capsule`, plus a deliberately truncated negative-test
package. There are no model source files, textures outside the packages, shader
files, SDK files, parser DLLs or encoder DLLs in this directory. Runs use that
directory as the working directory. Their environment retains ordinary Windows
profile/system variables, sets `PATH` to Windows and System32, and omits the
developer/SDK/CMake/ZenEngine environment variables.

| Input | Runtime load | Native upload, validation off/on |
| --- | --- | --- |
| Preserved v2 glTF complete-scene package | Pass | Not run |
| Preserved v2 FBX skin/morph package | Pass | Not run |
| Current v3 reordered animated geometry fixture | Pass | Both pass |
| v3 SimpleSkin with Zstandard | Pass | Not run |
| v3 SimpleSkin with meshoptimizer and Zstandard | Pass | Not run |
| v3 128x128 raw array-page fixture | Pass | Both pass |
| v3 128x128 BC7/BC5/BC4 array-page fixture | Pass | Both pass |
| v3 BC6H 7x7 environment, six faces and three mips | Pass | Both pass |

The package-directory inspection confirms v2/v3 versions, actual Zstandard and
mesh semantic encodings, and Image v2 array records. Every copied package's
SHA-256 matches its input. The upload checks read through the installed Reader,
adapt to engine asset data, construct the runtime scene and upload native Vulkan
images and canonical geometry buffers.

Both array packages create nine images, 15 views and 37 copy regions, including
the five fallback textures. Raw and BC payload totals are 611,680 and 131,252
bytes respectively. The HDR case uploads a native BC6H cube with six layers and
three explicit mips. Resource records sum to the reported totals. All eight
upload processes report zero validation errors, zero VMA bytes at teardown and
no tracked CPU leaks.

The two negative cases return failure with specific diagnostics: source-file
extensions require AssetTools, and a 31-byte package has a truncated header.
The source-path case checks runtime dispatch before opening a file; it is not
a source-importer conformance test.

The first runtime run also passed all 18 exit/sample checks. Its ordinary logger
interleaved with some resource JSON. The final run uses the benchmark's existing
`--output` option to separate metrics from logging and validates every resource
record. Both runs remain available. An earlier audit-script attempt stopped
before launching applications because it matched `find_package` inside a CMake
comment; the corrected audit checks actual command lines.

## Reproduction and evidence

From a Visual Studio x64 developer shell, the important build settings are:

```powershell
# Standalone repository; choose fresh build/install paths for another run.
cmake -S . -B build/installed-boundary-reader -G Ninja `
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded `
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DZENASSET_BUILD_PROCESSOR=OFF `
  -DZENASSET_BUILD_CLI=OFF -DZENASSET_BUILD_TESTING=ON `
  -DZENASSET_INSTALL_PROCESSOR=OFF `
  -DCMAKE_INSTALL_PREFIX=E:/Dev/ZenEngine/build/asset-installed-boundary/reader-install
cmake --build build/installed-boundary-reader --parallel 2
ctest --test-dir build/installed-boundary-reader --output-on-failure
cmake --install build/installed-boundary-reader

# Engine repository, after relocating the new install prefix.
cmake -S . -B build/asset-installed-boundary/engine-runtime -G Ninja `
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded `
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DBUILD_TESTING=OFF `
  -DZEN_BUILD_ASSET_TOOLS=OFF -DZEN_BUILD_EDITOR=OFF -DZEN_BUILD_RUNTIME_UI=OFF `
  -DZEN_ASSET_PROCESSOR_SOURCE_DIR= `
  -DCMAKE_PREFIX_PATH=E:/Dev/ZenEngine/build/asset-installed-boundary/reader-relocated `
  -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF -DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF `
  -DFETCHCONTENT_FULLY_DISCONNECTED=ON
cmake --build build/asset-installed-boundary/engine-runtime `
  --target ScenePackageSmoke SceneUploadBenchmark --parallel 2
```

The evidence directory retains configure/build logs, `ninja -t commands`
output, `dumpbin /linkermember:1` output for the three decoder archives, and
`dumpbin /dependents` output for both executables. The standalone build/test/
install logs are copied into its evidence snapshot. The saved
[runner](../build/asset-installed-boundary/verify_runtime.py) checks these inputs
and performs the runtime cases; `--output` must name a new directory. The
[identity manifest](../build/asset-installed-boundary/identities.json) records
the installed files, binaries, packages, logs, build descriptions and selected
source snapshot. It is not a complete frozen engine checkout.

## Limits

Both source checkouts remain on this machine. Compiler/link inputs establish
that the installed consumer does not use the processor checkout; copying the
executables and packages establishes the observed launch-directory contents.
No operating-system file-access trace or clean-machine test was performed.
The GPU harness dynamically loads the system Vulkan loader and driver; its
validation-on runs use an installed validation layer. Direct PE import listings
do not include those dynamic modules.

These are CPU scene-load and isolated graphics-queue upload checks. They do not
draw frames, animate the geometry on the GPU, exercise production staging pools,
or prove the shipping renderer's full deployment closure. Earlier rendering
reports provide their own scoped evidence. This local capsule is a verification
artifact, not a published redistributable release. Current Debug verification,
final platform/profile gates, release packaging and the verified release pin
remain open.
