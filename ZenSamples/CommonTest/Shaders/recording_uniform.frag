#version 450
layout(set = 0, binding = 0) uniform Parameters { vec4 value; } parameters;
layout(location = 0) out vec4 color;
void main() { color = parameters.value; }
