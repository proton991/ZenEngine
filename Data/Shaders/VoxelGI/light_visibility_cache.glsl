#ifndef ZEN_LIGHT_VISIBILITY_CACHE_GLSL
#define ZEN_LIGHT_VISIBILITY_CACHE_GLSL
// Light-visibility cache for ray hits (gi.limits.w set). For every class cell and surface class
// (voxel_surface_class.glsl) it keeps one bit per scene light: whether the light reaches that
// class's surfaces, traced once per injection from the class owner. A hit evaluates each visible
// light's attenuation and cosine at its own position and normal, and traces near lights
// (LightCacheNear) itself. Bit 31 marks a class present in the cell, so the first
// ZEN_LIGHT_CACHE_LIGHTS scene lights are cached; later lights and the camera light stay in the
// isotropic radiance. Each cache buffer starts with what its masks were traced for:
// uvec4 lightCount (x: cached lights, y: nonzero with averaged reflectance, z: class resolution),
// then SceneLight cacheLights[MAX_SCENE_LIGHTS], then one uint mask per class texel.
#define ZEN_LIGHT_CACHE_LIGHTS  31
#define ZEN_LIGHT_CACHE_PRESENT 0x80000000u
// Visibility starts this far off the owner along its normal, in class cells. From the surface
// itself, a light behind the owner's tangent plane is hidden by the solid a curved class (column,
// arch) wraps around, although other surfaces of the class face it. Larger offsets let sunlight
// pass occluders the class's surfaces do not see past (New Sponza sun bounce: 0 -6%, 0.1 +1%, 0.25 +9%).
#define ZEN_LIGHT_CACHE_ORIGIN_OFFSET 0.1
// A point or spot light within this many class cells of a cell's center lights the cell's surfaces
// differently (lantern panes centimetres from their lamp). Each hit in the cell traces it instead;
// injection sets its bit, so a lookup visits set bits only. Without these rays New Sponza's floor
// bounce was 23% low at 128^3 (all receivers 10%); 1.5 and 2 cells measured the same.
#define ZEN_LIGHT_CACHE_NEAR_CELLS 1.5
// Class volumes stack along z, as the class owners do (VoxelClassTexel).
uint LightCacheIndex(ivec3 cell, int surfaceClass, int resolution)
{
    return uint(cell.x + resolution * (cell.y + resolution * (cell.z + surfaceClass * resolution)));
}
bool LightCacheNear(SceneLight light, vec3 center, float size)
{
    return int(light.directionType.w) != 0 && distance(light.positionRange.xyz, center) < ZEN_LIGHT_CACHE_NEAR_CELLS * size;
}
#endif
