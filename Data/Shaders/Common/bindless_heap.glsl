#ifndef ZEN_BINDLESS_HEAP_GLSL
#define ZEN_BINDLESS_HEAP_GLSL

#extension GL_EXT_nonuniform_qualifier : require

#include "Graphics/Shared/Bindless.h"

layout(set = ZEN_BINDLESS_HEAP_SET, binding = ZEN_BINDLESS_HEAP_BINDING_TEXTURE2D) uniform texture2D uTexture2DHeap[];

layout(set = ZEN_BINDLESS_HEAP_SET, binding = ZEN_BINDLESS_HEAP_BINDING_SAMPLER) uniform sampler uSamplerHeap[];

// Keep the heap arrays runtime-sized even in stages that do not sample scene textures.
vec4 SamplerHeap2D(uint imageIdx, uint samplerIdx, vec2 uv)
{
    return texture(sampler2D(uTexture2DHeap[nonuniformEXT(imageIdx)], uSamplerHeap[nonuniformEXT(samplerIdx)]), uv);
}

#ifdef ZEN_BINDLESS_2D_ARRAY
layout(set     = ZEN_BINDLESS_HEAP_SET,
       binding = ZEN_BINDLESS_HEAP_BINDING_TEXTURE2DARRAY) uniform texture2DArray uTexture2DArrayHeap[];

vec4 SamplerHeap2DArray(uint imageIdx, uint samplerIdx, vec3 uvLayer)
{
    return texture(sampler2DArray(uTexture2DArrayHeap[nonuniformEXT(imageIdx)], uSamplerHeap[nonuniformEXT(samplerIdx)]),
                   uvLayer);
}
#endif

// Opt in only for shaders targeting devices with imageCubeArray enabled.
#ifdef ZEN_BINDLESS_CUBE_ARRAY
layout(set     = ZEN_BINDLESS_HEAP_SET,
       binding = ZEN_BINDLESS_HEAP_BINDING_TEXTURECUBEARRAY) uniform textureCubeArray uTextureCubeArrayHeap[];

vec4 SamplerHeapCubeArray(uint imageIdx, uint samplerIdx, vec4 directionCube)
{
    return texture(samplerCubeArray(uTextureCubeArrayHeap[nonuniformEXT(imageIdx)], uSamplerHeap[nonuniformEXT(samplerIdx)]),
                   directionCube);
}
#endif

#endif // ZEN_BINDLESS_HEAP_GLSL
