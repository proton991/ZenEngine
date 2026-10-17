#version 450
layout(set = 1, binding = 0) uniform Duplicate { vec4 value; } vertexData;
void main() { gl_Position = vertexData.value; }
