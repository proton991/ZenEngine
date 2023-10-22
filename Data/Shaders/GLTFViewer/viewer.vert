#version 460

layout(location = 1) out vec3 outColor;
layout(location = 2) out vec2 outUV;

layout (location = 0) in vec3 inPos;
layout (location = 1) in vec3 inNormal;
layout (location = 2) in vec2 inUV0;
layout (location = 3) in vec2 inUV1;
layout (location = 4) in vec4 inJoint0;
layout (location = 5) in vec4 inWeight0;
layout (location = 6) in vec4 inColor;

layout(set = 0, binding = 0) uniform uCameraData
{
    mat4 uProjViewMatrix;
};
layout(set = 0, binding = 1) uniform uNodeData
{
    mat4 uLocalMatrix[64];
};

layout(push_constant) uniform uNodeConstant
{
    uint uNodeIndex;
};

void main()
{
    vec4 locPos = uLocalMatrix[uNodeIndex] * vec4(inPos, 1.0);
    locPos.y = -locPos.y;
    gl_Position = uProjViewMatrix * vec4(locPos.xyz/locPos.w, 1.0);
    //    gl_Position = vec4(inPos, 1.0);
    outUV = inUV0;
    outColor = vec3(inColor.r, inColor.g, inColor.a);
}
