#version 450

// Every asset::Vertex attribute is declared, in order, so reflection derives the scene
// vertex buffer's stride. The preview reads only position and normal.
layout(location = 0) in vec4 inPos;
layout(location = 1) in vec4 inNormal;
layout(location = 2) in vec4 inTangent;
layout(location = 3) in vec2 inUV0;
layout(location = 4) in vec2 inUV1;
layout(location = 5) in vec4 inJoint0;
layout(location = 6) in vec4 inWeight0;
layout(location = 7) in vec4 inColor;

// Shared with mesh_preview.frag. Positions stay in the mesh's vertex space.
layout(push_constant) uniform MeshPreviewConstants
{
    mat4 viewProjection;
    vec4 eye;
    uint mode;
} constants;

layout(location = 0) out vec3 outPosition;
layout(location = 1) out vec3 outNormal;

void main()
{
    outPosition = inPos.xyz;
    outNormal   = inNormal.xyz;
    gl_Position = constants.viewProjection * vec4(inPos.xyz, 1.0);
}
