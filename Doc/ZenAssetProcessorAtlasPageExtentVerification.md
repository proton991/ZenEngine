# Atlas page extent publication checkpoint

On 2026-10-25, the cooked array/spatial rendering campaign passes on both the
AMD Radeon(TM) Graphics and NVIDIA GeForce RTX 5080. The AMD G-buffer pipeline
creation fault from the [sampler qualification checkpoint](ZenAssetProcessorSpatialQualificationVerification.md)
is avoided by publishing the physical atlas page extent with each runtime
material binding. The shared material shader no longer queries bindless image
dimensions. Atlas/reference comparisons retain exact equality; no tolerance was
relaxed. This closes that specific AMD acceptance gap, not the complete M6 gate.

## Evidence and implementation

The isolated controls in `build/amd-gbuffer-probe` reproduce an access violation
inside `amdvlk64.dll`, offset `0x24ea5a4`, while creating `GBufferSP`. Unrolling
the vertex loop, optimizing SPIR-V, using a fixed sampler, and querying the
texture without a sampler still reproduce the fault. Replacing texture-size
queries with constants allows the ordinary-texture test to pass. Constant
fragment/no-texture controls avoid the fault but fail their material assertions,
as expected. These controls implicate the query in the tested shader; they do
not establish the driver's internal defect. No diagnostic shader is shipped.

`sg::PublishTextureTransform` now writes two five-bit page-dimension exponents
to the previously unused `TextureTransformData::row1.w`. Their integer code is
represented exactly as a float. The existing spatial profile already requires
power-of-two pages, so the shader reconstructs their exact dimensions with
integer shifts. Both coordinate normalization and gradient scaling use those
dimensions. Sampler slots remain in `row0.w`; authored affine coefficients in
`row0.xyz`/`row1.xyz` remain unchanged. Source asset transforms are not modified.

Ordinary textures, private pixel overrides and absent/flat normal bindings
publish zero page metadata. Invalid spatial dimensions retain an invalid code;
existing texture upload validation rejects their payloads. Live material updates
must match the referenced texture's page code as well as its region and sampler.
Failed edits leave the published material unchanged.

The GPU transform remains 48 bytes, the feature binding 64 bytes, and the full
material 1,632 bytes. Package records and the standalone Processor/Reader API
are unchanged. This runtime encoding does not add NPOT, mipmapped, compressed
spatial, or anisotropic spatial support.

## Executed verification

Windows Release; Vulkan validation with synchronization validation requested;
explicit GPU selection; implicit layers disabled. Native executables use the
existing copied `7zFM.exe` test alias to avoid the RTSS hook. Both GPUs use inline
and threaded execution. All listed tests execute without skips.

| Check | Result |
| --- | --- |
| Adapter, material publication and animation compatibility | 31 passed |
| Shader reflection, including material layout | 23 passed |
| Native texture sampling and qualification failure/retry | 14 per GPU; 28 passed |
| Cooked raster, software/hardware GI and ray comparisons | 14 per GPU; 28 passed |
| Qualification failure preserves active editor scene, followed by successful retry | 2 per GPU; 4 passed |
| Original AMD ordinary-texture crash reproducer, production build | 1 passed |
| NVIDIA old-query material control against current build | 4 GI tests passed; all 24 captures equal |

For each GPU, the cooked campaign contains 88 exact raster pairs (64 array and
24 spatial), 393,216 exact paired ray results, eight exact spatial/reference GI
pairs and eight exact identical-package GI repeats. Native spatial float-channel
and alpha-decision difference counts are zero. Native normal sampling also
retains its existing acceptance checks. Expected allocation-failure diagnostics
are asserted by their tests; the campaigns report no unexpected validation,
synchronization or memory-leak errors.

The original AMD crash occurs with driver SHA-256
`81081de01e02beaa59bd96c547508488870db239cc8cc14e981cc711e6af1772`;
the successful production campaign uses that same driver. The new G-buffer
SPIR-V validates and contains no `OpImageQuerySize`. All five changed C++/GLSL
files pass the repository formatting configuration.

## Historical comparison and reproducibility

All 176 NVIDIA raster captures match the previous qualification checkpoint.
Its 24 historical GI captures differ in 37 RGB channels for the material scene
and 58 for the alpha scene, with a maximum difference of one byte. This is not
an atlas/reference discrepancy: standalone, spatial and repeat captures move
together. Source comparison identifies separate GI changes in the shared
checkout since that checkpoint.

A diagnostic executable built from the current sources loads copied current
shaders with only `Common/material.glsl` restored to its pre-change query
implementation. All 24 resulting GI captures equal the current production
captures exactly. This isolates the material-query change from the historical
GI differences while preserving the other worktree edits. Production source
and shader outputs are not replaced by the control.

Working evidence is under `build/atlas-page-extent`: build/format logs, test JSON,
capture pairs, the query control, before-edit source snapshots, source comparison,
and `audit.json`. A frozen copy with source/binary identities is under
`build/asset-integration-release/m6-page-extent-evidence`.
The editor verification script initially expected externally logged failure
messages that its test captures internally; it was corrected and resumed by
verifying the completed AMD result identities, without rerunning or overwriting
those results.

The standalone processor fingerprint remains
`6d13d0abb5d7624e8be26a763af7838e7021b38014db71ea6fa04e570c33ee2e`,
recomputed from its actual fingerprint inputs. The user's `Data/engine.cfg`
remains unchanged. No processor code or package schema changed in this fix.

## Remaining plan scope

This establishes bounded cooked raster/GI/ray parity on the two tested devices.
M6 still requires its broader sampling/quality and measured workload-benefit
gates; the single-level spatial profile remains optional. The M4/M5 quality and
performance gates and M8 release/package gates remain open. The overall
implementation plan is not complete.
