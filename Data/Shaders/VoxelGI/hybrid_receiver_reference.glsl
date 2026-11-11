// Mean reconstructed (diffuse, reflection) luminance of the opaque receivers around pixel p: the
// reference that bounds self-traced forward layers. Negative components where no receiver is near.
// bounce selects the ray-hit bounce texture. Requires hybridDepth, hybridSurface, hybridSky,
// hybridSpecular and hybridBounce.
vec2 HybridReceiverReference(ivec2 p, bool bounce)
{
    ivec2 extent = textureSize(hybridDepth, 0);
    vec2  total  = vec2(0);
    float count  = 0.0;
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            ivec2 q = clamp(p + ivec2(x, y) * 4, ivec2(0), extent - 1);
            if (texelFetch(hybridDepth, q, 0).r < 1.0 && texelFetch(hybridSurface, q, 0).g != 0u)
            {
                vec3 indirect  = texelFetch(hybridSky, min(q, textureSize(hybridSky, 0) - 1), 0).rgb
                               + (bounce ? texelFetch(hybridBounce, q, 0).rgb : vec3(0));
                total         += vec2(HybridLuminance(indirect), HybridLuminance(texelFetch(hybridSpecular, q, 0).rgb));
                count         += 1.0;
            }
        }
    }
    return count > 0.0 ? total / count : vec2(-1);
}
