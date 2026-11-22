#version 450
// Declare the complete asset::Vertex layout so reflection preserves its stride.
layout(location = 0) in vec4 inPos;
layout(location = 1) in vec4 inNormal;
layout(location = 2) in vec4 inTangent;
layout(location = 3) in vec2 inUV0;
layout(location = 4) in vec2 inUV1;
layout(location = 5) in vec4 inJoint0;
layout(location = 6) in vec4 inWeight0;
layout(location = 7) in vec4 inColor;
layout(location = 0) flat out vec4 outColor;
void main()
{
    gl_Position = inPos;
    outColor    = inColor;
}
