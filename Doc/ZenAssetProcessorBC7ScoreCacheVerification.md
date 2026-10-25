# BC7 endpoint score-cache verification

The 2026-10-24 follow-up adds bounded endpoint-score reuse to the quality-4
filtered-normal search. Three paired full-model runs reduce median cooking time
from **27.047 to 23.954 seconds (11.4%)**, preserving every tested resource
payload apart from build metadata. The complete M5 gate and implementation plan
remain open.

## Change and correctness boundary

The previous [performance checkpoint](ZenAssetProcessorBC7PerformanceVerification.md)
removed redundant packing and endpoint trials. A new instrumented probe found
40,067,680 endpoint-score calls for the 1024x1024 clearcoat normal level. A
128-entry direct-mapped cache reuses 3,094,105 complete scores and 6,765,335
rejected partial scores: 24.6% of calls overall. Candidate generation falls
from 12.486 to 10.195 seconds; total refinement falls from 17.817 to 15.608
seconds. Those are single diagnostic runs, separate from the production timing
comparison below.

Production changes are limited to the standalone library's private
`src/Codecs/BlockEncoding.cpp/.h` and the scratch owner/call in
`src/Processing/BC7NormalRefinement.cpp`. Each entry stores all six RGB endpoint
bytes, the score, packed selectors and whether the score is complete. A hash
collision triggers recomputation. Alpha does not enter this angular score;
the existing image-level alpha guard still selects the published block.

A partial sum can only reject a trial with an equal or smaller cutoff. It is
never used as a completed score or accepted with incomplete selectors. A larger
cutoff forces reevaluation. Source changes invalidate every entry before the
next block search, so scratch reuse cannot cross source-block identities.
Candidate order, selector tie-breaking, floating-point summation and quality
thresholds remain unchanged. Cancellation checks retain their existing scope.

The 128 entries occupy 4,096 bytes on the verified Windows build. They are part
of the caller-owned refinement scratch, charged before allocation and refunded
with the rest of that scratch. This increases the minimum working-memory
requirement for filtered quality-4 normal refinement by 4 KiB. It adds no
runtime package data, decoder state or public setting.

## Output and performance evidence

Baseline, instrumented cache and installed production-library probes generate
identical bytes for all 65,536 candidates from 4,096 deterministic source blocks.
Inputs cover constant colors, narrow ramps, positive-Z normals, arbitrary RGB,
opaque alpha, constant alpha and varying alpha. Each candidate is decoded by
the independent block decoder. The candidate stream SHA-256 is
`a839cb06ce220d30121dbdbf03268c32636eb782df3aa171eb2aa174cd1c2212`.

Both diagnostic image probes produce the same 65,536 encoded image blocks:
SHA-256 `4295d408456088fb14440034e95da0615e32d0c8046b5112d78a527f0f570664`.
The integrated regression suite also pins the first 256 source blocks against
the pre-cache output digest, reusing scratch across inputs. The previous
84 edge/alpha combinations, quality preset checks, allocation failure,
undefined-direction fallback and cancellation-at-every-poll tests still pass.

Complete-cook measurements use ClearCoatTest, the desktop profile, BC7 quality
4, offline mips and two workers. Both executables are installed x64 MSVC Release
builds on the same Ryzen 9 9950X3D host. Each measurement includes process
startup, source import, fallback decisions and package I/O. Pair order alternates;
this task runs no builds, tests or renderer alongside the six measurements.

| Pair | Previous implementation | Score cache |
| --- | ---: | ---: |
| 1 | 27.297 s | 23.954 s |
| 2 | 27.016 s | 23.360 s |
| 3 | 27.047 s | 24.047 s |
| Median | 27.047 s | 23.954 s |

Every package matches the previously verified resource payloads except metadata.
The final output passes the independent Pillow image audit under the unchanged
quality policy. This is a paired comparison within this campaign; absolute
times differ from the earlier campaign and should not be compared across runs
as though host conditions were fixed. The remaining cost is still substantial;
this does not establish acceptable shipping throughput.

## Integration checks

- All 35 standalone Release CTests and four affected Debug suites pass.
- The installed-package candidate consumer passes the full byte comparison.
- `CommonTest` and `EditorRenderingTest` rebuild against the current sibling
  library, with matching standalone/engine source fingerprints.
- All 50 engine cache/import/loader/adapter tests and six native editor tests
  execute and pass without skips in their selected scopes.
- The installed CLI builds the existing four-mip filtered-normal fixture using
  vertex-fetch optimization, Zstandard and meshoptimizer transport. Its decoded
  payload passes the independent image audit, and actual editor recooking agrees
  with the CLI's canonical scene in both execution modes.
- Native tests report no Vulkan validation hazards or CPU/VMA leaks. Exactly
  eight expected error messages are checked against their individual injected
  publication/upload failure tests; other error messages would fail the run.

Engine test and shader sources are unchanged by this follow-up. The native test
runner verifies that shader payloads remain unchanged during its runs. Earlier
full-frame screenshot campaigns were not repeated; the claim here is resource
byte equivalence and the listed native integration checks.

## Evidence identities and remaining work

Source fingerprint:
`c1401d534570a5b98a717aa703cfa8a7259fadeac6587a5fc610837a32c6eba8`.
Installed CLI SHA-256:
`bc78fabeab153efc1ea1f1eed95d0683b5a6570e5ffd48853f9eed17ad503a68`.
The paired baseline CLI SHA-256 is
`335be3504d97e20500467d129a9597e22260d8de897bddcc8d22d5abef1a3c52`.

Scripts, logs, probe source and binaries live in
`build/bc7-score-cache-probe`. The timing and native reports live in
`build/asset-processor/m5-bc7-score-cache-timing/results.json` and
`build/asset-processor/m5-bc7-score-cache-editor/results.json`. The prototype
probes were built against the pre-change private header, preserved with the
prior checkpoint; the production consumer uses the new scratch argument.

The [frozen identity manifest](../build/asset-integration-release/m5-bc7-score-cache-evidence/identities.json)
preserves the current sources, relevant historical header, actual binaries,
packages and reports. The [checkpoint audit](../build/asset-integration-release/m5-bc7-score-cache-evidence/final-audit.json)
checks those identities and the results above. It explicitly leaves shipping
throughput and full-plan completion false. Broader scene/device/filter coverage,
continuous and cross-mip normal quality, remaining atlas acceptance and the
versioned release/runtime distribution gates remain separate outstanding work.
