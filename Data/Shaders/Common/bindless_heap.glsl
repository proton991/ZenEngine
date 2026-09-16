#ifndef ZEN_BINDLESS_HEAP_GLSL
#define ZEN_BINDLESS_HEAP_GLSL

#extension GL_EXT_nonuniform_qualifier : require

#include "Graphics/Shared/Bindless.h"

layout(set = ZEN_BINDLESS_HEAP_SET, binding = ZEN_BINDLESS_HEAP_BINDING_TEXTURE2D) uniform texture2D uTexture2DHeap[];

layout(set = ZEN_BINDLESS_HEAP_SET, binding = ZEN_BINDLESS_HEAP_BINDING_SAMPLER) uniform sampler uSamplerHeap[];

// Keep the heap arrays runtime-sized even in stages that do not sample scene textures.
vec4 SamplerHeap2D(uint imageIdx, uint samplerIdx, vec2 uv)
{
    return texture(sampler2D(uTexture2DHeap[nonuniformEXT(imageIdx)],
                             uSamplerHeap[nonuniformEXT(samplerIdx)]), uv);
}

#endif // ZEN_BINDLESS_HEAP_GLSL
