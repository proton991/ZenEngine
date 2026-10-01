#version 450
#extension GL_GOOGLE_include_directive : require
#include "../Common/bindless_heap.glsl"
#include "../Common/material.glsl"
layout(location=0) in vec4 inPos;
// Reflection derives the packed binding stride from all declared asset::Vertex attributes.
layout(location=1) in vec4 inNormal;
layout(location=2) in vec4 inTangent;
layout(location=3) in vec2 inUV0;
layout(location=4) in vec2 inUV1;
layout(location=5) in vec4 inJoint0;
layout(location=6) in vec4 inWeight0;
layout(location=7) in vec4 inColor;
struct NodeData { mat4 modelMatrix; mat4 normalMatrix; vec4 surfaceScale; };
layout(std140,set=1,binding=0) uniform uShadowFace
{
    mat4 viewProjection;
    vec4 lightPositionInvRange;
} face;
layout(std140,set=1,binding=1) readonly buffer NodeBuffer { NodeData nodes[]; };
layout(std140,set=1,binding=2) readonly buffer MaterialBuffer { Material materials[]; };
layout(std430,set=1,binding=3) readonly buffer UVBuffer { vec4 uvValues[]; };
layout(push_constant) uniform Constants { uint nodeIndex; uint materialIndex; } pc;
layout(location=0) out vec3 worldPosition;
layout(location=1) out vec4 color;
layout(location=2) flat out vec4 modelScaleOrientation;
layout(location=3) out vec4 outPackedBindingUV[11];
out gl_PerVertex { vec4 gl_Position; };
void main()
{
    vec4 position=nodes[pc.nodeIndex].modelMatrix*vec4(inPos.xyz,1);
    worldPosition=position.xyz/position.w;
    color=inColor;
    gl_Position=face.viewProjection*vec4(worldPosition,1);
    Material material = materials[pc.materialIndex];
    int sets[5] = int[5](material.bcTexSet, material.mrTexSet, material.normalTexSet, material.aoTexSet, material.emissiveTexSet);
    uint stride = uint(uvValues[0].x);
    for (int slot = 0; slot < 22; ++slot)
    {
        uint set = uint(max(slot < 5 ? sets[slot] : int(material.featureTextures[slot - 5].properties.y), 0));
        vec2 coordinates = stride > set ? uvValues[1 + uint(gl_VertexIndex) * stride + set].xy : vec2(0.0);
        if ((slot & 1) == 0) outPackedBindingUV[slot / 2].xy = coordinates;
        else outPackedBindingUV[slot / 2].zw = coordinates;
    }
    modelScaleOrientation = nodes[pc.nodeIndex].surfaceScale;
}
