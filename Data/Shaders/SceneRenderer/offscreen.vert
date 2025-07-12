#version 450

layout (location = 0) in vec4 inPos;
layout (location = 1) in vec4 inNormal;
layout (location = 2) in vec4 inTangent;
layout (location = 3) in vec2 inUV0;
layout (location = 4) in vec2 inUV1;
layout (location = 5) in vec4 inJoint0;
layout (location = 6) in vec4 inWeight0;
layout (location = 7) in vec4 inColor;

layout(set = 0, binding = 0) uniform uCameraData
{
    mat4 uProjViewMatrix;
    mat4 uProjMatrix;
    mat4 uViewMatrix;
};

// scene graph node data
struct NodeData {
    mat4 modelMatrix;
    mat4 normalMatrix;
};

layout(std140, set = 0, binding = 1) readonly buffer NodeBuffer {
    NodeData nodesData[];
};

layout (push_constant) uniform uNodePushConstant
{
    uint uNodeIndex;
    uint uMaterialIndex;
};

layout (location = 0) out vec3 outNormal;
layout (location = 1) out vec2 outUV;
layout (location = 2) out vec3 outColor;
layout (location = 3) out vec3 outWorldPos;

void main()
{
    vec4 locPos = nodesData[uNodeIndex].modelMatrix * vec4(inPos.xyz, 1.0);

    gl_Position = uProjViewMatrix * vec4(locPos.xyz, 1.0);

    outUV = inUV0;

    // Vertex position in world space
    outWorldPos = locPos.xyz / locPos.w;

    // Normal in world space
    mat3 mNormal = mat3(nodesData[uNodeIndex].normalMatrix);
    outNormal = mNormal * normalize(inNormal.xyz);

    // Currently just vertex color
    outColor = inColor.rgb;
}
