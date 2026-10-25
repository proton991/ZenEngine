#ifndef ZEN_BRICK_SCENE_H
#define ZEN_BRICK_SCENE_H

// Sparse brick scene shared by BrickScene.cpp and the brick shaders (brick_common.glsl).
// Each cell of the cubic grid is empty, solid or owns a brick of 8x8x8 occupancy bits.
#define ZEN_BRICK_SIZE  8u
#define ZEN_BRICK_WORDS 16u
// A brick's 2x2x2 blocks of this many voxels per side: bit (z * 4 + y * 2 + x) of its mask is set
// when the block holds an occupied voxel, so rays skip empty blocks inside a brick.
#define ZEN_BRICK_BLOCK 4u
// Cell encoding: an empty cell sets the high bit and keeps its Chebyshev distance, in cells, to the
// nearest non-empty cell in the low bits. A brick cell holds its brick index plus one. A solid cell
// blocks completely; the build marks cells solid when the brick pool is exhausted.
#define ZEN_BRICK_EMPTY_BIT    0x80000000u
#define ZEN_BRICK_SOLID        0x7fffffffu
#define ZEN_BRICK_MAX_DISTANCE 15u
// Triangles overlapping more cells than this are marked and rasterized by one workgroup each.
#define ZEN_BRICK_LARGE_CELLS 64u
// Indirect dispatch width for the large-triangle passes; larger lists continue in y.
#define ZEN_BRICK_LARGE_GROUPS_X 65535u
// BrickCounters layout: indirect dispatch (x, y, z), then the large-triangle count, the number of
// allocated bricks and the overflow flag.
#define ZEN_BRICK_COUNTER_WORDS 8u
// A ray that starts inside occupied fine voxels ignores that starting run for up to this many fine
// voxels (brick_trace.glsl). A receiver in a concave corner sits inside the other surface's voxels;
// counting them blocked every direction and left black, voxel-stepped strips along mesh edges.
#define ZEN_BRICK_START_RUN_VOXELS 4.0

#endif // ZEN_BRICK_SCENE_H
