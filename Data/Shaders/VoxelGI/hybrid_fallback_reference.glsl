// Reference luminance of (sky, bounce, specular) for a reconstruction fallback receiver: the brightest
// interpolated receiver in a 5x5 grid at a four-pixel stride, whose GI has temporal history;
// negative without any. The brightest rather than the mean keeps a thin lit receiver among darker
// neighbours (a lamp-lit bevel) from losing real light. Requires missingReceiver, the three output
// images and the receiver guides. Only missing receivers are written by the fallback pass, so these
// reads never race its writes.
vec3 HybridFallbackReference(ivec2 p)
{
    vec3 brightest = vec3(-1);
    for (int y = -2; y <= 2; ++y)
    {
        for (int x = -2; x <= 2; ++x)
        {
            ivec2 q = p + ivec2(x, y) * 4;
            if (HybridInView(q) && texelFetch(missingReceiver, q, 0).r == 0u && texelFetch(receiverSurface, q, 0).g != 0u
                && texelFetch(receiverDepth, q, 0).r < 1.0)
            {
                vec3 sky    = all(lessThan(q, imageSize(outputSky))) ? imageLoad(outputSky, q).rgb : vec3(0);
                vec3 bounce = hybrid.bounce.x != 0u ? imageLoad(outputBounce, q).rgb : vec3(0);
                brightest   = max(brightest, vec3(HybridLuminance(sky), HybridLuminance(bounce),
                                                  HybridLuminance(imageLoad(outputSpecular, q).rgb)));
            }
        }
    }
    return brightest;
}
