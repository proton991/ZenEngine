#version 450
layout(set = 0, binding = 0) readonly buffer SharedBuffer
{
    float value;
}
sharedData;
void main()
{
    gl_Position = vec4(sharedData.value, 0, 0, 1);
}
