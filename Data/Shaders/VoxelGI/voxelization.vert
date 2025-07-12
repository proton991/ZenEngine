#version 450

layout (location = 0) in vec4 inPos;
layout (location = 1) in vec4 inNormal;
layout (location = 2) in vec4 inTangent;
layout (location = 3) in vec2 inUV0;
layout (location = 4) in vec2 inUV1;
layout (location = 5) in vec4 inJoint0;
layout (location = 6) in vec4 inWeight0;
layout (location = 7) in vec4 inColor;

layout(location = 0) out VS_OUT {
    vec3 color;
    vec3 tangent;
    vec3 normal;
    vec2 texCoord;
} vs_out;

// scene graph node data
struct NodeData {
    mat4 modelMatrix;
    mat4 normalMatrix;
};

layout(std140, set = 0, binding = 0) readonly buffer NodeBuffer {
    NodeData nodesData[];
};

layout (push_constant) uniform uNodePushConstant
{
    uint nodeIndex;
    uint materialIndex;
    uint flagStaticVoxels;
    uint volumeDimension;
} pc;

void main()
{
    gl_Position = nodesData[pc.nodeIndex].modelMatrix * vec4(inPos.xyz, 1.0);

    // Normal in world space
    mat3 mNormal = mat3(nodesData[pc.nodeIndex].normalMatrix);
    vs_out.normal = (nodesData[pc.nodeIndex].normalMatrix * vec4(inNormal.xyz, 0.0f)).xyz;
    vs_out.texCoord = inUV0;
    vs_out.color = inColor.xyz;
    vs_out.tangent = inTangent.xyz;
}