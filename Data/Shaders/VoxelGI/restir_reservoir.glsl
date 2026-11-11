#ifndef ZEN_RESTIR_RESERVOIR
#define ZEN_RESTIR_RESERVOIR
#include "../Common/gbuffer.glsl"
// Six scalar words: no vec3 std430 padding. Persistent weight is w_sum/(M*target).
struct PackedGIReservoir
{
    float x;
    float y;
    float z;
    uint  normalCount;
    uint  radiance;
    float weight;
};
struct GIReservoir
{
    vec3  position;
    vec3  normal;
    vec3  radiance;
    float weight;
    uint  count;
};
GIReservoir EmptyGIReservoir()
{
    return GIReservoir(vec3(0), vec3(0, 0, 1), vec3(0), 0.0, 0u);
}
uint PackGIRadiance(vec3 value)
{
    value          = clamp(value, vec3(0), vec3(65408));
    float maximum  = max(max(value.x, value.y), value.z);
    int   exponent = max(-16, int(floor(log2(max(maximum, exp2(-16.0)))))) + 16;
    float scale    = exp2(float(exponent - 24));
    if (floor(maximum / scale + 0.5) >= 512.0)
    {
        exponent += 1;
        scale    *= 2.0;
    }
    uvec3 encoded = uvec3(floor(value / scale + 0.5));
    return encoded.x | (encoded.y << 9u) | (encoded.z << 18u) | (uint(exponent) << 27u);
}
vec3 UnpackGIRadiance(uint value)
{
    return vec3(uvec3(value, value >> 9u, value >> 18u) & 511u) * exp2(float(int(value >> 27u) - 24));
}
PackedGIReservoir PackGIReservoir(GIReservoir r)
{
    uvec2 normal  = uvec2(round(EncodeGBufferNormal(r.normal) * 4095.0));
    uint  bits    = normal.x | (normal.y << 12u) | (min(r.count, 127u) << 24u);
    bits         |= r.weight > 0.0 ? 0x80000000u : 0u;
    return PackedGIReservoir(r.position.x, r.position.y, r.position.z, bits, PackGIRadiance(r.radiance), r.weight);
}
GIReservoir UnpackGIReservoir(PackedGIReservoir p)
{
    GIReservoir r =
        GIReservoir(vec3(p.x, p.y, p.z), DecodeGBufferNormal(vec2(uvec2(p.normalCount, p.normalCount >> 12u) & 4095u) / 4095.0),
                    UnpackGIRadiance(p.radiance), p.weight, (p.normalCount >> 24u) & 127u);
    r.weight = (p.normalCount & 0x80000000u) != 0u ? r.weight : 0.0;
    return r;
}
float GITarget(vec3 value)
{
    return dot(value, vec3(0.2126, 0.7152, 0.0722));
}
bool GICompatibleSurface(uint  identity,
                         uint  sourceIdentity,
                         vec3  position,
                         vec3  n,
                         vec3  ng,
                         vec3  sourcePosition,
                         vec3  sourceNormal,
                         vec3  sourceGeometric,
                         float threshold)
{
    return identity != 0u && identity == sourceIdentity && abs(dot(sourcePosition - position, ng)) <= threshold
        && dot(sourceNormal, n) >= 0.9 && dot(sourceGeometric, ng) >= 0.9;
}
vec3 GIDiffuse(GIReservoir r, vec3 position, vec3 normal, vec3 geometric)
{
    vec3  delta     = r.position - position;
    float distance2 = dot(delta, delta);
    vec3  direction = delta * inversesqrt(max(distance2, 1e-20));
    return distance2 > 1e-20 && dot(geometric, direction) > 0.0
             ? r.radiance * max(dot(normal, direction), 0.0) / 3.141592653589793
             : vec3(0);
}
// Reconnection maps the source solid-angle measure into the destination's measure.
float GIJacobian(GIReservoir r, vec3 source, vec3 destination)
{
    vec3  a = source - r.position, b = destination - r.position;
    float a2 = dot(a, a), b2 = dot(b, b);
    // HW hit normals face the original receiver. Crossing to the opposite side cannot reuse its Lo.
    float ca = max(dot(r.normal, a), 0.0) * inversesqrt(max(a2, 1e-20));
    float cb = max(dot(r.normal, b), 0.0) * inversesqrt(max(b2, 1e-20));
    return a2 > 1e-20 && b2 > 1e-20 && ca > 1e-6 ? cb * a2 / (ca * b2) : 0.0;
}
void GIStream(inout GIReservoir result, GIReservoir candidate, float weight, uint count, float random)
{
    // weight temporarily stores the sum; normalization happens once after all streams.
    weight    = isnan(weight) || isinf(weight) ? 0.0 : max(weight, 0.0);
    float sum = result.weight + weight;
    if (weight > 0.0 && random * sum < weight)
    {
        result.position = candidate.position;
        result.normal   = candidate.normal;
        result.radiance = candidate.radiance;
    }
    result.weight  = sum;
    result.count  += count;
}
void GIFinalize(inout GIReservoir r, float target)
{
    r.weight = r.count > 0u && target > 0.0 ? r.weight / (float(r.count) * target) : 0.0;
    r.weight = isnan(r.weight) || isinf(r.weight) ? 0.0 : r.weight;
    r.count  = min(r.count, 127u);
}
#endif
