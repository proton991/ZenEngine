#ifndef ZEN_DISTANCE_FIELD_SCENE_H
#define ZEN_DISTANCE_FIELD_SCENE_H

// Native bounded field construction. These values are shared by the owner and GLSL.
// The tracing uniform and persistent resource ABI are in distance_field_common.glsl.
#define ZEN_FIELD_BUILD_CROP_VOLUME 1u
#define ZEN_FIELD_BUILD_PARTIAL     2u

#define ZEN_FIELD_MAX_SIDE 64u
// Regions of a published field that committed edits have made stale (DistanceFieldStaleUniform).
#define ZEN_FIELD_STALE_BOXES 8u
// Local detail volumes around point and spot lights, each a slot of one pool (DistanceFieldLocalsUniform).
#define ZEN_FIELD_LOCAL_VOLUMES                   32u
#define ZEN_FIELD_MAX_TRIANGLE_BATCH              65536u
#define ZEN_FIELD_REGION_SIDE                     4u
#define ZEN_FIELD_SCAN_GROUP                      256u
#define ZEN_FIELD_MAX_SCAN_BLOCKS                 1024u
#define ZEN_FIELD_MAX_CELL_REFS                   8192u
#define ZEN_FIELD_REFERENCE_TILE                  256u
#define ZEN_FIELD_COUNTER_WORDS                   19u
#define ZEN_FIELD_COVERAGE_MAX_DEPTH              12u
#define ZEN_FIELD_COVERAGE_STACK_WORDS            (3u * ZEN_FIELD_COVERAGE_MAX_DEPTH + 1u)
#define ZEN_FIELD_COVERAGE_STATE_WORDS            (4u + ZEN_FIELD_COVERAGE_STACK_WORDS)
#define ZEN_FIELD_COVERAGE_NODES_PER_DISPATCH     32u
#define ZEN_FIELD_COVERAGE_MAX_NODES              65536u
#define ZEN_FIELD_COVERAGE_TRIANGLES_PER_DISPATCH 64u
// High bit of a patch job's depth, and of its resumable region cursor.
#define ZEN_FIELD_PATCH_UNAVAILABLE 0x80000000u

#define ZEN_FIELD_FAILURE_JOBS            1u
#define ZEN_FIELD_FAILURE_REFERENCES      2u
#define ZEN_FIELD_FAILURE_ATLAS           4u
#define ZEN_FIELD_FAILURE_CELL_REFS       8u
#define ZEN_FIELD_FAILURE_GEOMETRY        16u
#define ZEN_FIELD_FAILURE_COVERAGE        32u
#define ZEN_FIELD_FAILURE_SETTINGS        64u
#define ZEN_FIELD_FAILURE_COVERAGE_BUDGET 128u

// Allocation, failure and first-failure diagnostics persist across triangle batches.
#define ZEN_FIELD_COUNTER_ALLOCATED        0u
#define ZEN_FIELD_COUNTER_FAILURE          1u
#define ZEN_FIELD_COUNTER_JOBS             2u
#define ZEN_FIELD_COUNTER_REFERENCES       3u
#define ZEN_FIELD_COUNTER_ACTIVE           4u
#define ZEN_FIELD_COUNTER_FAILURE_SITE     5u
#define ZEN_FIELD_COUNTER_FAILURE_TRIANGLE 6u
#define ZEN_FIELD_COUNTER_FAILURE_CELL     7u
#define ZEN_FIELD_COUNTER_JOB_DISPATCH     8u
#define ZEN_FIELD_COUNTER_COVERAGE_COUNT   9u
#define ZEN_FIELD_COUNTER_COVERAGE_PENDING 10u
#define ZEN_FIELD_COUNTER_CELL_PEAK        11u // Largest per-cell reference count of the batch.
#define ZEN_FIELD_COUNTER_EMIT_DISPATCH    12u
#define ZEN_FIELD_COUNTER_FAILURE_SAMPLE   15u
#define ZEN_FIELD_COUNTER_RETAINED         16u
#define ZEN_FIELD_COUNTER_FREE             17u
#define ZEN_FIELD_COUNTER_UNAVAILABLE      18u

#define ZEN_FIELD_GEOMETRY_NONFINITE_INPUT    1u
#define ZEN_FIELD_GEOMETRY_OUTSIDE_DOMAIN     2u
#define ZEN_FIELD_GEOMETRY_CLOSEST_POINT      3u
#define ZEN_FIELD_GEOMETRY_NONFINITE_DISTANCE 4u
#define ZEN_FIELD_GEOMETRY_TRIANGLE_RANGE     5u
#define ZEN_FIELD_COVERAGE_DEPTH_LIMIT        6u
#define ZEN_FIELD_COVERAGE_WORK_LIMIT         7u

#endif // ZEN_DISTANCE_FIELD_SCENE_H
