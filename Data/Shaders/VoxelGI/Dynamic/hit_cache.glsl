// Compact DDA records preserve the full 24-bit cell, two-bit class/status and
// unmodified float distance. Surface attributes are resolved from the current grid.
// The decoded layout remains available for the independent reference provider.
#ifdef GI_CACHE_WRITE
layout(set=3,binding=3,std430) writeonly buffer StaticHits { uvec2 cachedWords[]; };
void StoreCachedHit(uint index,GIHit hit) {
    if(giWork.cache.x!=0u) {
        uint token=(hit.identity.z&0xffffffu)|(hit.identity.y<<24u)|(hit.identity.x<<26u);
        cachedWords[index]=uvec2(token,floatBitsToUint(hit.positionDistance.w));
    } else {
        uint first=index*12u;
        uvec4 words[6]=uvec4[6](floatBitsToUint(hit.positionDistance),floatBitsToUint(hit.normal),
            floatBitsToUint(hit.baseColorMetallic),floatBitsToUint(hit.emission),
            floatBitsToUint(hit.diffuseReflectance),hit.identity);
        for(uint i=0u;i<6u;++i) {
            cachedWords[first+i*2u]=words[i].xy;
            cachedWords[first+i*2u+1u]=words[i].zw;
        }
    }
}
#else
layout(set=3,binding=3,std430) readonly buffer StaticHits { uvec2 cachedWords[]; };
GIHit LoadCachedHit(uint index) {
    GIHit hit=EmptyGIHit(GI_UNKNOWN);
    if(giWork.cache.x!=0u) {
        uvec2 words=cachedWords[index];
        uint status=(words.x>>26u)&3u;
        hit=EmptyGIHit(status);
        if(status==GI_HIT) {
            float distance=uintBitsToFloat(words.y);
            // Gathering needs cached distance for class ordering. Reconstruct
            // position only when an analytic sender has no mesh lighting point.
            hit.positionDistance.w=distance;
            hit.identity=uvec4(status,(words.x>>24u)&3u,words.x&0xffffffu,GI_CELL_PRECISION);
        }
    } else {
        uint first=index*12u;
        hit.positionDistance=uintBitsToFloat(uvec4(cachedWords[first],cachedWords[first+1u]));
        hit.normal=uintBitsToFloat(uvec4(cachedWords[first+2u],cachedWords[first+3u]));
        hit.baseColorMetallic=uintBitsToFloat(uvec4(cachedWords[first+4u],cachedWords[first+5u]));
        hit.emission=uintBitsToFloat(uvec4(cachedWords[first+6u],cachedWords[first+7u]));
        hit.diffuseReflectance=uintBitsToFloat(uvec4(cachedWords[first+8u],cachedWords[first+9u]));
        hit.identity=uvec4(cachedWords[first+10u],cachedWords[first+11u]);
    }
    return ResolveHitSurface(hit);
}
#endif
