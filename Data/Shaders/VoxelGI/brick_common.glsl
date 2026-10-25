#ifndef ZEN_BRICK_COMMON
#define ZEN_BRICK_COMMON
#include "Graphics/Shared/BrickScene.h"
// Sparse brick scene (BrickScene.cpp). Coordinates are in cells or in fine voxels of the grid.
#define BRICK_DATA_BINDING  0
#define BRICK_CELLS_BINDING 1
#define BRICK_BITS_BINDING  2
#define BRICK_MASKS_BINDING 8
layout(std140, set = 4, binding = BRICK_DATA_BINDING) uniform uBrickData
{
    vec4  gridMinCellSize; // xyz grid minimum, w cell size
    uvec4 dimensions;      // x cells per axis, y brick capacity, z triangle count
    vec4  voxel;           // x fine voxel size, y inverse fine voxel size, z trace origin bias in world units
}
brick;
#ifdef BRICK_BUILD
// The clear pass writes all brick storage and reads none of it.
#    ifdef BRICK_CLEAR
#        define BRICK_STORAGE_ACCESS writeonly
#    else
#        define BRICK_STORAGE_ACCESS
#    endif
layout(std430, set = 4, binding = BRICK_CELLS_BINDING) BRICK_STORAGE_ACCESS buffer BrickCells
{
    uint brickCells[];
};
layout(std430, set = 4, binding = BRICK_BITS_BINDING) BRICK_STORAGE_ACCESS buffer BrickBits
{
    uint brickBits[];
};
layout(std430, set = 4, binding = BRICK_MASKS_BINDING) BRICK_STORAGE_ACCESS buffer BrickMasks
{
    uint brickMasks[];
};
#else
layout(std430, set = 4, binding = BRICK_CELLS_BINDING) readonly buffer BrickCells
{
    uint brickCells[];
};
layout(std430, set = 4, binding = BRICK_BITS_BINDING) readonly buffer BrickBits
{
    uint brickBits[];
};
// Per brick: which ZEN_BRICK_BLOCK-sided blocks hold occupied voxels (Graphics/Shared/BrickScene.h).
layout(std430, set = 4, binding = BRICK_MASKS_BINDING) readonly buffer BrickMasks
{
    uint brickMasks[];
};
#endif
int BrickResolution()
{
    return int(brick.dimensions.x);
}
uint BrickCellIndex(ivec3 cell)
{
    uint size = brick.dimensions.x;
    return (uint(cell.z) * size + uint(cell.y)) * size + uint(cell.x);
}
bool BrickCellInside(ivec3 cell)
{
    return all(greaterThanEqual(cell, ivec3(0))) && all(lessThan(cell, ivec3(BrickResolution())));
}
// Whether a cell value holds a brick index (plus one).
bool BrickCellHasBrick(uint value)
{
    return value != 0u && value < ZEN_BRICK_SOLID;
}
// Bit index of a fine voxel inside its brick, local coordinates in [0, 8).
uint BrickBitIndex(ivec3 local)
{
    return (uint(local.z) * ZEN_BRICK_SIZE + uint(local.y)) * ZEN_BRICK_SIZE + uint(local.x);
}
#endif
