#version 450
layout(location=0) in vec3 inPosition;
layout(push_constant) uniform BoundsCamera { mat4 projection; };
void main() { gl_Position = projection * vec4(inPosition, 1.0); }
