#version 460
#extension GL_GOOGLE_include_directive : require

#include "../Common/bindless_heap.glsl"

layout(location = 0) in FS_IN {
    vec4 position;
    vec2 texCoord;
} fs_in;

layout(location = 0) out vec4 outColor;

layout (push_constant) uniform uPushConstant
{
    vec2 exponents;
    uint nodeIndex;
    uint materialIndex;
    float alphaCutoff;
} pc;

struct Material
{
    int bcTexIndex;
    int mrTexIndex;
    int normalTexIndex;
    int occlusionTexIndex;
    int emissiveTexIndex;
    int bcTexSet;
    int mrTexSet;
    int normalTexSet;
    int aoTexSet;
    int emissiveTexSet;
    float metallicFactor;
    float roughnessFactor;
    vec4 baseColorFactor;
    vec4 emissiveFactor;
    vec4 surfaceProperties;
};

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
    vec4 diffuseColor = SamplerHeap2D(materialData[pc.materialIndex].bcTexIndex, 0, fs_in.texCoord);

    if (diffuseColor.a <= pc.alphaCutoff) { discard; }

    outColor = ShadowDepthToEVSM(gl_FragCoord.z);
}
