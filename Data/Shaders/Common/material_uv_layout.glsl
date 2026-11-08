#ifndef ZEN_MATERIAL_UV_LAYOUT_GLSL
#define ZEN_MATERIAL_UV_LAYOUT_GLSL
// Zero retains per-binding UVs for arbitrary authored sets. Scenes with two
// sets specialize both stages to one vec4, shared by every texture binding.
layout(constant_id = 7) const uint materialUVSetCount = 0u;
const uint materialUVVectorCount                      = materialUVSetCount == 0u ? 11u : (materialUVSetCount + 1u) / 2u;
#endif
