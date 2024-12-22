#version 450

layout (set = 1, binding = 0) uniform sampler2D samplerBaseColor;

layout (location = 0) in vec3 inNormal;
layout (location = 1) in vec2 inUV;
layout (location = 2) in vec3 inColor;
layout (location = 3) in vec3 inWorldPos;

layout (location = 0) out vec4 outPosition;
layout (location = 1) out vec4 outNormal;
layout (location = 2) out vec4 outAlbedo;

vec3 GetNormal() {
    vec3 pos_dx = dFdx(inWorldPos);
    vec3 pos_dy = dFdy(inWorldPos);
    vec3 st1    = dFdx(vec3(inUV, 0.0));
    vec3 st2    = dFdy(vec3(inUV, 0.0));
    vec3 T      = (st2.t * pos_dx - st1.t * pos_dy) / (st1.s * st2.t - st2.s * st1.t);
    vec3 N      = normalize(inNormal);
    T           = normalize(T - N * dot(N, T));
    vec3 B      = normalize(cross(N, T));
    mat3 TBN    = mat3(T, B, N);

    return normalize(TBN[2].xyz);
}

void main()
{
    outPosition = vec4(inWorldPos, 1.0);

    // Calculate normal in tangent space
    vec3 tnorm = GetNormal();
    outNormal = vec4(tnorm, 1.0);

    outAlbedo = texture(samplerBaseColor, inUV);
}