// Exact floor(pixelCenter / viewportExtent * gbufferExtent), avoiding an
// interpolated UV rounding below an integer boundary at noninteger upscales.
layout(set=1,binding=14,std140) uniform uSurfaceLookup { uvec4 surfaceExtent; };
ivec2 SurfaceTexel(uvec2 pixel,uvec2 viewport,ivec2 extent)
{
    uvec2 texel=((2u*pixel+1u)*uvec2(extent))/(2u*viewport);
    return clamp(ivec2(texel),ivec2(0),extent-1);
}
bool ValidSurface(float depth,uvec2 identity,vec4 position,vec4 normal,vec4 geometric)
{
    return depth<0.9999 && identity.x!=0u && (identity.y==GI_STATIC || identity.y==GI_DYNAMIC) &&
        position.w>0 && normal.w>0 && geometric.w>0 &&
        !any(isnan(position)) && !any(isinf(position)) && dot(normal.xyz,normal.xyz)>1e-8;
}
