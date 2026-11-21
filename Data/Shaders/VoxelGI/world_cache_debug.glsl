#include "world_cache.glsl"
#include "hybrid_guides.glsl"
layout(rgba16f, set = 1, binding = 79) writeonly uniform image2D worldIrradiance;
layout(local_size_x = 8, local_size_y = 8) in;
void main()
{
    ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
    if (HybridInView(pixel))
    {
        vec3 value = texelFetch(receiverDepth, pixel, 0).r < 1.0
                       ? WorldLookup(HybridPosition(pixel), HybridGeometricNormal(pixel), HybridNormal(pixel))
                       : vec3(0);
        imageStore(worldIrradiance, pixel, vec4(value, 1));
    }
}
