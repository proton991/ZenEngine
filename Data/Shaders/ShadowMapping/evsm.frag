#version 460
#extension GL_GOOGLE_include_directive : require

#include "../Common/bindless_heap.glsl"
#define MATERIAL_FRAGMENT
layout(location=4) in vec4 inPackedBindingUV[11];
#include "../Common/material_binding_uv.glsl"
#include "../Common/material.glsl"
#include "../Common/material_shadow.glsl"

layout(location = 0) in FS_IN {
    vec4 color;
    vec3 worldPosition;
    vec3 lightVector;
    flat vec4 modelScaleOrientation;
} fs_in;

layout(location = 0) out vec4 outColor;

layout (push_constant) uniform uPushConstant
{
    vec2 exponents;
    uint nodeIndex;
    uint materialIndex;
    float alphaCutoff;
} pc;

layout(std140, set = 1, binding = 2) readonly buffer MaterialBuffer {
    Material materialData[];
};


vec2 WarpDepth(float depth)
{
    // rescale depth into [-1, 1]
    depth = 2.0 * depth - 1.0;
    float pos =  exp(pc.exponents.x * depth);
    float neg = -exp(-pc.exponents.y * depth);

    return vec2(pos, neg);
}

vec4 ShadowDepthToEVSM(float depth)
{
    vec2 moment1 = WarpDepth(depth);
    vec2 moment2 = moment1 * moment1;

    return vec4(moment1, moment2);
}

void main()
{
    Material material = materialData[pc.materialIndex];
    vec2 uv0 = MaterialBindingUV(0);
    vec2 uv1 = MaterialBindingUV(1);
    vec4 diffuseColor = MaterialAlbedo(material, uv0, uv1, fs_in.color);

    float coverage = MaterialShadowCoverage(material, diffuseColor, uv0, uv1,
        fs_in.worldPosition, fs_in.lightVector, fs_in.modelScaleOrientation.xyz);
    if (!MaterialVisible(material, diffuseColor.a) || (material.volumeIridescence.x > 0.0 &&
        !(gl_FrontFacing == (fs_in.modelScaleOrientation.w >= 0.0)))) discard;
    if (coverage <= MaterialShadowNoise(pc.nodeIndex, pc.materialIndex)) discard;

    outColor = ShadowDepthToEVSM(gl_FragCoord.z);
}
