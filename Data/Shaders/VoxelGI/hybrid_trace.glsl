#include "hybrid_guides.glsl"
#include "hybrid_sampling.glsl"
#include "hybrid_provider.glsl"
#include "hit_radiance.glsl"
#include "environment_sampling.glsl"
#include "hybrid_sky_guide.glsl"
// One workgroup per screen tile of the visibility guide (HybridGIRenderer::kGuideTile).
layout(local_size_x=16,local_size_y=16) in;
layout(set=1,binding=6) uniform sampler3D voxelOpacity;
layout(set=1,binding=7) uniform samplerCube coneEnvironmentMap;
layout(set=1,binding=8,rgba16f) uniform writeonly image2D rawSky;
layout(set=1,binding=9,rgba16f) uniform writeonly image2D rawSpecular;
layout(set=1,binding=12,rgba16f) uniform writeonly image2D rawBounce;
layout(set=1,binding=13) uniform sampler3D hitRadiance;
layout(set=1,binding=14) uniform sampler3D hitNormal;
// Base radiance level from before this frame's injection, and the bounce this frame's rays read from
// it (meaningful only when the cache changed; hybrid_temporal.comp).
layout(set=1,binding=15) uniform sampler3D hitRadiancePrevious;
layout(set=1,binding=16,rgba16f) uniform writeonly image2D rawBouncePrevious;
layout(set=1,binding=17,rg32f) uniform writeonly image2D rawReflection;
layout(set=1,binding=18,rgba16f) uniform writeonly image2D rawReflectionPrevious;
layout(set=1,binding=19,rgba16f) uniform writeonly image2D skyGuide;
// Visibility guide: per screen tile, the visible residual radiance (without the receiver's cosine)
// over the environment's tiles, learned from this pass's rays, followed by the frames it averages.
// Each receiver weights it by its own cosine toward the tile centres and draws guided rays from it,
// so its rays go where the sky is both bright and unblocked from that part of the screen.
layout(std430,set=1,binding=10) readonly buffer HybridGuidePrevious { float guidePrevious[]; };
layout(std430,set=1,binding=11) writeonly buffer HybridGuideNext { float guideNext[]; };
shared float guideWeight[ZEN_HYBRID_GUIDE_TILES];
shared vec3 guideDirection[ZEN_HYBRID_GUIDE_TILES];
shared uint guideLearned[ZEN_HYBRID_GUIDE_TILES];
shared uint guideReceivers;
shared float guidePower[256];
// Learned frames are averaged equally up to this count, then exponentially.
const float HYBRID_GUIDE_FRAMES = 64.0;

// Bits match VoxelGISettings::Feature. The hardware variant also supports voxel queries
// so each feature can select its own provider without tracing every signal twice.
const uint SKY_PROVIDER = 1u;
const uint SPECULAR_PROVIDER = 2u;
const uint BOUNCE_PROVIDER = 8u;
const uint REFLECTION_PROVIDER = 16u;
bool FeatureHardware(uint feature) { return (hybrid.provider.z & feature) != 0u; }
HybridRayResult TraceFeature(uint feature, vec3 position, vec3 normal, vec3 direction, bool closest)
{
#ifdef HYBRID_HARDWARE_PROVIDER
    if(FeatureHardware(feature))
        return HardwareTraceRay(HybridTraceOrigin(position,normal,0.0),direction,1e20,closest);
#endif
    return VoxelTraceRay(voxelOpacity,TraceOrigin(position,normal),direction,1e20);
}

void GuideLearn(uint tile, float value)
{
    uint previous = guideLearned[tile], expected;
    do
    {
        expected = previous;
        previous = atomicCompSwap(guideLearned[tile], expected, floatBitsToUint(uintBitsToFloat(expected) + value));
    } while(previous != expected);
}

float GuideCosineWeight(uint tile, vec3 normal)
{
    return max(guideWeight[tile],0.0) * max(dot(normal, guideDirection[tile]), 0.0);
}

