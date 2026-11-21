#include "hybrid_guides.glsl"
#include "hybrid_sampling.glsl"
#include "hybrid_provider.glsl"
void main()
{
    uvec2 block  = gl_LaunchIDEXT.xy;
    uint  offset = (HybridHash(block.x + block.y * gl_LaunchSizeEXT.x) + hybrid.sampling.x) & 15u;
    ivec2 pixel  = ivec2(block * 4u + uvec2(offset & 3u, offset >> 2u));
    if (HybridInView(pixel) && texelFetch(receiverDepth, pixel, 0).r < 1.0)
    {
        vec3  normal = HybridNormal(pixel), geometric = HybridGeometricNormal(pixel);
        float pdf;
        vec3  direction = HybridCosineSample(normal, HybridSample(uvec2(pixel), hybrid.sampling.x, 0x37a19u), pdf);
        if (dot(geometric, direction) > 0.0)
        {
            rayShading =
                uvec3(HybridHash(HybridMorton(uvec2(pixel)) ^ hybrid.sampling.x), uint((hybrid.reservoir.x & 1u) != 0u), 2u);
            HardwareTraceRay(HybridTraceOrigin(HybridPosition(pixel), geometric, 0.0), direction, 1e20, true);
        }
    }
}
