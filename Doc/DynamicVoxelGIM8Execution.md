# M8 execution and acceptance record

Work started 2026-09-29 against the existing uncommitted M8.0/M8.1 worktree.
The objective covers the provider boundary, C++ conventions, and every remaining
M8.2–M8.9 deliverable in `DynamicVoxelGIImplementationPlan.md`. Hardware RT remains
the separately deferred H0–H2 work.

The user accepts the isolated AMD planar constant-irradiance difference as
nonblocking: 3.1394338608 versus pi, absolute error 0.0021588802 against the unchanged
0.002 test bound. Keep that test and the image-quality limits unchanged; retain its
failure in raw results and identify it explicitly in final acceptance.

## Execution state

- Provider refactor implemented: stage shader selection, mesh-light bindings,
  compact-cache capability, and static cache keys belong to the provider.
- M8.2 implemented: bounded asynchronous GPU status/work readback with frame identity,
  cache epochs, completed-readback gating, and explicit fallback exports.
- M8.3 implemented: serial benchmark runner, input identities, completed-readiness gates,
  paired uninstrumented throughput, traversal summaries, config restoration and failure tests.
- M8.4 matched AMD baselines complete under `baselines-v2/`.
- M8.5 complete: 32/128-thread gather, tiled filter and active scratch were built,
  validated, measured and rejected. Their source/binaries/results remain under `experiments/`.
- M8.6 complete: 128 rays retained; 64/32 rays fail 18/42 locked image profiles.
  Sixteen compact/decoded profiles are byte-identical.
- M8.7 complete on AMD: 24 cells at 1080p, three trials per cell, plus 13 supplemental
  layout/preset/submission/fallback comparisons. All 210 profile IDs are unique.
- M8.8 complete with the explicit planar exception: 56 quality profiles, four stationary
  summaries, 332 V0/V1 checks and all required native/runtime coverage. Profiling-on/off
  output is byte-identical in both checked submission modes.
- M8.9 complete: keep `auto` on cone and DDA explicit; no universal queue/default-budget
  change. The NVIDIA reproduction/target and other platform gates remain open.

The [final report](DynamicVoxelGIM8FinalVerification.md), [measurement tables](DynamicVoxelGIM8Measurements.md)
and [acceptance manifest](../build/dynamic-voxel-m8-final/acceptance.json) are authoritative
for current acceptance. `Data/engine.cfg` and locked quality limits are unchanged.

The initial captures exposed timestamp-pool starvation: completed startup command buffers
retained the process-wide pool budget. Pools now belong to the device and are recycled only
after retirement. Capacity is bounded at 256 pools (512 scopes each). Thirteen native timing
tests pass, including allocation failure, capacity, reuse, frame boundaries and completion.
The 208-frame inline moving-fixture diagnostic passes synchronization validation with no
dropped frame/GI records. Failed captures under `diagnostics/dda-ready` and `baselines/` are
retained as rejected evidence; only the fresh matched `baselines-v2/` set is accepted.

Available hardware is AMD Radeon RX 7900 XT. Historical NVIDIA artifacts under
`build/dynamic-voxel-m8-current-20260925/` are absent. Matched NVIDIA regression
reproduction remains a named hardware/artifact gap; AMD evidence must stay separate.
The plan permits a documented non-promotion decision with that gap retained.

The initial review artifacts are `build/review-dynamic-voxel-*.json` and `.log`.
This work retains fresh builds, test results, manifests, raw captures, and experiment
decisions under `build/dynamic-voxel-m8-final/`. Final performance and correctness must
refer to the same frozen executable and shaders; intermediate results are not final acceptance.