// Guided directions are one two-dimensional warp of the ray's stratified sample, like the
// environment's own row and column sampler, so neighbouring pixels' guided rays stay stratified:
// the environment tiles are laid out as an atlas of the six faces (three by two), u.y draws an atlas
// row, u.x a tile within it, and the remaining fractions place the direction within the tile.
const uint GUIDE_ATLAS_COLUMNS = 3u * ZEN_HYBRID_GUIDE_PER_FACE;
const uint GUIDE_ATLAS_ROWS = 2u * ZEN_HYBRID_GUIDE_PER_FACE;
uint GuideAtlasTile(uint x, uint y)
{
    uint face = (y / ZEN_HYBRID_GUIDE_PER_FACE) * 3u + x / ZEN_HYBRID_GUIDE_PER_FACE;
    return (face * ZEN_HYBRID_GUIDE_PER_FACE + y % ZEN_HYBRID_GUIDE_PER_FACE) * ZEN_HYBRID_GUIDE_PER_FACE
        + x % ZEN_HYBRID_GUIDE_PER_FACE;
}

float GuideAtlasRow(uint y, vec3 normal)
{
    float sum = 0.0;
    for(uint x = 0u; x < GUIDE_ATLAS_COLUMNS; ++x) sum += GuideCosineWeight(GuideAtlasTile(x, y), normal);
    return sum;
}

// Total of the cosine-weighted guide used by GuideSample and GuidePdf.
float GuideTotal(vec3 normal, out float fullTotal)
{
    float total = 0.0; fullTotal=0.0;
    for(uint y = 0u; y < GUIDE_ATLAS_ROWS; ++y)
        for(uint x=0u;x<GUIDE_ATLAS_COLUMNS;++x)
        {
            uint tile=GuideAtlasTile(x,y);
            float cosine=max(dot(normal,guideDirection[tile]),0.0);
            total+=max(guideWeight[tile],0.0)*cosine;
            fullTotal+=abs(guideWeight[tile])*cosine;
        }
    return total;
}

vec3 GuideSample(vec3 normal, float total, vec2 u, out float pdf)
{
    float target = min(u.y * total, uintBitsToFloat(floatBitsToUint(total) - 1u)), running = 0.0, row = 0.0;
    uint y = 0u;
    for(; y + 1u < GUIDE_ATLAS_ROWS; ++y)
    {
        row = GuideAtlasRow(y, normal);
        if(running + row > target) break;
        running += row;
    }
    if(y + 1u == GUIDE_ATLAS_ROWS) row = GuideAtlasRow(y, normal);
    u.y = clamp((target - running) / max(row, 1e-30), 0.0, 0.99999994);
    target = min(u.x * row, uintBitsToFloat(floatBitsToUint(max(row, 1e-30)) - 1u));
    running = 0.0;
    float weight = 0.0;
    uint x = 0u;
    for(; x + 1u < GUIDE_ATLAS_COLUMNS; ++x)
    {
        weight = GuideCosineWeight(GuideAtlasTile(x, y), normal);
        if(running + weight > target) break;
        running += weight;
    }
    if(x + 1u == GUIDE_ATLAS_COLUMNS) weight = GuideCosineWeight(GuideAtlasTile(x, y), normal);
    u.x = clamp((target - running) / max(weight, 1e-30), 0.0, 0.99999994);
    return SampleEnvironmentTileGrid(GuideAtlasTile(x,y),u,ZEN_HYBRID_GUIDE_PER_FACE,ZEN_HYBRID_GUIDE_CDF_OFFSET,pdf);
}

