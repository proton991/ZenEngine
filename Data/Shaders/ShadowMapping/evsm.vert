#version 460
#extension GL_GOOGLE_include_directive : require

#include "../Common/bindless_heap.glsl"
#include "../Common/material.glsl"

layout (location = 0) in vec4 inPos;
layout (location = 1) in vec4 inNormal;
layout (location = 2) in vec4 inTangent;
layout (location = 3) in vec2 inUV0;
layout (location = 4) in vec2 inUV1;
layout (location = 5) in vec4 inJoint0;
layout (location = 6) in vec4 inWeight0;
layout (location = 7) in vec4 inColor;

layout(location = 0) out VS_OUT {
    vec4 color;
    vec3 worldPosition;
    vec3 lightVector;
    flat vec4 modelScaleOrientation;
} vs_out;
layout(location=4) out vec4 outPackedBindingUV[11];
out gl_PerVertex { vec4 gl_Position; };

struct NodeData {
    mat4 modelMatrix;
    mat4 normalMatrix;
    vec4 surfaceScale;
};

layout (set = 1, binding = 0) uniform uLightInfo
{
    mat4 uLightViewProjection;
};

layout(std140, set = 1, binding = 1) readonly buffer NodeBuffer {
    NodeData nodesData[];
};
layout(std140,set=1,binding=2) readonly buffer MaterialBuffer { Material materials[]; };
layout(std430,set=1,binding=3) readonly buffer UVBuffer { vec4 uvValues[]; };

layout (push_constant) uniform uNodePushConstant
{
    vec2 exponents;
    uint nodeIndex;
    uint materialIndex;
    float alphaCutoff;
} pc;

void main()
{
    vec4 vertexPos = vec4(inPos.xyz, 1.0);
    mat4 model = nodesData[pc.nodeIndex].modelMatrix;
    vec4 world = model * vertexPos;
    vec4 clipPosition = uLightViewProjection * world;
    vec4 nearPosition = inverse(uLightViewProjection) * vec4(clipPosition.xy / clipPosition.w, 0.0, 1.0);
    vs_out.worldPosition = world.xyz / world.w;
    vs_out.lightVector = nearPosition.xyz / nearPosition.w - vs_out.worldPosition;
    vs_out.modelScaleOrientation = nodesData[pc.nodeIndex].surfaceScale;
    uint stride = uint(uvValues[0].x);
    Material material = materials[pc.materialIndex];
    int sets[5] = int[5](material.bcTexSet, material.mrTexSet, material.normalTexSet, material.aoTexSet, material.emissiveTexSet);
    for (int slot = 0; slot < 22; ++slot)
    {
        uint set = uint(max(slot < 5 ? sets[slot] : int(material.featureTextures[slot - 5].properties.y), 0));
        vec2 coordinates = set < stride ? uvValues[1 + uint(gl_VertexIndex) * stride + set].xy : vec2(0.0);
        if ((slot & 1) == 0) outPackedBindingUV[slot / 2].xy = coordinates;
        else outPackedBindingUV[slot / 2].zw = coordinates;
    }
    vs_out.color = inColor;
    // final drawing pos
    gl_Position = clipPosition;
}
