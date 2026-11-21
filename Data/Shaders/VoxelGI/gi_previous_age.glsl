// Reprojection and rejection shared by preblur and young-receiver sampling.
vec2 PreviousAge(ivec2 p, GIDenoiseGuide center)
{
    vec2 age    = vec2(0);
    vec4 motion = HybridMotion(p);
    if (hybrid.sampling.w != 0u && motion.z > 0.0)
    {
        vec2 previousPixel =
            hybrid.resolution.w != 0u ? vec2(p) : vec2(p) + motion.xy * 0.5 * vec2(textureSize(receiverDepth, 0));
        ivec2 q = ivec2(round(previousPixel));
        if (HybridInView(q))
        {
            uvec2          surface            = texelFetch(previousReceiver, q, 0).rg;
            vec4           lengthAndRoughness = texelFetch(previousLength, q, 0);
            GIDenoiseGuide old =
                GIDenoiseGuide(texelFetch(previousPosition, q, 0).xyz, DecodeGBufferNormal(texelFetch(previousNormal, q, 0).rg),
                               DecodeGBufferNormal(unpackUnorm2x16(surface.r)), surface.g, lengthAndRoughness.w);
            if (DenoiseHistoryRejection(center, old, false) == 0.0)
            {
                age.x = lengthAndRoughness.x;
                if (hybrid.reflection.x != 0u && DenoiseHistoryRejection(center, old, true) == 0.0)
                {
                    age.y = texelFetch(previousReflection, q, 0).w;
                }
            }
        }
    }
    return age;
}
