#version 450
#extension GL_GOOGLE_include_directive : require
#define MATERIAL_FRAGMENT
#include "../Common/material.glsl"
layout(std140,set=1,binding=0) uniform uShadowFace
{
    mat4 viewProjection;
    vec4 lightPositionInvRange;
} face;
layout(std140,set=1,binding=2) readonly buffer MaterialBuffer { Material materials[]; };
layout(push_constant) uniform Constants { uint nodeIndex; uint materialIndex; } pc;
layout(location=0) in vec3 worldPosition;
layout(location=1) in vec2 uv0;
layout(location=2) in vec2 uv1;
layout(location=3) in vec4 color;
void main()
{
    Material material=materials[pc.materialIndex];
    if(!MaterialVisible(material,MaterialAlbedo(material,uv0,uv1,color).a)) discard;
    // Linear radial depth avoids the loss of distant precision from a small near plane.
    gl_FragDepth=face.lightPositionInvRange.w>0 ?
        length(worldPosition-face.lightPositionInvRange.xyz)*face.lightPositionInvRange.w : gl_FragCoord.z;
}
