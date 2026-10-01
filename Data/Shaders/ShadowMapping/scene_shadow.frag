#version 450
#extension GL_GOOGLE_include_directive : require
#define MATERIAL_FRAGMENT
layout(location=3) in vec4 inPackedBindingUV[11];
layout(location=2) flat in vec4 modelScaleOrientation;
#include "../Common/material_binding_uv.glsl"
#include "../Common/material.glsl"
#include "../Common/material_shadow.glsl"
layout(std140,set=1,binding=0) uniform uShadowFace
{
    mat4 viewProjection;
    vec4 lightPositionInvRange;
} face;
layout(std140,set=1,binding=2) readonly buffer MaterialBuffer { Material materials[]; };
layout(push_constant) uniform Constants { uint nodeIndex; uint materialIndex; } pc;
layout(location=0) in vec3 worldPosition;
layout(location=1) in vec4 color;
void main()
{
    Material material=materials[pc.materialIndex];
    vec2 uv0 = MaterialBindingUV(0);
    vec2 uv1 = MaterialBindingUV(1);
    vec4 albedo = MaterialAlbedo(material, uv0, uv1, color);
    vec3 lightVector = face.lightPositionInvRange.w > 0.0 ?
        face.lightPositionInvRange.xyz - worldPosition : face.lightPositionInvRange.xyz;
    float coverage = MaterialShadowCoverage(material, albedo, uv0, uv1, worldPosition, lightVector, modelScaleOrientation.xyz);
    if (!MaterialVisible(material, albedo.a) || (material.volumeIridescence.x > 0.0 &&
        !(gl_FrontFacing == (modelScaleOrientation.w >= 0.0)))) discard;
    if (coverage <= MaterialShadowNoise(pc.nodeIndex, pc.materialIndex)) discard;
    // Linear radial depth avoids the loss of distant precision from a small near plane.
    gl_FragDepth=face.lightPositionInvRange.w>0 ?
        length(worldPosition-face.lightPositionInvRange.xyz)*face.lightPositionInvRange.w : gl_FragCoord.z;
}
