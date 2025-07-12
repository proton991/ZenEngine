#version 460

layout (location = 0) in vec4 inPos;
layout (location = 1) in vec4 inNormal;
layout (location = 2) in vec4 inTangent;
layout (location = 3) in vec2 inUV0;
layout (location = 4) in vec2 inUV1;
layout (location = 5) in vec4 inJoint0;
layout (location = 6) in vec4 inWeight0;
layout (location = 7) in vec4 inColor;

layout(location = 0) out VS_OUT {
    vec4 position;
    vec2 texCoord;
} vs_out;

struct NodeData {
    mat4 modelMatrix;
    mat4 normalMatrix;
};

layout (set = 0, binding = 0) uniform uLightInfo
{
    mat4 uLightViewProjection;
};

layout(std140, set = 0, binding = 1) readonly buffer NodeBuffer {
    NodeData nodesData[];
};

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
    vs_out.position = uLightViewProjection * nodesData[pc.nodeIndex].modelMatrix * vertexPos;
    vs_out.texCoord = inUV0.xy;
    // final drawing pos
    gl_Position = vs_out.position;
}