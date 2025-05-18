#version 460

layout(location = 0) in FS_IN {
    vec4 position;
    vec2 texCoord;
} fs_in;

layout(location = 0) out vec4 outColor;

layout (push_constant) uniform uPushConstant
{
    vec2 exponents;
    uint nodeIndex;
    uint materialIndex;
    float alphaCutoff;
} pc;

struct Material
{
    int bcTexIndex;
    int mrTexIndex;
    int normalTexIndex;
    int occlusionTexIndex;
    int emissiveTexIndex;
    int bcTexSet;
    int mrTexSet;
    int normalTexSet;
    int aoTexSet;
    int emissiveTexSet;
    float metallicFactor;
    float roughnessFactor;
    vec4 baseColorFactor;
    vec4 emissiveFactor;
};

layout(std140, set = 0, binding = 2) readonly buffer MaterialBuffer {
    Material materialData[];
};

layout (set = 1, binding = 0) uniform sampler2D uTextureArray[1024];

vec2 WarpDepth(float depth)
{
    // rescale depth into [-1, 1]
    depth = 2.0 * depth - 1.0;
    float pos =  exp(pc.exponents.x * depth);
    float neg = -exp(-pc.exponents.y * depth);

    return vec2(pos, neg);
}

vec4 ShadowDepthToEVSM(float depth)
{
    vec2 moment1 = WarpDepth(depth);
    vec2 moment2 = moment1 * moment1;

    return vec4(moment1, moment2);
}

void main()
{
    vec4 diffuseColor = texture(uTextureArray[materialData[pc.materialIndex].bcTexIndex], fs_in.texCoord);

    if (diffuseColor.a <= pc.alphaCutoff) { discard; }

    outColor = ShadowDepthToEVSM(gl_FragCoord.z);
}