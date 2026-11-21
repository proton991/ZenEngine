#ifndef ZEN_HYBRID_RAY_RESULT
#define ZEN_HYBRID_RAY_RESULT
struct HybridRayResult
{
    bool  hit;
    vec3  position;
    vec3  normal;
    ivec3 cell;
    // A completed observation: a hit or a covered miss. Only a valid miss contributes unoccluded
    // light; an invalid result has neither a surface nor a visibility contribution.
    bool valid;
    // Exact HW surface identity, retained through shared sky/bounce queries. Field hits have none.
    uvec4 triangle;
    vec2  barycentrics;
};
HybridRayResult MakeHybridRayResult(bool hit, vec3 position, vec3 normal, ivec3 cell, bool valid)
{
    return HybridRayResult(hit, position, normal, cell, valid, uvec4(0xffffffffu), vec2(0));
}
#endif
