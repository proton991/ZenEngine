layout(constant_id=0) const uint filterStep=1u;
#include "hybrid_filter_common.glsl"
layout(local_size_x=8,local_size_y=8) in;
layout(set=1,binding=7) uniform sampler2D inputSpecular;
layout(set=1,binding=8) uniform sampler2D moments;
layout(set=1,binding=9) uniform sampler2D historyLength;
layout(set=1,binding=10,rgba16f) uniform writeonly image2D filteredSky;
layout(set=1,binding=11,rgba16f) uniform writeonly image2D filteredSpecular;
layout(set=1,binding=13,rgba32f) uniform writeonly image2D filteredVariance;
layout(set=1,binding=15) uniform sampler2D inputBounce;
layout(set=1,binding=17) uniform sampler2D bounceHistory;
layout(set=1,binding=18,rgba16f) uniform writeonly image2D filteredBounce;
layout(set=1,binding=19) uniform sampler2D reflectionHistory;
#ifdef FINAL_SKY_FILTER
layout(set=1,binding=26) uniform sampler2D previousSearchWeight;
layout(set=1,binding=27,r32f) uniform writeonly image2D nextSearchWeight;
layout(std140,set=1,binding=28) uniform uHybridReuseData { uvec4 reuseData; };
layout(std430,set=1,binding=21) buffer HybridSkySearchArgs { uint searchCount; uint searchX; uint searchY; uint searchZ; };
layout(std430,set=1,binding=22) writeonly buffer HybridSkySearchPixels { uint searchPixels[]; };
#include "hybrid_sky_support_bounds.glsl"
#endif
// Bounce inputs are 1x1 placeholders unless ray-hit bounce is selected.
vec4 Bounce(ivec2 p) { return hybrid.bounce.x!=0u ? texelFetch(inputBounce,p,0) : vec4(0); }
float BounceLength(ivec2 p) { return hybrid.bounce.x!=0u ? texelFetch(bounceHistory,p,0).a : 0.0; }
vec4 Reflection(ivec2 p) { return hybrid.reflection.x!=0u ? texelFetch(reflectionHistory,p,0) : vec4(0); }
#ifdef FINAL_SKY_FILTER
bool HasSkySupport(ivec2 p, vec3 geometric, vec3 position)
{
    ivec2 lo=max(p-32,ivec2(0))/8;
    ivec2 hi=min(p+32,textureSize(inputSky,0)-1)/8;
    uint mask=SkySupportMask(geometric);
    for(int y=lo.y;y<=hi.y;++y) for(int x=lo.x;x<=hi.x;++x)
        if(TileHasSkySupport(ivec2(x,y),mask,geometric,position)) return true;
    return false;
}
#endif
void main()
{
#ifdef HYBRID_FILTER_CACHED
    LoadFilterInputs();
#endif
    ivec2 p=ivec2(gl_GlobalInvocationID.xy);
    if(!HybridInView(p)) return;
#ifdef FINAL_SKY_FILTER
    imageStore(nextSearchWeight,p,vec4(reuseData.x!=0u ? texelFetch(previousSearchWeight,p,0).r : 0.0));
#endif
    vec4 center=texelFetch(inputSky,p,0), centerSpec=texelFetch(inputSpecular,p,0);
    vec4 centerBounce=Bounce(p);
    uint identity=FilterSurface(p).g;
    // Background has no receiver and is not consumed by lighting reconstruction.
    // In particular, it must not run the dense all-miss surface search.
    if(identity==0u)
    {
        imageStore(filteredVariance,p,vec4(0));
        imageStore(filteredSky,p,center);
        imageStore(filteredSpecular,p,centerSpec);
        if(hybrid.bounce.x!=0u) imageStore(filteredBounce,p,centerBounce);
        return;
    }
    vec3 position=FilterPosition(p), normal=FilterNormal(p), geometric=FilterGeometricNormal(p);
    vec3 guide=SkyGuide(p);
    // Sky luminance and its variance are compared as ratios to the guide luminance.
    vec4 scale=vec4(1.0/HybridLuminance(guide),1,1,1);
    vec4 lengths=vec4(texelFetch(historyLength,p,0).rg,BounceLength(p),Reflection(p).w);
    vec4 mean=vec4(0), second=vec4(0); float count=0.0;
    // A local variance estimate is also used during the first four frames.
    if(filterStep==1u && (any(lessThan(lengths.xy,vec2(4)))
        || (hybrid.bounce.x!=0u && lengths.z<4.0) || (hybrid.reflection.x!=0u && lengths.w<4.0)))
    for(int y=-1;y<=1;++y) for(int x=-1;x<=1;++x)
    {
        ivec2 q=p+ivec2(x,y)*int(filterStep);
        if(!HybridInView(q) || FilterSurface(q).g!=identity) continue;
        vec4 value=vec4(HybridLuminance(texelFetch(inputSky,q,0).rgb)/HybridLuminance(SkyGuide(q)),texelFetch(inputSpecular,q,0).a,
                        HybridLuminance(Bounce(q).rgb),HybridLuminance(texelFetch(inputSpecular,q,0).rgb));
        mean+=value; second+=value*value; count+=1.0;
    }
    mean/=max(count,1.0); second/=max(count,1.0);
    vec4 variance=FilterVariance(p)*scale*scale;
    if(filterStep==1u)
        variance=mix(variance,max(second-mean*mean,vec4(0)),lessThan(lengths,vec4(4)));
    const float kernel[5]=float[5](1,4,6,4,1);
    vec4 sky=vec4(0), spec=vec4(0), total=vec4(0), varianceOut=vec4(0); vec3 bounce=vec3(0);
    vec4 centerValue=vec4(HybridLuminance(center.rgb)*scale.x,centerSpec.a,HybridLuminance(centerBounce.rgb),HybridLuminance(centerSpec.rgb));
    float roughness=FilterRoughness(p);
    for(int y=-2;y<=2;++y) for(int x=-2;x<=2;++x)
    {
        ivec2 q=p+ivec2(x,y)*int(filterStep);
        float normalWeight=SurfaceWeight(q,identity,position,normal,geometric);
        if(normalWeight==0.0) continue;
        vec4 sampleSky=texelFetch(inputSky,q,0), sampleSpec=texelFetch(inputSpecular,q,0);
        vec3 sampleGuide=SkyGuide(q);
        vec3 sampleBounce=Bounce(q).rgb;
        vec4 sampleScale=vec4(1.0/HybridLuminance(sampleGuide),1,1,1);
        vec4 difference=abs(vec4(HybridLuminance(sampleSky.rgb)*sampleScale.x,sampleSpec.a,HybridLuminance(sampleBounce),HybridLuminance(sampleSpec.rgb))-centerValue);
        vec4 neighborVariance=FilterVariance(q)*sampleScale*sampleScale;
        // Diffuse lighting must preserve the sharp visibility edges of small HDR sources.
        vec4 sigma=vec4(1.5,4,4,4)*sqrt(max(variance+neighborVariance,vec4(0)))+vec4(1e-4)*scale;
        vec4 w=kernel[x+2]*kernel[y+2]*normalWeight*exp(-difference/sigma);
        w.x=kernel[x+2]*kernel[y+2]*normalWeight*SkyWeight(sampleSky.rgb,sampleGuide,center.rgb,guide,sigma.x);
        // Smooth reflections keep a small footprint; roughness boundaries must not exchange lobes.
        if(abs(roughness-FilterRoughness(q))>0.02
            || (float(filterStep)>max(1.0,roughness*roughness*128.0) && any(notEqual(q,p)))) w.a=0.0;
        varianceOut+=w*w*neighborVariance;
        sky+=w.x*vec4(sampleSky.rgb/sampleGuide,sampleSky.a);
        spec+=vec4(w.a*sampleSpec.rgb,w.y*sampleSpec.a); total+=w;
        bounce+=w.z*sampleBounce;
    }
#ifdef FINAL_SKY_FILTER
    // Both providers append only receivers that need the dense fallback.
    if(filterStep==16u && all(equal(sky.rgb,vec3(0))) && texelFetch(moments,p,0).r==0.0
        && HasSkySupport(p,geometric,position))
    {
        uint index=atomicAdd(searchCount,1u);
        uint pixel=uint(p.y*textureSize(inputSky,0).x+p.x);
        searchPixels[index]=pixel;
        atomicMax(searchX,min(index+1u,65535u));
        atomicMax(searchY,index/65535u+1u);
    }
#endif
    imageStore(filteredVariance,p,varianceOut/max(total*total,vec4(1e-20))/(scale*scale));
    imageStore(filteredSky,p,total.x>0.0 ? vec4(sky.rgb*guide,sky.a)/total.x : center);
    imageStore(filteredSpecular,p,vec4(total.a>0.0 ? spec.rgb/total.a : centerSpec.rgb,total.y>0.0 ? spec.a/total.y : centerSpec.a));
    if(hybrid.bounce.x!=0u)
        imageStore(filteredBounce,p,vec4(total.z>0.0 ? bounce/total.z : centerBounce.rgb,centerBounce.a));
}
