layout(location=0) in vec2 inUV;
layout(location=0) out vec4 outColor;
layout(set=1,binding=1,std140) uniform uDebugData
{
    mat4 inverseProjection;
    vec4 range;
    uvec4 selection;
} debug;

float DisplayDepth(float depth)
{
    if (debug.range.z > 0.5)
    {
        vec4 position = debug.inverseProjection * vec4(inUV * 2.0 - 1.0, depth, 1.0);
        depth = abs(position.z / position.w);
    }
    return clamp((depth - debug.range.x) / (debug.range.y - debug.range.x), 0.0, 1.0);
}

vec3 DisplaySRGB(vec3 linearColor)
{
    vec3 color = max(linearColor, vec3(0));
    return mix(1.055 * pow(color, vec3(1.0 / 2.4)) - 0.055, color * 12.92, lessThanEqual(color, vec3(0.0031308)));
}
