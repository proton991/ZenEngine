#version 460

layout (location = 0) out vec3 outWorldPos;
layout (location = 1) out vec3 outNormal;
layout (location = 2) out vec2 outUV0;
layout (location = 3) out vec2 outUV1;
layout (location = 4) out vec4 outColor;

layout (location = 0) in vec3 inPos;
layout (location = 1) in vec3 inNormal;
layout (location = 2) in vec2 inUV0;
layout (location = 3) in vec2 inUV1;
layout (location = 4) in vec4 inJoint0;
layout (location = 5) in vec4 inWeight0;
layout (location = 6) in vec4 inColor;

layout (set = 0, binding = 0) uniform uCameraData
{
    mat4 uProjViewMatrix;
    mat4 uModelMatrix;
    vec4 uCameraPos;
};

struct NodeData
{
    mat4 modelMatrix;
    mat4 normalMatrix;
};

layout (set = 0, binding = 1) uniform uNodeData
{
    NodeData uNodeArray[4];
};

layout (push_constant) uniform uNodePushConstant
{
    uint uSubMeshIndex;
    uint uMaterialIndex;
};

void main()
{
    mat4 transformMat = uModelMatrix * uNodeArray[uSubMeshIndex].modelMatrix;
    vec4 locPos = transformMat * vec4(inPos, 1.0);
    //    locPos.y = -locPos.y;
    gl_Position = uProjViewMatrix * vec4(locPos.xyz, 1.0);
    outNormal = normalize(transpose(inverse(mat3(transformMat))) * inNormal);
    outUV0 = inUV0;
    outUV1 = inUV1;
    outColor = inColor;
    outWorldPos = locPos.xyz / locPos.w;
}
