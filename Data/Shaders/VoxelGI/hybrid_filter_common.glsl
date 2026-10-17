#include "hybrid_guides.glsl"
layout(set=1,binding=6) uniform sampler2D inputSky;
layout(set=1,binding=12) uniform sampler2D inputVariance;
// RGB is the visible irradiance guide. Triangle visibility has no occupied-start boundary.
layout(set=1,binding=14) uniform sampler2D skyGuide;
// Temporal packs roughness in the unused fourth position component. All filters
// keep full-precision positions and avoid a separate roughness texture fetch per tap.
layout(set=1,binding=25) uniform sampler2D filterPosition;
#ifdef HYBRID_FILTER_CACHED
const uint FILTER_CACHE_WIDTH=8u+4u*filterStep;
const uint FILTER_CACHE_PIXELS=FILTER_CACHE_WIDTH*FILTER_CACHE_WIDTH;
shared vec4 filterPositions[FILTER_CACHE_PIXELS];
shared uvec2 filterSurfaces[FILTER_CACHE_PIXELS];
shared uint filterNormals[FILTER_CACHE_PIXELS];
shared vec4 filterVariances[FILTER_CACHE_PIXELS];
uint FilterCacheIndex(ivec2 p)
{
    ivec2 q=p-ivec2(gl_WorkGroupID.xy)*8+int(2u*filterStep);
    return uint(q.y)*FILTER_CACHE_WIDTH+uint(q.x);
}
void LoadFilterInputs()
{
    ivec2 origin=ivec2(gl_WorkGroupID.xy)*8-int(2u*filterStep);
    for(uint index=gl_LocalInvocationIndex;index<FILTER_CACHE_PIXELS;index+=64u)
    {
        ivec2 q=origin+ivec2(index%FILTER_CACHE_WIDTH,index/FILTER_CACHE_WIDTH);
        bool valid=HybridInView(q);
        filterPositions[index]=valid ? texelFetch(filterPosition,q,0) : vec4(0);
        filterSurfaces[index]=valid ? texelFetch(receiverSurface,q,0).rg : uvec2(0);
        // These are already RG16 UNORM texels: packing preserves their stored precision.
        filterNormals[index]=valid ? packUnorm2x16(texelFetch(receiverNormal,q,0).rg) : 0u;
        filterVariances[index]=valid ? texelFetch(inputVariance,q,0) : vec4(0);
    }
    barrier();
}
vec4 FilterPositionData(ivec2 p) { return filterPositions[FilterCacheIndex(p)]; }
uvec2 FilterSurface(ivec2 p) { return filterSurfaces[FilterCacheIndex(p)]; }
vec3 FilterNormal(ivec2 p) { return DecodeGBufferNormal(unpackUnorm2x16(filterNormals[FilterCacheIndex(p)])); }
vec4 FilterVariance(ivec2 p) { return filterVariances[FilterCacheIndex(p)]; }
#else
vec4 FilterPositionData(ivec2 p) { return texelFetch(filterPosition,p,0); }
uvec2 FilterSurface(ivec2 p) { return texelFetch(receiverSurface,p,0).rg; }
vec3 FilterNormal(ivec2 p) { return HybridNormal(p); }
vec4 FilterVariance(ivec2 p) { return texelFetch(inputVariance,p,0); }
#endif
vec3 FilterPosition(ivec2 p) { return FilterPositionData(p).xyz; }
float FilterRoughness(ivec2 p) { return FilterPositionData(p).w; }
vec3 FilterGeometricNormal(ivec2 p) { return DecodeGBufferNormal(unpackUnorm2x16(FilterSurface(p).r)); }
vec3 SkyGuide(ivec2 p) { return texelFetch(skyGuide,p,0).rgb; }
float SurfaceWeight(ivec2 q, uint identity, vec3 position, vec3 normal, vec3 geometric)
{
    if(!HybridInView(q)) return 0.0;
    uvec2 surface=FilterSurface(q);
    if(surface.g!=identity) return 0.0;
    vec3 delta=FilterPosition(q)-position;
    if(abs(dot(delta,geometric))>hybrid.rejection.x*max(1.0,length(delta))
        || dot(geometric,DecodeGBufferNormal(unpackUnorm2x16(surface.r)))<0.9) return 0.0;
    return pow(max(dot(normal,FilterNormal(q)),0.0),32.0);
}
float SkyWeight(vec3 sampleSky, vec3 sampleGuide, vec3 center, vec3 guide, float sigma)
{
    vec3 difference=sampleSky/sampleGuide-center/guide;
    return exp(-0.5*dot(difference,difference)/(3.0*sigma*sigma));
}
