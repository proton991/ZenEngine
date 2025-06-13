#version 450
#extension GL_ARB_separate_shader_objects : enable

layout(location = 0) in GS_OUT {
    vec3 wsPosition;
    vec2 texCoord;
    vec3 position;
    vec3 normal;
    vec3 tangent;
    vec4 triangleAABB;
    uint faxis;
} fs_in;

layout(set = 1, binding = 0, r32ui) uniform volatile coherent uimage3D voxelAlbedo;
layout(set = 1, binding = 1, r32ui) uniform volatile coherent uimage3D voxelNormal;
layout(set = 1, binding = 2, r32ui) uniform volatile coherent uimage3D voxelEmissive;
layout(set = 1, binding = 3, r8) uniform volatile coherent image3D staticVoxelFlag;

layout (set = 2, binding = 0) uniform sampler2D uTextureArray[1024];

layout (location = 0) out vec4 outFragColor;

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

layout (push_constant) uniform uNodePushConstant
{
    uint nodeIndex;
    uint materialIndex;
    uint flagStaticVoxels;
    uint volumeDimension;
} pc;

vec3 EncodeNormal(vec3 normal)
{
    return normal * 0.5f + vec3(0.5f);
}

vec4 convRGBA8ToVec4(uint val)
{
    return vec4(float((val & 0x000000FF)), 
    float((val & 0x0000FF00) >> 8U), 
    float((val & 0x00FF0000) >> 16U), 
    float((val & 0xFF000000) >> 24U));
}

uint convVec4ToRGBA8(vec4 val)
{
    return (uint(val.w) & 0x000000FF) << 24U | 
    (uint(val.z) & 0x000000FF) << 16U | 
    (uint(val.y) & 0x000000FF) << 8U | 
    (uint(val.x) & 0x000000FF);
}

#define IMAGE_ATOMIC_RGBA8_AVG(IMAGE, COORDS, VALUE)                          \
{                                                                             \
    vec4 _val = VALUE;                                                        \
    _val.rgb *= 255.0; /* optimize following calculations */                  \
    uint _newVal = convVec4ToRGBA8(_val);                                     \
    uint _prevStoredVal = 0;                                                 \
    uint _curStoredVal;                                                      \
    uint _numIterations = 0;                                                 \
                                                                              \
    while ((_curStoredVal = imageAtomicCompSwap(IMAGE, COORDS, _prevStoredVal, _newVal)) \
            != _prevStoredVal && _numIterations < 255)                        \
    {                                                                         \
        _prevStoredVal = _curStoredVal;                                       \
        vec4 _rval = convRGBA8ToVec4(_curStoredVal);                          \
        _rval.rgb *= _rval.a; /* Denormalize */                               \
        vec4 _curValF = _rval + _val; /* Add */                               \
        _curValF.rgb /= _curValF.a; /* Renormalize */                         \
        _newVal = convVec4ToRGBA8(_curValF);                                  \
        ++_numIterations;                                                     \
    }                                                                         \
}

void imageAtomicRGBA8AvgAlbedo(ivec3 coords, vec4 value)
{
    value.rgb *= 255.0;                 // optimize following calculations
    uint newVal = convVec4ToRGBA8(value);
    uint prevStoredVal = 0;
    uint curStoredVal;
    uint numIterations = 0;

    while((curStoredVal = imageAtomicCompSwap(voxelAlbedo, coords, prevStoredVal, newVal))
            != prevStoredVal
            && numIterations < 255)
    {
        prevStoredVal = curStoredVal;
        vec4 rval = convRGBA8ToVec4(curStoredVal);
        rval.rgb = (rval.rgb * rval.a); // Denormalize
        vec4 curValF = rval + value;    // Add
        curValF.rgb /= curValF.a;       // Renormalize
        newVal = convVec4ToRGBA8(curValF);

        ++numIterations;
    }
}

void imageAtomicRGBA8AvgNormal(ivec3 coords, vec4 value)
{
    value.rgb *= 255.0;                 // optimize following calculations
    uint newVal = convVec4ToRGBA8(value);
    uint prevStoredVal = 0;
    uint curStoredVal;
    uint numIterations = 0;

    while((curStoredVal = imageAtomicCompSwap(voxelNormal, coords, prevStoredVal, newVal))
    != prevStoredVal
    && numIterations < 255)
    {
        prevStoredVal = curStoredVal;
        vec4 rval = convRGBA8ToVec4(curStoredVal);
        rval.rgb = (rval.rgb * rval.a); // Denormalize
        vec4 curValF = rval + value;    // Add
        curValF.rgb /= curValF.a;       // Renormalize
        newVal = convVec4ToRGBA8(curValF);

        ++numIterations;
    }
}

void imageAtomicRGBA8AvgEmissive(ivec3 coords, vec4 value)
{
    value.rgb *= 255.0;                 // optimize following calculations
    uint newVal = convVec4ToRGBA8(value);
    uint prevStoredVal = 0;
    uint curStoredVal;
    uint numIterations = 0;

    while((curStoredVal = imageAtomicCompSwap(voxelEmissive, coords, prevStoredVal, newVal))
    != prevStoredVal
    && numIterations < 255)
    {
        prevStoredVal = curStoredVal;
        vec4 rval = convRGBA8ToVec4(curStoredVal);
        rval.rgb = (rval.rgb * rval.a); // Denormalize
        vec4 curValF = rval + value;    // Add
        curValF.rgb /= curValF.a;       // Renormalize
        newVal = convVec4ToRGBA8(curValF);

        ++numIterations;
    }
}

void main() {
//    if (fs_in.position.x < fs_in.triangleAABB.x || fs_in.position.y < fs_in.triangleAABB.y ||
//    fs_in.position.x > fs_in.triangleAABB.z || fs_in.position.y > fs_in.triangleAABB.w)
//    {
//        discard;
//    }
    Material surfMat = materialData[pc.materialIndex];
    vec4 albedoColor = texture(uTextureArray[surfMat.bcTexIndex], fs_in.texCoord);
    vec4 normal = vec4(EncodeNormal(normalize(fs_in.normal)), 1.0f);     // bring normal to 0-1 range
    vec4 emissive = texture(uTextureArray[surfMat.emissiveTexIndex], fs_in.texCoord);

    //alpha clip
    if (albedoColor.w == 0)
    {
        discard;
    }

    ivec3 texCoord3d = ivec3(fs_in.wsPosition);

    // voxel albedo
    IMAGE_ATOMIC_RGBA8_AVG(voxelAlbedo, texCoord3d, albedoColor);
    // voxel normal
    IMAGE_ATOMIC_RGBA8_AVG(voxelNormal, texCoord3d, normal);
    // voxel emissive
    IMAGE_ATOMIC_RGBA8_AVG(voxelEmissive, texCoord3d, emissive);

    outFragColor = vec4(albedoColor.xyz, 1.0);
}