void main()
{
    ivec2 p=ivec2(gl_GlobalInvocationID.xy);
    uint lane=gl_LocalInvocationIndex;
    uint guideBase=(gl_WorkGroupID.y*gl_NumWorkGroups.x+gl_WorkGroupID.x)*(ZEN_HYBRID_GUIDE_TILES+1u);
    // The guide survives resets that keep the screen and environment (HybridGIRenderer); source
    // tiles have their own rays.
    bool guideValid=hybrid.provider.y!=0u;
    float power=0.0;
    for(uint t=lane;t<ZEN_HYBRID_GUIDE_TILES;t+=gl_WorkGroupSize.x*gl_WorkGroupSize.y)
    {
        uint perFace=ZEN_HYBRID_GUIDE_PER_FACE*ZEN_HYBRID_GUIDE_PER_FACE;
        vec2 center=(vec2(t%ZEN_HYBRID_GUIDE_PER_FACE,(t%perFace)/ZEN_HYBRID_GUIDE_PER_FACE)+0.5)
            *(2.0/float(ZEN_HYBRID_GUIDE_PER_FACE))-1.0;
        guideDirection[t]=EnvironmentWorldDirection(t/perFace,center);
        // Rays are drawn from the guide smoothed over the neighbouring screen tiles (a 5x5 binomial
        // kernel): each tile learns from its own receivers, and sampling borrows their neighbours'
        // statistics, which are noisier per tile than they are different between nearby tiles.
        float weight=0.0;
        if(guideValid)
            for(int dy=-2;dy<=2;++dy)
                for(int dx=-2;dx<=2;++dx)
                {
                    ivec2 neighbour=clamp(ivec2(gl_WorkGroupID.xy)+ivec2(dx,dy),ivec2(0),ivec2(gl_NumWorkGroups.xy)-1);
                    uint base=(uint(neighbour.y)*gl_NumWorkGroups.x+uint(neighbour.x))*(ZEN_HYBRID_GUIDE_TILES+1u);
                    const float poolKernel[5]=float[5](1,4,6,4,1);
                    weight+=poolKernel[dx+2]*poolKernel[dy+2]*guidePrevious[base+t];
                }
        // A cosine ray can learn radiance in a dim cell whose CDF mass rounded to zero.
        // Such a cell cannot be sampled by the guide; the cosine proposal retains its support.
        uint cdfEnd=ZEN_HYBRID_GUIDE_CDF_OFFSET+t*(ZEN_HYBRID_GUIDE_CELL_SIZE+1u)+ZEN_HYBRID_GUIDE_CELL_SIZE;
        guideWeight[t]=environmentTiles[cdfEnd]>0.0 ? weight/256.0 : 0.0;
        power+=guideWeight[t];
        uint ratio=ZEN_HYBRID_GUIDE_PER_FACE/ZEN_ENVIRONMENT_TILES_PER_FACE;
        uint sourceTile=((t/perFace)*ZEN_ENVIRONMENT_TILES_PER_FACE+(t%perFace)/ZEN_HYBRID_GUIDE_PER_FACE/ratio)
            *ZEN_ENVIRONMENT_TILES_PER_FACE+(t%ZEN_HYBRID_GUIDE_PER_FACE)/ratio;
        if(EnvironmentSourceTile(sourceTile)) guideWeight[t]=-guideWeight[t];
        guideLearned[t]=0u;
    }
    guidePower[lane]=power;
    if(lane==0u) guideReceivers=0u;
    barrier();
    if(lane==0u)
        for(uint i=1u;i<256u;++i) guidePower[0]+=guidePower[i];
    barrier();

    vec4 sky=vec4(0), specular=vec4(0,0,0,1), bounce=vec4(0), previous=vec4(0);
    vec2 reflection=vec2(0);
    vec3 reflectionPrevious=vec3(0);
    vec3 reconstructionGuide=vec3(1);
    if(HybridInView(p) && texelFetch(receiverDepth,p,0).r<1.0 && texelFetch(receiverSurface,p,0).g!=0u)
    {
        atomicAdd(guideReceivers,1u);
        vec3 position=HybridPosition(p), n=HybridNormal(p), ng=HybridGeometricNormal(p);
        float scale=sceneUbo.environment.x*sceneUbo.environment.z*gi.lighting.y;
        float fullGuideTotal;
        float guideTotal=GuideTotal(n,fullGuideTotal);
        reconstructionGuide=HybridSkyGuide(n);
        if(guideValid)
        {
            if(guidePower[0]>0.0 && fullGuideTotal>0.0)
                reconstructionGuide=max(reconstructionGuide/HybridLuminance(reconstructionGuide)
                    *(fullGuideTotal/guidePower[0]),vec3(1e-3));
        }
        // Shares of guided, residual-environment and cosine rays; without a guide its share goes to
        // the environment. Every density below is the mixture of these three proposals.
        vec3 share=guideTotal>0.0 ? vec3(0.75,0.0,0.25) : vec3(0.0,HYBRID_ENVIRONMENT_SHARE,1.0-HYBRID_ENVIRONMENT_SHARE);
        bool residualEnvironment=environmentResidualRows[ZEN_ENVIRONMENT_IMPORTANCE_ROWS]>0.0
            && gi.lighting.y*sceneUbo.environment.x*sceneUbo.environment.z>0.0;
        if(!residualEnvironment) share=vec3(0,0,1);
        for(uint i=0u;i<hybrid.sampling.y;++i)
        {
            // Rays from the same proposal share one temporal Sobol net. The pixel's digital shift
            // preserves spatial stratification while each proposal advances by its own ray count.
            vec2 u=HybridSkySample(uvec2(p),hybrid.sampling.x,0x1000u+i);
            // A deterministic split for multiples of four rays, else a stratified choice per ray.
            float choice=hybrid.sampling.y%4u==0u ? (float(i)+0.5)/float(hybrid.sampling.y) : u.x;
            uint proposal=choice<share.x ? 0u : choice<share.x+share.y ? 1u : 2u;
            if(hybrid.sampling.y%4u==0u)
            {
                uint first=uint(float(hybrid.sampling.y)*(proposal==0u ? 0.0 : proposal==1u ? share.x : share.x+share.y));
                uint count=uint(float(hybrid.sampling.y)*share[proposal]);
                u=HybridSkySample(uvec2(p),hybrid.sampling.x*count+i-first,0x1000u+proposal);
            }
            if(hybrid.sampling.y%4u!=0u)
                u.x=min(proposal==0u ? u.x/share.x : proposal==1u ? (u.x-share.x)/share.y : (u.x-share.x-share.y)/share.z,0.99999994);
            vec3 w;
            float pdf;
            if(proposal==0u)
                w=GuideSample(n,guideTotal,u,pdf);
            else if(proposal==1u)
                w=SampleEnvironmentDistribution(u,true,pdf);
            else
                w=HybridCosineSample(n,u,pdf);
            // Mixture density of the direction under all three proposals.
            uint face; vec2 uv,st; uvec2 cell;
            EnvironmentLocate(w,face,uv,cell,st);
            uint size=ZEN_ENVIRONMENT_IMPORTANCE_SIZE/ZEN_HYBRID_GUIDE_PER_FACE;
            uint tile=(face*ZEN_HYBRID_GUIDE_PER_FACE+cell.y/size)*ZEN_HYBRID_GUIDE_PER_FACE+cell.x/size;
            float tilePDF=EnvironmentCellInTilePDF(tile,face*ZEN_ENVIRONMENT_IMPORTANCE_SIZE+cell.y,cell.x,uv,st,
                ZEN_HYBRID_GUIDE_CELL_SIZE,ZEN_HYBRID_GUIDE_CDF_OFFSET);
            bool source=EnvironmentSourceTile(EnvironmentTileOf(face,cell));
            float guided=guideTotal>0.0 ? GuideCosineWeight(tile,n)/guideTotal*tilePDF : 0.0;
            float residual=source || !residualEnvironment ? 0.0 : EnvironmentDistributionPDF(w,true);
            pdf=share.x*guided+share.y*residual+share.z*max(dot(n,w),0.0)/HYBRID_PI;
            bool inDomain=pdf>0.0 && dot(n,w)>0.0 && dot(ng,w)>0.0;
            bool sharedBounce=FeatureHardware(SKY_PROVIDER)==FeatureHardware(BOUNCE_PROVIDER);
            HybridRayResult hit=HybridRayResult(false,position,vec3(0),ivec3(-1));
            if(inDomain)
                hit=TraceFeature(SKY_PROVIDER,position,ng,w,hybrid.bounce.x!=0u && sharedBounce);
            bool escaped=inDomain && !hit.hit;
            if(inDomain && hybrid.bounce.x!=0u && !sharedBounce)
                hit=TraceFeature(BOUNCE_PROVIDER,position,ng,w,true);
            if(inDomain && hit.hit && hybrid.bounce.x!=0u)
            {
                bool rejected;
                float weight=max(dot(n,w),0.0)/(HYBRID_PI*pdf);
                vec3 value=HitRadiance(hitRadiance,hitNormal,hit,w,rejected)*weight;
                bounce.rgb+=value;
                bounce.a+=rejected ? 1.0:0.0;
                // The same hit in the previous cache: these correlated lookups measure the change
                // with little noise when lighting changes a little.
                if(hybrid.bounce.w!=0u)
                    previous.rgb+=HitRadiance(hitRadiancePrevious,hitNormal,hit,w,rejected)*weight;
            }
            if(escaped)
            {
                float weight=max(dot(n,w),0.0)/(HYBRID_PI*pdf);
                if(!source)
                {
                    vec3 radiance=textureLod(coneEnvironmentMap,EnvironmentDirection(w),0).rgb;
                    sky.rgb+=radiance*weight*scale;
                    // The guide learns visible radiance without this receiver's cosine.
                    GuideLearn(tile,HybridLuminance(radiance)/(HYBRID_PI*pdf*float(hybrid.sampling.y)));
                }
                // Alpha estimates the cosine-weighted visible fraction over all directions.
                sky.a+=weight;
            }
        }
        sky/=float(hybrid.sampling.y);
        bounce.rgb/=float(hybrid.sampling.y);
        previous.rgb/=float(hybrid.sampling.y);
        // Each source tile gets its own rays, half the residual count (two at the shipping preset),
        // so sources are integrated without selection noise between them. Their directions are
        // coherent across pixels, so they cost far less than residual rays (P4Execution.md).
        uint sourceRays=max(hybrid.sampling.y/2u,1u);
        for(uint k=0u;k<environmentSourceCount;++k)
            for(uint j=0u;j<sourceRays;++j)
            {
                float pdf;
                vec2 u=HybridSkySample(uvec2(p),hybrid.sampling.x*sourceRays+j,0x8000u+k*0x1000u);
                vec3 w=SampleEnvironmentTile(environmentSourceTiles[k],u,pdf);
                if(pdf>0.0 && dot(n,w)>0.0 && dot(ng,w)>0.0 && !TraceFeature(SKY_PROVIDER,position,ng,w,false).hit)
                {
                    vec3 radiance=textureLod(coneEnvironmentMap,EnvironmentDirection(w),0).rgb;
                    sky.rgb+=radiance*max(dot(n,w),0.0)/(HYBRID_PI*pdf*float(sourceRays))*scale;
                    uint face; vec2 uv,st; uvec2 cell;
                    EnvironmentLocate(w,face,uv,cell,st);
                    uint tile=(face*ZEN_HYBRID_GUIDE_PER_FACE+cell.y/ZEN_HYBRID_GUIDE_CELL_SIZE)*ZEN_HYBRID_GUIDE_PER_FACE
                        +cell.x/ZEN_HYBRID_GUIDE_CELL_SIZE;
                    GuideLearn(tile,HybridLuminance(radiance)/(HYBRID_PI*pdf*float(sourceRays)));
                }
            }
        if(hybrid.rejection.y>0.0)
        {
            vec3 view=sceneUbo.viewPosition.w<0.5 ? normalize(sceneUbo.lightInfo.yzw) : normalize(sceneUbo.viewPosition.xyz-position);
            float roughness=clamp(texelFetch(receiverRoughness,p,0).g,0.04,1.0);
            uint specularSamples=hybrid.sampling.y>4u ? hybrid.sampling.y : 1u;
            specular.a=0.0;
            float hitCount=0.0;
            for(uint i=0u;i<specularSamples;++i)
            {
                vec3 w=HybridSpecularDirection(n,view,roughness,uvec2(p),hybrid.sampling.x*specularSamples+i,0x1234u);
                if(dot(ng,w)>0.0)
                {
                    bool sharedReflection=FeatureHardware(SPECULAR_PROVIDER)==FeatureHardware(REFLECTION_PROVIDER);
                    HybridRayResult hit=TraceFeature(SPECULAR_PROVIDER,position,ng,w,hybrid.reflection.x!=0u && sharedReflection);
                    if(!hit.hit) specular.a+=1.0;
                    if(hybrid.reflection.x!=0u && !sharedReflection)
                        hit=TraceFeature(REFLECTION_PROVIDER,position,ng,w,true);
                    if(hit.hit && hybrid.reflection.x!=0u)
                    {
                        bool rejected;
                        // Store the unconditional hit integral C=(1-S)*H. Averaging conditional H
                        // separately from the hit fraction would multiply two noisy estimates.
                        specular.rgb+=HitRadiance(hitRadiance,hitNormal,hit,w,rejected);
                        reflection.y+=rejected ? 1.0 : 0.0;
                        reflection.x+=length(hit.position-position);
                        hitCount+=1.0;
                        if(hybrid.reflection.w!=0u)
                            reflectionPrevious+=HitRadiance(hitRadiancePrevious,hitNormal,hit,w,rejected);
                    }
                }
            }
            specular/=float(specularSamples);
            reflectionPrevious/=float(specularSamples);
            reflection.x/=max(hitCount,1.0);
        }
    }
    if(HybridInView(p))
    {
        imageStore(rawSky,p,sky);
        imageStore(rawSpecular,p,specular);
        imageStore(skyGuide,p,vec4(reconstructionGuide,0));
        if(hybrid.reflection.x!=0u)
        {
            imageStore(rawReflection,p,vec4(reflection,0,0));
            imageStore(rawReflectionPrevious,p,vec4(reflectionPrevious,0));
        }
        if(hybrid.bounce.x!=0u) // 1x1 placeholders for cone bounce
        {
            imageStore(rawBounce,p,bounce);
            imageStore(rawBouncePrevious,p,previous);
        }
    }
    barrier();
    // Average this frame into the guide: equally over the first frames, then exponentially.
    float frames=guideValid ? guidePrevious[guideBase+ZEN_HYBRID_GUIDE_TILES] : 0.0;
    float alpha=1.0/min(frames+1.0,HYBRID_GUIDE_FRAMES);
    for(uint t=lane;t<ZEN_HYBRID_GUIDE_TILES;t+=gl_WorkGroupSize.x*gl_WorkGroupSize.y)
    {
        float learned=guideReceivers>0u ? uintBitsToFloat(guideLearned[t])/float(guideReceivers) : 0.0;
        float previous=guideValid ? guidePrevious[guideBase+t] : 0.0;
        guideNext[guideBase+t]=mix(previous,learned,alpha);
    }
    if(lane==0u) guideNext[guideBase+ZEN_HYBRID_GUIDE_TILES]=min(frames+1.0,HYBRID_GUIDE_FRAMES);
}
