#ifndef ZEN_HYBRID_RETRACE
#define ZEN_HYBRID_RETRACE
// Brick traversal queues diffuse rays whose hit lies next to a cached point or spot light
// (HitNearLocalLight, hit_radiance.glsl). The trace leaves out their bounce, and their sky when the
// sky ray is the bounce ray; hybrid_retrace_bricks.comp re-traces them against triangles and adds
// the results to the trace's images. Brick trace shaders thus carry no triangle traversal.
// Reference accumulation traces hundreds of rays per receiver and frame, more than a queue can
// hold, so its trace variants re-trace inline instead (HYBRID_INLINE_RETRACE, hybrid_trace.glsl).
#ifdef HYBRID_RETRACE_PASS
#    define HYBRID_RETRACE_HEADER_ACCESS readonly
#    define HYBRID_RETRACE_QUEUE_ACCESS  readonly
#else
#    define HYBRID_RETRACE_HEADER_ACCESS
#    define HYBRID_RETRACE_QUEUE_ACCESS writeonly
#endif
const uint HYBRID_RETRACE_NONE = 0xffffffffu;
layout(std430, set = 1, binding = 38) HYBRID_RETRACE_HEADER_ACCESS buffer HybridRetraceHeader
{
    uint groups; // The re-trace's indirect dispatch (groups, 1, 1), one thread per listed receiver.
    uint groupsY;
    uint groupsZ;
    uint receivers;   // Receivers listed.
    uint claimedRays; // Rays claimed, including claims past the ray buffer.
}
retraceHeader;
// Per queued ray: the start (one fine voxel before its brick hit) and the receiver's previous queued
// ray, then direction and sample weight. The weight's sign bit marks an environment source direction.
layout(std430, set = 1, binding = 39) HYBRID_RETRACE_QUEUE_ACCESS buffer HybridRetraceRays
{
    uvec4 retraceRays[];
};
// Per listed receiver: its texel in the re-traced images (x | y << 16) and its last queued ray.
layout(std430, set = 1, binding = 43) HYBRID_RETRACE_QUEUE_ACCESS buffer HybridRetraceReceivers
{
    uvec2 retraceReceivers[];
};
#ifdef HYBRID_RETRACE_PASS
// Whether a receiver's chain continues at this ray, after walking the given number. Every queued
// ray is visited, however many the receiver queued; the queue's capacity only bounds a corrupt chain.
bool HybridRetraceContinues(uint ray, uint walked)
{
    return ray != HYBRID_RETRACE_NONE && walked < uint(retraceRays.length()) / 2u;
}
#endif
#endif
