#include "../Common/gbuffer.glsl"
layout(std140,set=1,binding=0) uniform uHybridData
{
    mat4 inverseViewProjection;
    vec4 worldOrigin;
    mat4 previousProjectionView;
    vec4 previousViewPosition;
    vec4 previousViewDirection;
    uvec4 sampling;
    uvec4 provider;
    vec4 rejection;
    uvec4 bounce;
} hybrid;
layout(set=1,binding=1) uniform sampler2D receiverDepth;
layout(set=1,binding=2) uniform sampler2D receiverNormal;
layout(set=1,binding=3) uniform sampler2D receiverRoughness;
layout(set=1,binding=4) uniform usampler2D receiverSurface;
layout(set=1,binding=5) uniform sampler2D receiverMotion;
layout(set=1,binding=24) uniform sampler2D receiverPosition;
vec3 HybridPosition(ivec2 pixel)
{
    return hybrid.provider.x!=0u ? texelFetch(receiverPosition,pixel,0).xyz
        : ReconstructGBufferPosition(vec2(pixel)+0.5,textureSize(receiverDepth,0),texelFetch(receiverDepth,pixel,0).r,
            hybrid.inverseViewProjection)+hybrid.worldOrigin.xyz;
}
vec3 HybridNormal(ivec2 pixel) { return DecodeGBufferNormal(texelFetch(receiverNormal,pixel,0).rg); }
vec3 HybridGeometricNormal(ivec2 pixel) { return DecodeGBufferNormal(unpackUnorm2x16(texelFetch(receiverSurface,pixel,0).r)); }
float HybridLuminance(vec3 rgb) { return dot(rgb,vec3(0.2126,0.7152,0.0722)); }
bool HybridInView(ivec2 pixel) { return all(greaterThanEqual(pixel,ivec2(0))) && all(lessThan(pixel,textureSize(receiverDepth,0))); }
