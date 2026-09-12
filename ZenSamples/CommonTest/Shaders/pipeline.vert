#version 450
layout(constant_id = 0) const bool enabled = true;
layout(constant_id = 1) const int integerValue = -7;
void main()
{
    const vec2 positions[3] = vec2[](vec2(-1, -1), vec2(3, -1), vec2(-1, 3));
    float depth = enabled ? float(integerValue + 7) * 0.01 : 0.5;
    gl_Position = vec4(positions[gl_VertexIndex], depth, 1);
}
