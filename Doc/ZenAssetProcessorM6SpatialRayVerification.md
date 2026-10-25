# M6 cooked spatial alpha ray verification

24 October 2026. Cooked spatial pages preserve the tested alpha decisions through
the production software and hardware ray-query paths. All 393,216 paired rays
match standalone textures exactly, including selected instance/material and hit
distance. This closes the bounded alpha-query gap from the
[spatial GPU checkpoint](ZenAssetProcessorM6SpatialGpuVerification.md); full GI
frames, broader sampling profiles, other devices and workload benefit remain open.

## Inputs and execution

The original [fixture generator](../ZenEditor/Tests/Fixtures/generate_spatial_alpha_scene.py)
creates four embedded RGBA8 checker images, alternating 16x8 and 8x16 dimensions.
Six front panels use metallic/roughness or specular/glossiness materials, MASK or
BLEND alpha, vertex alpha, factor alpha, UV0 or UV1 and authored UV transforms.
Six opaque rear panels reveal whether rejected front intersections continue
traversal. A material variant swaps paired front materials and must change actual
coverage, not merely material IDs.

The [integration tests](../../ZenAssetProcessor/tests/integration/zenengine/Editor/SpatialRayRenderingTests.inl) cook the
self-contained glTF twice through the source service: standalone textures and
spatial pages. They wait for publication, inspect the recorded settings, delete
the copied source, then load each package through the editor/runtime path.
Recooking is unavailable. All four spatial texture records must have nonempty
regions and share fewer than four image records. Standalone records have no
regions. Runtime materials include the engine's additional fallback material.

Each invocation sends 24,576 rays: 32x32 samples per panel for each of four modes.
The modes exercise closest-hit continuation to an opaque rear panel, a shorter
closest-hit interval that misses through holes, reverse-facing queries from
between the panels, and any-hit visibility. Any-hit comparisons intentionally
check visibility only; the API does not promise a selected intersection.

Both default and alternate materials run in compute and fragment shaders, using
the production `SoftwareQuery` or `HardwareQuery` implementation and production
alpha evaluation. The fragment wrapper enables the forward material UV-binding
macros with unrelated raster UVs to verify that hit UVs remain authoritative.
This is a query pass in that shader context, not a full forward-rendered GI frame.
An opaque material update through `RenderScene::UpdateMaterial` must close every
hole and select the front panel for closest-hit queries. Every non-control group
must contain substantial accepted and rejected front coverage.

## Results

Windows Release, NVIDIA GeForce RTX 5080, raw Vulkan driver version 2588573696,
reported subtexel precision 8 bits. Software cases explicitly disable hardware
ray queries; hardware cases require usable hardware queries. No cases skipped.
SPIR-V inspection confirms `RayQueryKHR` only in the hardware wrappers.

| Check | Result |
| --- | --- |
| Software/hardware x inline/threaded ray cases | 4/4 passed |
| Spatial/standalone result pairs | 16 captures, 393,216 paired rays, byte-identical |
| Opaque negative controls | 16 captures, 393,216 queries, all front coverage |
| Total recorded query invocations | 1,179,648 across 48 result captures |
| Compute/fragment comparison | Hit/material/instance exact; distance within the predeclared 1e-5 tolerance |
| Additional software/hardware capture comparison | No decision or distance differences in this fixture |
| Combined cooked array/spatial regression run | 10/10 passed, 0 skipped, 7.287 seconds |
| Vulkan validation and teardown | No reported validation errors; all ten VMA teardown reports zero bytes |
| Tracked CPU allocations | No leaks reported |

The six existing raster cases in the combined run retain their 24 spatial and 64
array frame comparisons. These checks do not measure GPU performance. No
production renderer or processor change was needed for this ray checkpoint.

## Reproduction and evidence

Build target `EditorAssetRenderingTest` in the existing Release integration
configuration with the sibling processor source override. Run from the engine
root, with a pre-created capture directory:

```powershell
$env:ZEN_EDITOR_SPATIAL_RAY_DIR = 'E:/Dev/ZenEngine/build/asset-integration-release/m6-spatial-ray-captures-final'
& build/asset-integration-release/bin/EditorAssetRenderingTest.exe `
  '--gtest_filter=*CookedSpatial*:*CookedArray*' `
  '--gtest_output=xml:build/asset-integration-release/m6-spatial-ray-final.xml'
Remove-Item Env:ZEN_EDITOR_SPATIAL_RAY_DIR
```

Use a new directory and output filenames for another checkpoint. Each `.bin`
capture contains 24,576 little-endian float4 results in panel, mode, y, x order:
hit flag, distance, material ID plus one, instance ID plus one. Misses are zero;
any-hit results store only the hit flag. Eight cooked packages accompany the 48
captures. The saved analyzer independently checks finite values, per-group
coverage, opaque controls and byte-identical spatial/standalone output.

Evidence under `build/asset-integration-release`:

- `m6-spatial-ray-build*.log`: successful builds.
- `m6-spatial-ray-tests.{log,xml}`: first run, rejected by an incorrect fixture
  expectation of seven runtime materials. Correcting it to include the fallback
  material enabled traversal; no quality gate or production behavior changed.
- `m6-spatial-ray-tests-2.{log,xml}`: initial four-case pass.
- `m6-spatial-ray-final.{log,xml}` and `m6-spatial-ray-results.json`: final combined
  run and independent analysis after adding fragment UV context and captures.
- `m6-spatial-ray-format.log`: scoped format verification.
- `m6-spatial-ray-evidence/identities.json`: hash-verified source, baseline, binary,
  SPIR-V, package, capture and report snapshots. The prior GPU checkpoint remains
  preserved separately.

The accepted spatial profile remains explicit linear-clamp, one-level,
power-of-two RGBA8. The tests do not establish general mipmapping, anisotropy,
block-compressed spatial filtering, full GI accumulation or another device's
sampling behavior. The earlier allocation increase and lossy-compression visual
acceptance gaps remain unchanged. M6 and the overall release remain incomplete.
