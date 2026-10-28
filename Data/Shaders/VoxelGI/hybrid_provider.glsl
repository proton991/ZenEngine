#ifndef ZEN_HYBRID_PROVIDER
#define ZEN_HYBRID_PROVIDER
#include "gi_common.glsl"
#include "hybrid_ray_counts.glsl"
struct HybridRayResult
{
    bool  hit;
    vec3  position;
    vec3  normal;
    ivec3 cell;
    // A completed observation: a hit or a covered miss. Only a valid miss contributes unoccluded
    // light; an invalid result has neither a surface nor a visibility contribution.
    bool valid;
};
#ifdef HYBRID_FIELD_TRAVERSAL
// Compute features trace the GPU-built sparse distance field (DistanceFieldScene), with finer local
// volumes around lights, whose lanterns let light out through openings the scene field closes.
#    define DISTANCE_FIELD_BINDING 20
#    define DISTANCE_FIELD_LOCALS
#    include "distance_field_trace.glsl"
// Radiance-cache cell of a world position; a position outside the cache maps to no cell.
ivec3 HybridCacheCell(vec3 position)
{
    ivec3 cell = ivec3(floor((position - gi.gridMinVoxelSize.xyz) / gi.gridMinVoxelSize.w));
    return all(greaterThanEqual(cell, ivec3(0))) && all(lessThan(cell, ivec3(gi.volume.x))) ? cell : ivec3(-1);
}
// Start offset that leaves the receiver's own hit band: the march accepts a hit within twice the
// tolerance of a surface, and trilinear interpolation of the unsigned field adds up to half a
// sample spacing (Doc/ComputeGI/BrixelizerGIReferenceMathExperiment.md, outward-surface sweep). Inside
// a local volume its finer bricks set the band, so thin fixture parts are not stepped over.
vec3 FieldTraceOrigin(vec3 position, vec3 normal)
{
    float bias = DistanceFieldBrickSizeAt(position) * (2.0 * distanceField.tracing.x + 0.5 / 7.0);
    return position + normal * bias;
}
// Field traces of [0, maximumDistance). Hits outside the radiance cache keep blocking light and
// carry no cache cell; unavailable data or an exhausted budget returns an invalid result.
HybridRayResult FieldTraceRay(vec3 origin, vec3 direction, float maximumDistance, bool normals)
{
#    ifdef HYBRID_COUNT_RAYS
    ++issuedComputeRays[issuedRayFeature];
#    endif
    DistanceFieldTraceResult trace;
    bool                     retry = true;
    for (uint attempt = 0u; attempt < 2u && retry; ++attempt)
    {
        // A long dense ray may retry once with four times the default budget. Keep one
        // traversal call site and account for every attempt through the same path.
        uint budget = attempt == 0u ? 0u : distanceField.dimensions.w * 4u;
        trace       = DistanceFieldTraceCurrent(origin, direction, maximumDistance, budget, normals);
#    ifdef HYBRID_COUNT_RAYS
        issuedFieldWork.xy += uvec2(trace.iterations, trace.distanceSamples);
        issuedFieldWork.w +=
            uint(trace.failureReason == DISTANCE_FIELD_FAILURE_WORK || trace.failureReason == DISTANCE_FIELD_FAILURE_BRICK);
#    endif
        retry = trace.status == DISTANCE_FIELD_TRACE_UNAVAILABLE && trace.failureReason == DISTANCE_FIELD_FAILURE_WORK;
    }
#    ifdef HYBRID_COUNT_RAYS
    issuedFieldWork.z += uint(trace.status == DISTANCE_FIELD_TRACE_UNAVAILABLE);
#    endif
    bool hit = trace.status == DISTANCE_FIELD_HIT;
    return HybridRayResult(hit, hit ? trace.position : origin, hit ? trace.normal : vec3(0),
                           hit ? HybridCacheCell(trace.position) : ivec3(-1), trace.status != DISTANCE_FIELD_TRACE_UNAVAILABLE);
}

float FieldLightVisibility(HybridRayResult hit, vec3 lightPosition)
{
    vec3  origin            = FieldTraceOrigin(hit.position, hit.normal);
    vec3  delta             = lightPosition - origin;
    float distanceToLight   = length(delta);
    vec3  direction         = delta / max(distanceToLight, 1e-20);
    distanceToLight         = uintBitsToFloat(floatBitsToUint(distanceToLight) - uint(distanceToLight > 0.0));
    HybridRayResult segment = FieldTraceRay(origin, direction, distanceToLight, false);
    return segment.valid && !segment.hit ? 1.0 : 0.0;
}
#endif
#if defined(HYBRID_VOXEL_REFERENCE)
#    define HybridTraceRay(occupancy, origin, direction, maximumDistance) \
        VoxelTraceRay(occupancy, origin, direction, maximumDistance)
