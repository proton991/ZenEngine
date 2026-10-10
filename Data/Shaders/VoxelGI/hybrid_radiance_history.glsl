// Shared responsive lighting policy for diffuse bounce and reflected hit radiance.
// Samples from both cache generations use identical rays (P5); retain history through moving lights.
void HybridUpdateRadianceHistory(ivec2 p, uint identity, vec3 position, vec3 ng,
    sampler2D samples, sampler2D previousSamples, bool cacheChanged, bool clampToNeighbourhood,
    inout vec4 oldValue, inout vec2 oldMoments)
{
    vec3 mean=vec3(0), second=vec3(0), previous=vec3(0); float count=0.0;
    vec2 products=vec2(0); // luminance: previous squared, previous times new
    for(int y=-4;y<=4;++y) for(int x=-4;x<=4;++x)
    {
        ivec2 q=p+ivec2(x,y);
        if(!HybridInView(q) || texelFetch(receiverSurface,q,0).g!=identity) continue;
        if(dot(HybridGeometricNormal(q),ng)<0.9 || abs(dot(HybridPosition(q)-position,ng))>hybrid.rejection.x) continue;
        vec3 value=texelFetch(samples,q,0).rgb;
        mean+=value; second+=value*value; count+=1.0;
        if(cacheChanged)
        {
            vec3 old=texelFetch(previousSamples,q,0).rgb;
            previous+=old;
            products+=HybridLuminance(old)*vec2(HybridLuminance(old),HybridLuminance(value));
        }
    }
    mean/=max(count,1.0); second/=max(count,1.0); previous/=max(count,1.0);
    if(cacheChanged)
    {
        // This frame's rays read both the previous and the new radiance cache. Over the
        // neighbourhood, the ratio of the two results scales the history: the lookups are
        // correlated, so it is nearly exact for a light that moves a little, and the history
        // keeps accumulating while it moves. The relative size of the change shortens the
        // history, down to a restart when a light goes out or comes on.
        float before=HybridLuminance(previous), after=HybridLuminance(mean);
        // Beale's correction removes the ratio's first-order bias, which sparse hits make
        // about one percent when the change is largest where hits are brightest.
        float beale=1.0;
        if(before>0.0 && after>0.0 && count>1.0)
        {
            vec2 covariance=(products/count-before*vec2(before,after))*count/(count-1.0);
            beale=(1.0+covariance.y/(count*before*after))/(1.0+covariance.x/(count*before*before));
        }
        oldValue.rgb=mix(oldValue.rgb+mean,oldValue.rgb*mean/max(previous,vec3(1e-30))*beale,greaterThan(previous,vec3(0)));
        float ratio=before>0.0 ? after/before*beale : 1.0;
        float spread=max(oldMoments.y-oldMoments.x*oldMoments.x,0.0)*ratio*ratio;
        oldMoments.x=HybridLuminance(oldValue.rgb);
        oldMoments.y=spread+oldMoments.x*oldMoments.x;
        oldValue.a*=1.0-abs(after-before)/max(max(after,before),1e-30);
    }
    // Four-ray hit radiance has sparse bright samples; a 3x3, three-sigma box clipped valid
    // point-light energy. This wide box still empties history where the light went out. A
    // neighbourhood without any hit this frame collapses it to zero, so one-ray reflections skip it
    // after cache changes, where the correlated update above already empties history that went dark.
    if(clampToNeighbourhood)
    {
        vec3 deviation=10.0*sqrt(max(second-mean*mean,vec3(0)));
        oldValue.rgb=clamp(oldValue.rgb,max(mean-deviation,vec3(0)),mean+deviation);
    }
}
