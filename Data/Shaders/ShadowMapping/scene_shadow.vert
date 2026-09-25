#version 450
#extension GL_GOOGLE_include_directive : require
#include "../Common/bindless_heap.glsl"
layout(location=0) in vec4 inPos;
// Reflection derives the packed binding stride from all declared asset::Vertex attributes.
layout(location=1) in vec4 inNormal;
layout(location=2) in vec4 inTangent;
layout(location=3) in vec2 inUV0;
layout(location=4) in vec2 inUV1;
layout(location=5) in vec4 inJoint0;
layout(location=6) in vec4 inWeight0;
layout(location=7) in vec4 inColor;
struct NodeData { mat4 modelMatrix; mat4 normalMatrix; };
layout(std140,set=1,binding=0) uniform uShadowFace
{
    mat4 viewProjection;
    vec4 lightPositionInvRange;
} face;
layout(std140,set=1,binding=1) readonly buffer NodeBuffer { NodeData nodes[]; };
layout(push_constant) uniform Constants { uint nodeIndex; uint materialIndex; } pc;
layout(location=0) out vec3 worldPosition;
layout(location=1) out vec2 uv0;
layout(location=2) out vec2 uv1;
layout(location=3) out vec4 color;
void main()
{
    vec4 position=nodes[pc.nodeIndex].modelMatrix*vec4(inPos.xyz,1);
    worldPosition=position.xyz/position.w;
    uv0=inUV0;
    uv1=inUV1;
    color=inColor;
    gl_Position=face.viewProjection*vec4(worldPosition,1);
}
