#ifndef ZEN_VOXEL_SURFACE_CLASS_GLSL
#define ZEN_VOXEL_SURFACE_CLASS_GLSL
#include "voxel_coverage.glsl"
// A cell's surfaces are split by the dominant axis of their oriented geometric normal and its sign,
// so an arch soffit and its front face, a floor and the wall base beside it, or the inside and the
// outside of a lantern pane keep separate owners. Class volumes stack the classes along z: class c
// of class cell p is texel (p.x, p.y, p.z + c * resolution). They may be coarser than the voxel grid
// (ZEN_LIGHT_CACHE_MAX_RESOLUTION).
int VoxelSurfaceClass(vec3 normal)
{
    int axis = VoxelProjectionAxis(normal);
    return normal[axis] < 0.0 ? axis + 3 : axis;
}
ivec3 VoxelClassTexel(ivec3 cell, int surfaceClass, int resolution)
{
    return ivec3(cell.xy, cell.z + surfaceClass * resolution);
}
#endif
