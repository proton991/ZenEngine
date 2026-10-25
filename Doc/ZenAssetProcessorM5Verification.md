# M5 implementation and verification status

23 October 2026. The opt-in BC7 path is implemented from standalone cooking
through package loading, Vulkan upload and bounded editor previews. M5 remains
open for BC4/BC5/BC6H integration, deterministic GI comparisons, desktop visual
acceptance and detailed upload/allocation measurements.

The [texture quality and rendering report](ZenAssetProcessorM5VisualVerification.md)
contains the frozen binary identities, 11 independent CPU audits, 96 renderer
runs, 15 exact raster reference comparisons, memory tradeoffs and capture
limitations. In particular, identical raw packages vary by one 8-bit unit in
some GI channels, and accepted BC7 normal maps can shift sharp glossy highlights.
No tolerance was relaxed and desktop processing remains disabled by default.

## Standalone processing and package checks

The final BC7 Release build passed all **25 CTest suites**, including the
role classifier, per-image/mip quality kernel, Cook API, CLI, format validation,
scene serialization, codec and bounded-preview tests. Evidence is in
`E:/Dev/ZenAssetProcessor/build/processor-port/m5-texture-final-build.log` and
`m5-texture-final-tests.log`. These runs predate subsequent OBJ and BC4/BC5/BC6H
work and do not certify later changes.

Coverage includes shared color/data/normal uses, variants and animated UVs,
authored mips, tiny/odd block footprints, alpha-mask coverage, exact-alpha
fallback, immutable ownership, allocation limits and cancellation. Non-unit or
animated normal scales and vertex/morph alpha receive conservative exclusions.
Scene validation checks every BC7 block's reserved-mode prefix, including tiny
mips; sampled thumbnail decoding is not the full-payload validator.

The engine's source/Cook/adapter, explicit package and automatic cache paths
also passed an actual BC7 case using shared color/data/normal inputs. It checks
encoded block bytes, mips, immutable original pixels and complete adapted
records across cache miss/hit. Both focused cache/identity tests passed; logs
are in `build/asset-integration-release/m5-cook-cache-tests.log` and `.json`.

## Native engine and preview boundary

[Native boundary evidence](../build/asset-processor/m5-engine-boundary-results.json)
records 17 adapter/scene I/O tests, 703 RenderCore tests (nine existing disabled),
six RHI creation tests and four native Vulkan cases. The latter verify BC7
linear/sRGB storage and views, odd/tiny mip uploads and byte-exact readback on
an RTX 5080. All passed with no validation errors or remaining VMA allocations.
The supported physical-device BC compression feature is enabled at device
creation. Unsupported texture creation prevents scene publication and retains
the previous scene.

Reader previews use bounded output and fixed block scratch. The reader-only
Debug tests and installed consumer exercised BC7 previews without processor or
encoder linkage; the editor model suite passed 42 cases. Preview verification
does not imply full-scene rendered acceptance.

## Instrumented reader exercise

The Reader, Format, transport/mesh decoders and block decoder were rebuilt with
Clang 22 AddressSanitizer and libFuzzer. All four instrumented test suites
passed. Successful fuzz reads also exercised bounded RGBA8/BC7 previews for
each image/mip and pre-cancelled preview requests.

The seeded run completed **15,781 executions in 121 seconds** with no sanitizer
or invariant failure: seed 45678, 256 KiB maximum input, 5-second input timeout,
1 GiB RSS limit, reported peak RSS 449 MiB. Seeds include tiny and odd BC7
packages plus raw, Zstandard and mesh-compressed scene resources. Repaired
integrity fields allow mutation to reach semantic validation.

Evidence: `E:/Dev/ZenAssetProcessor/build/reader-fuzz/bc7-verification.json`,
`bc7-seeds.json`, `bc7-tests.log` and `bc7-run.log`. This finite run covers the
reader and sampled previews; it does not establish exhaustive malformed-input
safety or cover source importers and encoders.

## Subsequent BC4/BC5 development

Texture resource version 2 adds the explicit BC5 positive-Z normal marker;
metadata version 6 records the new codec flags and HDR error limit. A shared
processing-activation predicate keeps reference-profile checks consistent
between CLI and engine tools. All settings participate in cache identity.

Six focused schema/Cook/Writer/Format suites and the CLI suite passed in Release
(`E:/Dev/ZenAssetProcessor/build/processor-port/m5-formats-schema-tests.log`
and `m5-formats-cli-tests.log`). Tests include Texture-v1 compatibility,
BC4/BC5 odd/tiny payload round trips and unknown/mismatched interpretation
rejection. The processor image/classifier suites passed 2/2 separately. These
checks do not yet establish native rendered acceptance for BC4/BC5.

The CLI frozen for the independent audit is
`build/asset-processor/m5-bc45-binaries/zenassetc.exe`, SHA-256
`a4cc5cc83f1aa6c55d103a57113bac2d0da90499b77bb6565a1fac69ddddb0ec`.
The [BC4/BC5 report](ZenAssetProcessorM5BC45Verification.md) records 15 exact
default/reference and source-free roundtrips, 48 independent mip measurements,
native GPU checks and rendered evidence. Four BC5 images and five BC4 images
were actually compressed. Role splits can increase texture memory and package
size; image-space limits do not imply glossy rendered equivalence.

Final engine checks passed 38 CommonTest cases, 43 editor model cases, 703
RenderCore cases, 30 reflection/resource cases, two editor rollback cases and
three native GPU cases, with no reported engine or GPU leaks. The BC5 marker
reaches both base and clearcoat normal reconstruction; the appended material
flag field is at byte 1248 with a 1264-byte record stride.

BC6H immutable public environment records and cooking/upload integration are
being [validated separately](ZenAssetProcessorM5BC6HVerification.md). The full
standalone Release suite passes 27/27 after that public/schema change.
The earlier fuzz run above predates these schema
changes and does not certify them.
