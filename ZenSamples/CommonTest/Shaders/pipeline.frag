#version 450
layout(constant_id = 0) const bool enabled = true;
layout(constant_id = 1) const int integerValue = -7;
layout(constant_id = 2) const float floatValue = 1.25;
layout(location = 0) out vec4 color;
void main()
{
    color = vec4(enabled ? float(integerValue + 8) : 0, 0, 0, floatValue * 0.2);
}
