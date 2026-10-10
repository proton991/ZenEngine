#ifndef ZEN_ENVIRONMENT_SAMPLING_H
#define ZEN_ENVIRONMENT_SAMPLING_H

// Matches the maximum generated prefiltered environment face resolution.
#define ZEN_ENVIRONMENT_IMPORTANCE_SIZE 512u
#define ZEN_ENVIRONMENT_IMPORTANCE_ROWS (6u * ZEN_ENVIRONMENT_IMPORTANCE_SIZE)
// Order-2 spherical-harmonic projection of the same environment: one partial sum of nine
// RGB coefficients (float4 each) per 64-row column group, followed by the total.
#define ZEN_ENVIRONMENT_HARMONIC_COEFFICIENTS 9u
#define ZEN_ENVIRONMENT_HARMONIC_GROUPS       (ZEN_ENVIRONMENT_IMPORTANCE_ROWS / 64u)
// Square tiles of the importance map, eight by eight per face (64 x 64 texels each). Each tile
// stores the prefix sums of its row segments, so a direction can be drawn within any one tile.
#define ZEN_ENVIRONMENT_TILES_PER_FACE 8u
#define ZEN_ENVIRONMENT_TILE_SIZE      (ZEN_ENVIRONMENT_IMPORTANCE_SIZE / ZEN_ENVIRONMENT_TILES_PER_FACE)
#define ZEN_ENVIRONMENT_TILES          (6u * ZEN_ENVIRONMENT_TILES_PER_FACE * ZEN_ENVIRONMENT_TILES_PER_FACE)
// Tiles dominated by a compact bright source (a sun or lamp) get a dedicated ray each frame.
#define ZEN_ENVIRONMENT_MAX_SOURCES 4u

#endif