#elif defined(HYBRID_HARDWARE_PROVIDER)
#    include "hardware_ray_query.glsl"
// Call sites share triangle visibility; only the traversal mechanism differs.
#    define HybridTraceRay(occupancy, origin, direction, maximumDistance) \
        HardwareTraceRay(origin, direction, maximumDistance, false)
#    define HybridTraceClosestRay(occupancy, origin, direction, maximumDistance) \
        HardwareTraceRay(origin, direction, maximumDistance, true)
#elif defined(HYBRID_FIELD_TRAVERSAL)
// The field reports its first surface along the ray, so closest and any-hit traces coincide.
#    define HybridTraceRay(occupancy, origin, direction, maximumDistance) \
        FieldTraceRay(origin, direction, maximumDistance, false)
#    define HybridTraceClosestRay(occupancy, origin, direction, maximumDistance) \
        FieldTraceRay(origin, direction, maximumDistance, true)
#endif
// Start of a trace from a receiver surface with this variant's provider: past the field's hit
// band, or the triangle offset of HW RT.
vec3 ComputeTraceOrigin(vec3 position, vec3 normal)
{
#if defined(HYBRID_FIELD_TRAVERSAL) && !defined(HYBRID_HARDWARE_PROVIDER)
    return FieldTraceOrigin(position, normal);
#elif defined(HYBRID_HARDWARE_PROVIDER)
    return HybridTraceOrigin(position, normal, 0.0);
#else
    return position;
#endif
}
// The starting cell is tested. Do not skip occupied starts: that leaks at wall bases.
// Slab entry also handles rays starting outside the voxel volume.
HybridRayResult VoxelTraceRay(sampler3D occupancy, vec3 origin, vec3 direction, float maximumDistance)
{
    HybridRayResult result = HybridRayResult(false, origin, vec3(0), ivec3(-1), true);
    vec3            p      = (origin - gi.gridMinVoxelSize.xyz) / gi.gridMinVoxelSize.w;
    float           enter = 0.0, leave = maximumDistance / gi.gridMinVoxelSize.w;
    // An occupied origin blocks visibility, but has no entered face. Do not
    // invent a facing normal: bounce cannot identify which wall side it hit.
    vec3 entryNormal = vec3(0);
    bool intersects  = true;
    for (int axis = 0; axis < 3; ++axis)
    {
        if (abs(direction[axis]) < 1e-9)
        {
            intersects = intersects && p[axis] >= 0.0 && p[axis] < gi.volume.x;
        }
        else
        {
            float a = -p[axis] / direction[axis], b = (gi.volume.x - p[axis]) / direction[axis];
            float nearDistance = min(a, b);
            if (nearDistance > enter)
            {
                enter             = nearDistance;
                entryNormal       = vec3(0);
                entryNormal[axis] = -sign(direction[axis]);
            }
            leave = min(leave, max(a, b));
        }
    }
    if (intersects && leave > enter)
    {
#ifdef HYBRID_COUNT_RAYS
        ++issuedComputeRays[issuedRayFeature];
#endif
        vec3  entry         = p + direction * enter;
        ivec3 cell          = ivec3(clamp(floor(entry), vec3(0), vec3(gi.volume.x - 1.0)));
        ivec3 stepDirection = ivec3(sign(direction));
        vec3  delta         = 1.0 / max(abs(direction), vec3(1e-9));
        vec3  boundary      = vec3(cell) + max(vec3(stepDirection), vec3(0));
        vec3  next          = abs(boundary - entry) * delta + enter;
        next                = mix(next, vec3(1e30), lessThan(abs(direction), vec3(1e-9)));
        float distance      = enter;
        vec3  normal        = entryNormal;
        for (int i = 0; i < int(3.0 * gi.volume.x) + 3; ++i)
        {
            if (any(lessThan(cell, ivec3(0))) || any(greaterThanEqual(cell, ivec3(gi.volume.x))) || distance >= leave)
            {
                break;
            }
            if (texelFetch(occupancy, cell, 0).a > 0.5)
            {
                result = HybridRayResult(true, origin + direction * distance * gi.gridMinVoxelSize.w, normal, cell, true);
                break;
            }
            int axis      = next.x < next.y ? 0 : 1;
            axis          = next[axis] < next.z ? axis : 2;
            distance      = next[axis];
            cell[axis]   += stepDirection[axis];
            next[axis]   += delta[axis];
            normal        = vec3(0);
            normal[axis]  = -float(stepDirection[axis]);
        }
    }
    return result;
}
#endif
