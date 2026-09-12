#version 450
layout(set = 0, binding = 0) writeonly buffer SharedBuffer
{
    float value;
}
sharedData;
layout(location = 0) out vec4 outputColor;
void main()
{
    sharedData.value = 1;
    outputColor      = vec4(1);
}
