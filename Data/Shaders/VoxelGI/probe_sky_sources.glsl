#include "probe_pixel.glsl"
#include "probe_ray.glsl"
#include "environment_sampling.glsl"
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 7, binding = 30, rgba16f) uniform writeonly image2D probeSourceSky;

// Compact bright sources need visibility at the receiver: interpolating their shadows between
// probes smears sunlit edges even after the probes converge. The broader sky stays in the probes.
void main()
{
    ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
    if (HybridInView(pixel))
    {
        ProbeReceiver receiver = ProbeLoadReceiver(pixel);
        vec3          sky      = vec3(0);
        bool          valid    = true;
        float         scale    = sceneUbo.environment.x * sceneUbo.environment.z * gi.lighting.y;
        if (receiver.valid && scale > 0.0)
        {
            const uint samples = 2u;
            for (uint k = 0u; k < environmentSourceCount; ++k)
            {
                vec3 sum       = vec3(0);
                uint completed = samples;
                for (uint j = 0u; j < samples; ++j)
                {
                    float pdf;
                    vec2  u         = HybridSkySample(uvec2(pixel), hybrid.sampling.x * samples + j, 0x8000u + k * 0x1000u);
                    vec3  direction = SampleEnvironmentTile(environmentSourceTiles[k], u, pdf);
                    if (pdf > 0.0 && dot(receiver.normal, direction) > 0.0 && dot(receiver.geometric, direction) > 0.0)
                    {
                        HybridRayResult hit =
                            ProbeTraceFeature(SKY_PROVIDER, receiver.position, receiver.geometric, direction, false);
                        completed -= hit.valid ? 0u : 1u;
                        if (hit.valid && !hit.hit)
                        {
                            vec3 radiance  = textureLod(coneEnvironmentMap, EnvironmentDirection(direction), 0.0).rgb;
                            sum           += radiance * (dot(receiver.normal, direction) / (HYBRID_PI * pdf));
                        }
                    }
                }
                sky   += sum * (scale / float(max(completed, 1u)));
                valid  = valid && completed > 0u;
            }
        }
        imageStore(probeSourceSky, pixel, vec4(sky, valid ? 1.0 : -1.0));
    }
}
