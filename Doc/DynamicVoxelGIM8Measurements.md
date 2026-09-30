> Historical archive: implementation retired on 2026-09-30. Dynamic Voxel code, shaders, configuration and tools referenced below have been removed. Cone tracing is the supported GI path. See [retirement evaluation](DynamicVoxelGIM8Measurements.md#retirement-evaluation-2026-09-30).

# M8 final AMD measurements

See [final verification and promotion decision](DynamicVoxelGIM8FinalVerification.md) for acceptance, exceptions and scope.

AMD Radeon RX 7900 XT, driver version raw `8388981`, Vulkan API version raw `4211017`; AMD Ryzen 7 9700X (8 cores / 16 threads), Windows build 19045. Optimized MSVC build, RT and validation disabled for timing. Configuration, shader/binary/input identities and commands are retained per cell.

Every matrix cell uses 1920x1080 presentation, a 1024-square G-buffer, 1024 shadows, owner reflectance, radius 2, elapsed temporal history and spatial filtering, 128 rays, compact cache, a 6144 MiB method cap, threaded RHI, dedicated compute and VSync off. Static/light-update scenes use Sponza; motion uses the lifecycle fixture. Sponza camera is `(0.55, -0.15, 0)`; the motion camera is `(0, 0, 2)`. The environment is papermill. Fixed animation steps do not freeze elapsed GI history time.

Three trials each contain 96 warmup + 120 measured frames, a separate delayed-start 64-frame cold capture and paired uninstrumented throughput. Every DDA trial also has an intrusive traversal capture, excluded from timings. Table medians/p95 pool all 360 steady measured frames per cell; trial range covers the three individual GPU medians. Cold initialization reports the median and full range of three application-wall intervals through the first GPU-proven ready frame. Every capture starts a new process/cache; driver shader caches are not purged. Cone has no directional-cache initialization interval.

## Complete matrix

| Case | GPU median / p95 ms | CPU median / p95 ms (uninstrumented) | Trial GPU median range ms | Init wall median [min-max] ms | VMA total / device-local peak GiB | Directional reserve GiB |
| --- | --- | --- | --- | --- | --- | --- |
| 128-comp-lights-cone | 6.477 / 6.872 | 6.430 / 7.413 | 6.464-6.499 | n/a | 1.125 / 1.000 | 0.000 |
| 128-comp-lights-dynamic_voxel | 10.234 / 10.557 | 9.721 / 10.251 | 10.194-10.260 | 653.31 [643.11-662.48] | 6.942 / 6.817 | 6.000 |
| 128-comp-motion-cone | 2.197 / 2.371 | 3.696 / 4.013 | 2.174-2.210 | n/a | 0.625 / 0.500 | 0.000 |
| 128-comp-motion-dynamic_voxel | 66.487 / 76.171 | 64.602 / 76.985 | 66.282-66.616 | 193.82 [191.82-199.44] | 6.692 / 6.567 | 6.000 |
| 128-comp-static-cone | 5.798 / 6.204 | 6.015 / 6.971 | 5.794-5.803 | n/a | 1.125 / 1.000 | 0.000 |
| 128-comp-static-dynamic_voxel | 9.142 / 9.381 | 8.659 / 9.591 | 9.125-9.178 | 609.83 [597.54-618.64] | 6.942 / 6.817 | 6.000 |
| 128-geom-lights-cone | 6.476 / 6.917 | 6.438 / 7.129 | 6.413-6.526 | n/a | 1.125 / 1.000 | 0.000 |
| 128-geom-lights-dynamic_voxel | 10.207 / 10.434 | 9.716 / 10.631 | 10.200-10.216 | 656.36 [635.97-663.29] | 6.942 / 6.817 | 6.000 |
| 128-geom-motion-cone | 2.211 / 2.467 | 3.955 / 4.343 | 2.197-2.221 | n/a | 0.625 / 0.500 | 0.000 |
| 128-geom-motion-dynamic_voxel | 66.711 / 76.265 | 65.367 / 78.726 | 66.490-66.796 | 187.15 [185.23-194.36] | 6.692 / 6.567 | 6.000 |
| 128-geom-static-cone | 5.844 / 6.275 | 6.058 / 6.911 | 5.806-5.942 | n/a | 1.125 / 1.000 | 0.000 |
| 128-geom-static-dynamic_voxel | 9.121 / 9.260 | 8.675 / 9.574 | 9.116-9.128 | 601.87 [600.56-609.36] | 6.942 / 6.817 | 6.000 |
| 64-comp-lights-cone | 3.629 / 4.009 | 3.638 / 4.402 | 3.623-3.637 | n/a | 0.875 / 0.750 | 0.000 |
| 64-comp-lights-dynamic_voxel | 3.896 / 4.192 | 5.946 / 6.293 | 3.878-3.961 | 165.15 [164.95-166.26] | 2.625 / 2.500 | 1.689 |
| 64-comp-motion-cone | 1.693 / 1.919 | 3.696 / 4.029 | 1.514-1.763 | n/a | 0.625 / 0.500 | 0.000 |
| 64-comp-motion-dynamic_voxel | 12.391 / 13.413 | 12.103 / 13.277 | 12.312-12.462 | 52.46 [51.42-55.19] | 2.375 / 2.250 | 1.689 |
| 64-comp-static-cone | 3.136 / 3.548 | 3.363 / 4.197 | 3.125-3.159 | n/a | 0.875 / 0.750 | 0.000 |
| 64-comp-static-dynamic_voxel | 3.357 / 3.611 | 3.920 / 4.167 | 3.343-3.378 | 175.68 [165.43-909.92] | 2.625 / 2.500 | 1.689 |
| 64-geom-lights-cone | 3.678 / 4.114 | 3.657 / 4.304 | 3.659-3.721 | n/a | 0.875 / 0.750 | 0.000 |
| 64-geom-lights-dynamic_voxel | 3.885 / 4.127 | 5.814 / 6.261 | 3.858-3.935 | 164.38 [162.42-186.29] | 2.625 / 2.500 | 1.689 |
| 64-geom-motion-cone | 1.815 / 2.098 | 3.907 / 4.304 | 1.791-1.826 | n/a | 0.625 / 0.500 | 0.000 |
| 64-geom-motion-dynamic_voxel | 12.618 / 13.815 | 11.923 / 13.276 | 12.560-12.658 | 54.10 [53.23-57.23] | 2.375 / 2.250 | 1.689 |
| 64-geom-static-cone | 3.177 / 3.613 | 3.359 / 4.213 | 3.154-3.191 | n/a | 0.875 / 0.750 | 0.000 |
| 64-geom-static-dynamic_voxel | 3.336 / 3.579 | 3.886 / 4.111 | 3.323-3.379 | 160.12 [158.79-212.25] | 2.625 / 2.500 | 1.689 |

VMA peaks are maxima over cold, profile and uninstrumented runs. They include retained allocator blocks and unrelated renderer allocations; they exclude private driver/swapchain allocations and do not establish residency. The logical directional reserve is not the complete process allocation nor the total transition budget. Dedicated transfer-only timestamps remain unsupported/excluded; all accepted frame GPU samples are available.

## Dominant passes

The following are the three largest pass costs per DDA cell. Repeated instances of a pass are summed within each application frame, then summarized over measured frames. Different passes may overlap: these values must not be added into a frame total. Full pass median/p95 and unavailable coverage are in [summary.json](../build/dynamic-voxel-m8-final/matrix/summary.json).

| Cell | Pass | Median / p95 ms per frame |
| --- | --- | --- |
| 128-comp-lights-dynamic_voxel | frame_rdg / GIStaticPad / compute | 1.954 / 2.001 |
| 128-comp-lights-dynamic_voxel | frame_rdg / GIStaticGather / compute | 1.110 / 1.134 |
| 128-comp-lights-dynamic_voxel | frame_rdg / GIDynamicTemporal / compute | 1.042 / 1.076 |
| 128-comp-motion-dynamic_voxel | frame_rdg / GIDynamicGather / compute | 25.310 / 33.009 |
| 128-comp-motion-dynamic_voxel | frame_rdg / GIStaticSenderEnvironment / compute | 20.938 / 21.971 |
| 128-comp-motion-dynamic_voxel | frame_rdg / GIDynamicSenderEnvironment / compute | 13.941 / 19.078 |
| 128-comp-static-dynamic_voxel | frame_rdg / GIStaticPad / compute | 1.956 / 2.003 |
| 128-comp-static-dynamic_voxel | frame_rdg / GIStaticGather / compute | 1.102 / 1.123 |
| 128-comp-static-dynamic_voxel | frame_rdg / GIDynamicTemporal / compute | 1.024 / 1.060 |
| 128-geom-lights-dynamic_voxel | frame_rdg / GIStaticPad / compute | 1.960 / 2.006 |
| 128-geom-lights-dynamic_voxel | frame_rdg / GIStaticGather / compute | 1.111 / 1.134 |
| 128-geom-lights-dynamic_voxel | frame_rdg / GIDynamicTemporal / compute | 1.037 / 1.078 |
| 128-geom-motion-dynamic_voxel | frame_rdg / GIDynamicGather / compute | 25.503 / 33.104 |
| 128-geom-motion-dynamic_voxel | frame_rdg / GIStaticSenderEnvironment / compute | 20.973 / 22.113 |
| 128-geom-motion-dynamic_voxel | frame_rdg / GIDynamicSenderEnvironment / compute | 14.002 / 19.129 |
| 128-geom-static-dynamic_voxel | frame_rdg / GIStaticPad / compute | 1.967 / 2.010 |
| 128-geom-static-dynamic_voxel | frame_rdg / GIStaticGather / compute | 1.104 / 1.123 |
| 128-geom-static-dynamic_voxel | frame_rdg / GIDynamicTemporal / compute | 1.024 / 1.063 |
| 64-comp-lights-dynamic_voxel | frame_rdg / LightMarkers / graphics | 0.526 / 0.542 |
| 64-comp-lights-dynamic_voxel | frame_rdg / SceneLighting / graphics | 0.526 / 0.543 |
| 64-comp-lights-dynamic_voxel | frame_rdg / GIStaticGather / compute | 0.514 / 0.539 |
| 64-comp-motion-dynamic_voxel | frame_rdg / GIDynamicSenderEnvironment / compute | 5.917 / 6.211 |
| 64-comp-motion-dynamic_voxel | frame_rdg / GIStaticSenderEnvironment / compute | 3.354 / 3.667 |
| 64-comp-motion-dynamic_voxel | frame_rdg / GIDynamicGather / compute | 3.184 / 4.572 |
| 64-comp-static-dynamic_voxel | frame_rdg / LightMarkers / graphics | 0.559 / 0.573 |
| 64-comp-static-dynamic_voxel | frame_rdg / SceneLighting / graphics | 0.558 / 0.572 |
| 64-comp-static-dynamic_voxel | frame_rdg / GIStaticGather / compute | 0.508 / 0.525 |
| 64-geom-lights-dynamic_voxel | frame_rdg / LightMarkers / graphics | 0.526 / 0.543 |
| 64-geom-lights-dynamic_voxel | frame_rdg / SceneLighting / graphics | 0.526 / 0.542 |
| 64-geom-lights-dynamic_voxel | frame_rdg / GIStaticGather / compute | 0.512 / 0.534 |
| 64-geom-motion-dynamic_voxel | frame_rdg / GIDynamicSenderEnvironment / compute | 5.910 / 6.227 |
| 64-geom-motion-dynamic_voxel | frame_rdg / GIStaticSenderEnvironment / compute | 3.343 / 3.665 |
| 64-geom-motion-dynamic_voxel | frame_rdg / GIDynamicGather / compute | 3.184 / 4.593 |
| 64-geom-static-dynamic_voxel | frame_rdg / LightMarkers / graphics | 0.559 / 0.574 |
| 64-geom-static-dynamic_voxel | frame_rdg / SceneLighting / graphics | 0.559 / 0.573 |
| 64-geom-static-dynamic_voxel | frame_rdg / GIStaticGather / compute | 0.506 / 0.525 |

## Traversal and cold readiness

Queries are deterministic samples of up to 128 receivers per class, six faces and 128 rays. Static receivers contribute static-only queries and (when present) dynamic-only queries; dynamic receivers contribute full-scene queries. Closest-hit and occlusion are evaluated separately. Counts below are representative final-frame diagnostics, not total production-frame rays. Every trial is retained in the summary JSON.

| DDA cell | Occupied static / dynamic | Selected static / dynamic | Sample queries | Closest / occlusion cell visits | Unknown / fallback bits | First-ready frame (three trials) |
| --- | --- | --- | --- | --- | --- | --- |
| 128-comp-lights-dynamic_voxel | 118827 / 0 | 18849 / 0 | 98304 | 532628 / 532628 | 0 / 0 | 30, 30, 30 |
| 128-comp-motion-dynamic_voxel | 6428 / 3773 | 3234 / 26163 | 294912 | 17588497 / 17588497 | 0 / 0 | 2, 2, 2 |
| 128-comp-static-dynamic_voxel | 118827 / 0 | 18849 / 0 | 98304 | 532628 / 532628 | 0 / 0 | 30, 30, 30 |
| 128-geom-lights-dynamic_voxel | 118827 / 0 | 18849 / 0 | 98304 | 532628 / 532628 | 0 / 0 | 30, 30, 30 |
| 128-geom-motion-dynamic_voxel | 6428 / 3773 | 3234 / 26163 | 294912 | 17588497 / 17588497 | 0 / 0 | 2, 2, 2 |
| 128-geom-static-dynamic_voxel | 118827 / 0 | 18849 / 0 | 98304 | 532628 / 532628 | 0 / 0 | 30, 30, 30 |
| 64-comp-lights-dynamic_voxel | 22997 / 0 | 7510 / 0 | 98304 | 351378 / 351378 | 0 / 0 | 6, 6, 6 |
| 64-comp-motion-dynamic_voxel | 1626 / 950 | 884 / 7938 | 294912 | 8817521 / 8817521 | 0 / 0 | 1, 1, 1 |
| 64-comp-static-dynamic_voxel | 22997 / 0 | 7510 / 0 | 98304 | 351378 / 351378 | 0 / 0 | 6, 6, 6 |
| 64-geom-lights-dynamic_voxel | 22997 / 0 | 7510 / 0 | 98304 | 351378 / 351378 | 0 / 0 | 6, 6, 6 |
| 64-geom-motion-dynamic_voxel | 1626 / 950 | 884 / 7938 | 294912 | 8817521 / 8817521 | 0 / 0 | 1, 1, 1 |
| 64-geom-static-dynamic_voxel | 22997 / 0 | 7510 / 0 | 98304 | 351378 / 351378 | 0 / 0 | 6, 6, 6 |

## Supplemental presets, submission modes and fallback

These use the same final application and compute producer at 1080p. All are 64 cubed except the explicit decoded-128 reference, which uses a 12288 MiB cap. Decoded-64 uses 6144 MiB. Lower-ray captures retain a 64-thread workgroup and skip cold runs. Their speed does not override failed quality gates. Inline changes only RHI threading; shared changes only async compute. The 128-ray reference rows are in the complete matrix above. Budget/auto cases report their actual cone/PBR fallback, not DDA performance.

| Case | GPU median / p95 ms | CPU median / p95 ms (uninstrumented) | Trial GPU median range ms | Init wall median [min-max] ms | VMA total / device-local peak GiB | Directional reserve GiB |
| --- | --- | --- | --- | --- | --- | --- |
| auto-cone | 3.148 / 3.595 | 3.389 / 4.243 | 3.137-3.170 | n/a | 0.875 / 0.750 | 0.000 |
| budget-cone | 3.160 / 3.617 | 3.409 / 4.217 | 3.139-3.209 | n/a | 0.875 / 0.750 | 0.000 |
| budget-pbr | 0.306 / 0.393 | 0.978 / 1.110 | 0.304-0.317 | n/a | 0.875 / 0.750 | 0.000 |
| decoded-128-static | 10.571 / 10.820 | 10.134 / 10.746 | 10.562-10.581 | 711.05 [705.44-713.41] | 12.942 / 12.817 | 12.000 |
| decoded-64-static | 4.185 / 4.402 | 3.932 / 4.190 | 4.174-4.198 | 209.63 [208.01-225.73] | 6.936 / 6.811 | 6.000 |
| inline-motion | 12.300 / 13.394 | 11.525 / 12.947 | 12.202-12.326 | 92.07 [87.21-94.37] | 2.375 / 2.250 | 1.689 |
| inline-static | 3.237 / 3.457 | 3.313 / 4.744 | 3.233-3.241 | 158.51 [135.29-158.66] | 2.625 / 2.500 | 1.689 |
| rays-32-motion | 5.849 / 6.274 | 7.474 / 12.680 | 5.833-5.866 | n/a | 1.125 / 1.000 | 0.564 |
| rays-32-static | 3.069 / 3.411 | 3.977 / 4.281 | 3.034-3.151 | n/a | 1.594 / 1.500 | 0.564 |
| rays-64-motion | 7.960 / 8.789 | 12.362 / 13.313 | 7.929-8.023 | n/a | 1.500 / 1.375 | 0.939 |
| rays-64-static | 3.105 / 3.353 | 3.938 / 4.509 | 3.099-3.117 | n/a | 1.875 / 1.750 | 0.939 |
| shared-motion | 11.373 / 12.524 | 11.719 / 13.119 | 11.367-11.412 | 50.96 [50.35-51.25] | 2.375 / 2.250 | 1.689 |
| shared-static | 2.571 / 2.860 | 3.544 / 3.821 | 2.569-2.574 | 106.67 [105.14-107.71] | 2.625 / 2.500 | 1.689 |

## Retirement evaluation (2026-09-30)

Decision: remove the Dynamic Voxel implementation at the user's request after recurring hangs. No additional hang diagnosis or Dynamic Voxel runs were performed for this change. The matrix above is historical controlled evidence, not a new benchmark of the refactor.

At 128 cubed with compute voxelization, static GPU time was 5.798 ms for Cone versus 9.142 ms for Dynamic Voxel; animated lights were 6.477 versus 10.234 ms, and the moving-geometry fixture was 2.197 versus 66.487 ms. Static device-local allocator peaks were 1.000 versus 6.817 GiB. At 64 cubed the static timing gap was smaller (3.136 versus 3.357 ms), but allocator peaks were still 0.750 versus 2.500 GiB. These results support retaining Cone on the measured AMD system; they do not predict every scene or GPU.

Cone stores a radiance mip chain and sky irradiance, rebuilding them only when geometry, lighting or relevant settings change. The directional implementation added separate static/dynamic voxel sets, per-receiver hit caches, six-face lighting, neighborhood lists, history/filter/padding volumes, and extra G-buffer attachments. Cached visibility did not remove gathering, filtering, padding or motion-driven environment traversal. The largest recorded costs identify those passes directly. Its better directional representation was not enough to justify the observed cost and reliability problems.

| Feature | Decision and reason |
| --- | --- |
| Mesh shadow visibility at an owner-triangle surface point | Ported to Cone analytic radiance injection. Reuses the direct-light shadow atlas and existing triangle records, avoiding a coarse voxel-center visibility query for valid surfaces. It adds no persistent directional cache. Retains voxel visibility for missing/degenerate surface data. |
| Analytic, environment and emissive GI controls | Ported to Cone configuration and runtime UI; live edits invalidate only the affected irradiance/radiance work. Turning a contribution off skips its shader work. Defaults preserve all contributions. |
| Averaged diffuse reflectance, material/alpha correctness, exact environment visibility, async scheduling | Already shared with Cone and retained, including their regression coverage and checked resource sizes. |
| Normal-aware temporal/spatial denoising | Do not copy the 3D history stack. A future depth/normal-aware screen-space temporal resolve could reduce Cone shimmer at lower memory cost, but requires motion vectors, disocclusion rejection and image validation. Not implemented here. |
| Six-face directional irradiance reconstruction | Not compatible with Cone's isotropic radiance texture as a local change. Directional radiance mips could reduce opposite-face leakage, but would multiply storage/bandwidth; defer until an isolated benchmark demonstrates a worthwhile quality gain. |
| Receiver caches, class-specific producers, query providers, automatic multi-GiB cap and fallback machinery | Removed with the implementation. Preserve ordinary scene transforms/mobility and merged voxel rebuilding so moving geometry still works in Cone. |

The runtime method dropdown is gone; voxel resolution remains configurable at 64/128/256. GPU memory reporting and default automatic application of UI edits remain. Obsolete config entries were removed from the example and local config without changing scene/light/camera values. Historical documents are retained only as research evidence.
