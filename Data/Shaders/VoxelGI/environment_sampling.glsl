#ifndef ZEN_ENVIRONMENT_SAMPLING
#define ZEN_ENVIRONMENT_SAMPLING
#include "environment_distribution.glsl"
#include "hybrid_sampling.glsl"
layout(std430,set=3,binding=13) readonly buffer EnvironmentColumns { float environmentColumns[]; };
layout(std430,set=3,binding=14) readonly buffer EnvironmentRows { float environmentRows[]; };

// Densities use the stored CDF intervals, including rounding, and the cube's
// area-to-solid-angle Jacobian. Sampling and PDF evaluation share this function.
float EnvironmentCellPDF(uint row, uint column, vec2 uv)
{
    uint offset = row * (ZEN_ENVIRONMENT_IMPORTANCE_SIZE + 1u);
    float rowTotal = environmentColumns[offset + ZEN_ENVIRONMENT_IMPORTANCE_SIZE];
    float total = environmentRows[ZEN_ENVIRONMENT_IMPORTANCE_ROWS];
    float probability = (environmentRows[row + 1u] - environmentRows[row])
        * (environmentColumns[offset + column + 1u] - environmentColumns[offset + column]);
    return probability / max(total * rowTotal, 1e-30)
        * (float(ZEN_ENVIRONMENT_IMPORTANCE_SIZE * ZEN_ENVIRONMENT_IMPORTANCE_SIZE) * 0.25)
        / EnvironmentCubeJacobian(uv);
}

float EnvironmentPDF(vec3 direction)
{
    vec3 d = EnvironmentDirection(direction), a = abs(d);
    uint face;
    vec2 uv;
    if(a.x >= a.y && a.x >= a.z)
    {
        face = d.x >= 0.0 ? 0u : 1u;
        uv = vec2(d.x >= 0.0 ? -d.z : d.z, -d.y) / a.x;
    }
    else if(a.y >= a.z)
    {
        face = d.y >= 0.0 ? 2u : 3u;
        uv = vec2(d.x, d.y >= 0.0 ? d.z : -d.z) / a.y;
    }
    else
    {
        face = d.z >= 0.0 ? 4u : 5u;
        uv = vec2(d.z >= 0.0 ? d.x : -d.x, -d.y) / a.z;
    }
    uvec2 cell = uvec2(clamp((uv * 0.5 + 0.5) * float(ZEN_ENVIRONMENT_IMPORTANCE_SIZE),
        vec2(0), vec2(ZEN_ENVIRONMENT_IMPORTANCE_SIZE - 1u)));
    return EnvironmentCellPDF(face * ZEN_ENVIRONMENT_IMPORTANCE_SIZE + cell.y, cell.x, uv);
}

vec3 SampleEnvironment(vec2 u, out float pdf)
{
    float total = environmentRows[ZEN_ENVIRONMENT_IMPORTANCE_ROWS];
    float target = min(u.y * total, uintBitsToFloat(floatBitsToUint(total) - 1u));
    uint low = 0u, high = ZEN_ENVIRONMENT_IMPORTANCE_ROWS;
    while(low + 1u < high)
    {
        uint middle = (low + high) / 2u;
        if(environmentRows[middle] <= target) low = middle;
        else high = middle;
    }
    uint row = low;
    float y = (target - environmentRows[row]) / (environmentRows[row + 1u] - environmentRows[row]);
    uint offset = row * (ZEN_ENVIRONMENT_IMPORTANCE_SIZE + 1u);
    total = environmentColumns[offset + ZEN_ENVIRONMENT_IMPORTANCE_SIZE];
    target = min(u.x * total, uintBitsToFloat(floatBitsToUint(total) - 1u));
    low = 0u; high = ZEN_ENVIRONMENT_IMPORTANCE_SIZE;
    while(low + 1u < high)
    {
        uint middle = (low + high) / 2u;
        if(environmentColumns[offset + middle] <= target) low = middle;
        else high = middle;
    }
    float x = (target - environmentColumns[offset + low])
        / (environmentColumns[offset + low + 1u] - environmentColumns[offset + low]);
    vec2 uv = (vec2(low, row % ZEN_ENVIRONMENT_IMPORTANCE_SIZE) + min(vec2(x,y),vec2(0.99999994)))
        * (2.0 / float(ZEN_ENVIRONMENT_IMPORTANCE_SIZE)) - 1.0;
    pdf = EnvironmentCellPDF(row, low, uv);
    vec3 direction = EnvironmentCubeDirection(row / ZEN_ENVIRONMENT_IMPORTANCE_SIZE, uv);
    // Invert EnvironmentDirection: inverse authored quaternion, then inverse Y rotation.
    vec4 q = vec4(-sceneUbo.environmentOrientation.xyz, sceneUbo.environmentOrientation.w);
    direction += 2.0 * cross(q.xyz, cross(q.xyz, direction) + q.w * direction);
    float c = cos(sceneUbo.environment.y), s = sin(sceneUbo.environment.y);
    return vec3(c * direction.x + s * direction.z, direction.y, -s * direction.x + c * direction.z);
}

// Share of sky samples drawn from the environment distribution; the rest are cosine
// samples. Callers allocate exactly this share (or select each proposal with this
// probability), so the mixture density below is the sampling density. Indoor receivers
// see the sky through openings, and environment samples find its bright parts there;
// the cosine share bounds the weight of dim visible directions (2026-10-09 measurements
// in Doc/HybridGI/P4.md).
const float HYBRID_ENVIRONMENT_SHARE = 0.75;

vec3 HybridSkySample(vec3 normal, vec2 u, bool environmentSample, out float pdf)
{
    vec3 direction;
    if(environmentRows[ZEN_ENVIRONMENT_IMPORTANCE_ROWS] <= 0.0)
        direction = HybridCosineSample(normal, u, pdf);
    else
    {
        if(environmentSample)
        {
            direction = SampleEnvironment(u, pdf);
            pdf = HYBRID_ENVIRONMENT_SHARE * pdf + (1.0 - HYBRID_ENVIRONMENT_SHARE) * max(dot(normal, direction), 0.0) / HYBRID_PI;
        }
        else
        {
            direction = HybridCosineSample(normal, u, pdf);
            pdf = (1.0 - HYBRID_ENVIRONMENT_SHARE) * pdf + HYBRID_ENVIRONMENT_SHARE * EnvironmentPDF(direction);
        }
    }
    return direction;
}

// Proposal for sample i of count: a deterministic split when count is a multiple of four,
// otherwise a stratified per-sample choice that consumes and remaps u.x.
bool HybridEnvironmentProposal(uint i, uint count, inout vec2 u)
{
    if(count % 4u == 0u)
        return i < count * 3u / 4u;
    bool environment = u.x < HYBRID_ENVIRONMENT_SHARE;
    u.x = environment ? u.x / HYBRID_ENVIRONMENT_SHARE : (u.x - HYBRID_ENVIRONMENT_SHARE) / (1.0 - HYBRID_ENVIRONMENT_SHARE);
    u.x = min(u.x, 0.99999994);
    return environment;
}
#endif
