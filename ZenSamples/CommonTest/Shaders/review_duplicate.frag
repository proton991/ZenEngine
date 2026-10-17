#version 450
layout(set = 1, binding = 1) uniform Duplicate { vec4 value; } fragmentData;
layout(location = 0) out vec4 color;
void main() { color = fragmentData.value; }
