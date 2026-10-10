#ifndef ZEN_HYBRID_SAMPLING
#define ZEN_HYBRID_SAMPLING
const float HYBRID_PI = 3.141592653589793;
uint HybridHash(uint x)
{
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
    return x;
}
// Burley, "Practical Hash-based Owen Scrambling" (JCGT 2020).
uint HybridOwenScramble(uint x, uint seed)
{
    x = bitfieldReverse(x);
    x += seed; x ^= x * 0x6c50b47cu; x ^= x * 0xb82f1e52u; x ^= x * 0xc7afe638u; x ^= x * 0x8d22f6e6u;
    return bitfieldReverse(x);
}
uint HybridMorton(uvec2 p)
{
    p &= 0xffffu;
    p = (p | (p << 8)) & 0x00ff00ffu; p = (p | (p << 4)) & 0x0f0f0f0fu;
    p = (p | (p << 2)) & 0x33333333u; p = (p | (p << 1)) & 0x55555555u;
    return p.x | (p.y << 1);
}
uint HybridSobol1(uint index)
{
    uint result = 0u;
    for(uint v = 1u << 31; index != 0u; index >>= 1, v ^= v >> 1)
        if((index & 1u) != 0u) result ^= v;
    return result;
}
vec2 HybridSample(uvec2 pixel, uint sampleIndex, uint dimension)
{
    // Per-pixel rotations are Owen-scrambled Sobol points in Morton order: every aligned
    // 2^k x 2^k pixel block holds a (0,2k,2)-net, so neighbors' samples stratify the
    // domain and the spatial filter averages blue-noise rather than white-noise error
    // (Ahmed and Wonka, "Screen-space blue-noise diffusion of Monte Carlo sampling error
    // via hierarchical ordering of pixels", 2020).
    uint seed = HybridHash(dimension ^ 0x9e3779b9u);
    uint index = HybridOwenScramble(HybridMorton(pixel), seed);
    uvec2 rotation = uvec2(HybridOwenScramble(bitfieldReverse(index), HybridHash(seed ^ 0x68bc21ebu)),
                           HybridOwenScramble(HybridSobol1(index), HybridHash(seed ^ 0x02e5be93u)));
    // Each pixel then follows the R2 sequence. Integer phase accumulation avoids loss of
    // the fractional sequence after many successful frames.
    uvec2 phase=rotation+sampleIndex*uvec2(3242174889u,2447445414u);
    return vec2(phase>>8)/16777216.0;
}
mat3 HybridBasis(vec3 n)
{
    vec3 t = normalize(cross(abs(n.z) < 0.999 ? vec3(0,0,1) : vec3(0,1,0), n));
    return mat3(t, cross(n,t), n);
}
vec3 HybridCosineSample(vec3 normal, vec2 u, out float pdf)
{
    float r = sqrt(u.x), phi = 2.0 * HYBRID_PI * u.y;
    float z = sqrt(max(1.0-u.x,0.0));
    pdf = z / HYBRID_PI;
    return HybridBasis(normal) * vec3(r*cos(phi),r*sin(phi),z);
}
// Heitz's isotropic GGX visible-normal sampling, including the projected-disk warp.
vec3 HybridGGXSample(vec3 normal, vec3 view, float roughness, vec2 u)
{
    mat3 basis = HybridBasis(normal);
    vec3 v = transpose(basis) * view;
    float alpha = max(roughness * roughness, 0.0016);
    vec3 vh = normalize(vec3(alpha*v.xy, max(v.z,1e-5)));
    float lensq = dot(vh.xy,vh.xy);
    vec3 t1 = lensq > 0.0 ? vec3(-vh.y,vh.x,0)/sqrt(lensq) : vec3(1,0,0);
    vec3 t2 = cross(vh,t1);
    float radius = sqrt(u.x), phi = 2.0*HYBRID_PI*u.y;
    float x = radius*cos(phi), y = radius*sin(phi);
    float s = 0.5*(1.0+vh.z);
    y = (1.0-s)*sqrt(max(1.0-x*x,0.0)) + s*y;
    vec3 nh = x*t1+y*t2+sqrt(max(1.0-x*x-y*y,0.0))*vh;
    vec3 h = basis * normalize(vec3(alpha*nh.xy,max(0.0,nh.z)));
    return reflect(-view,h);
}
float HybridSmithG1(float cosine, float alpha)
{
    float a2 = alpha*alpha;
    return cosine > 0.0 ? 2.0*cosine/(cosine+sqrt(a2+(1.0-a2)*cosine*cosine)) : 0.0;
}
const uint HYBRID_SPECULAR_CANDIDATES = 16u;
// Samples the environment-specular lobe f*cos over the upper hemisphere of n, which is the
// domain of the prefiltered map and the split-sum terms. With separable Smith masking the
// visible-normal density is f*cos/G1(l), so a candidate is accepted with probability G1(l).
// Directions below n are outside that integral; counting them as blocked would leave S<1 on
// an unoccluded rough surface. The bounded loop falls back to its first above-horizon
// candidate (at most 0.3% of samples, at roughness 1 and normal incidence).
vec3 HybridSpecularDirection(vec3 normal, vec3 view, float roughness, uvec2 pixel, uint sampleIndex, uint dimension)
{
    float alpha = max(roughness * roughness, 0.0016);
    vec3 direction = normal, fallback = normal;
    bool found = false;
    for(uint k = 0u; k < HYBRID_SPECULAR_CANDIDATES; ++k)
    {
        // Rejection requires independent dimensions. Two rotated R2 sequences have the same
        // per-frame increment, so using another rotation's x for acceptance correlates it with
        // the candidate and biases the lobe. Preserve the stratified first candidate, then use
        // independent counter hashes for acceptance and retries.
        uint seed=HybridHash(HybridMorton(pixel)^HybridHash(sampleIndex)^HybridHash(dimension+k));
        vec2 u=k==0u ? HybridSample(pixel,sampleIndex,dimension)
                     : vec2(uvec2(HybridHash(seed^0x68bc21ebu),HybridHash(seed^0x02e5be93u))>>8)/16777216.0;
        direction = HybridGGXSample(normal, view, roughness, u);
        float cosine = dot(normal, direction);
        if(cosine <= 0.0) continue;
        if(float(HybridHash(seed^0x9e3779b9u)>>8)/16777216.0 < HybridSmithG1(cosine, alpha)) return direction;
        fallback = found ? fallback : direction;
        found = true;
    }
    return found ? fallback : direction - 2.0*dot(normal, direction)*normal;
}
#endif
