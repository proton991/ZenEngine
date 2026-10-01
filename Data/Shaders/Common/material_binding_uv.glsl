#ifndef ZEN_MATERIAL_BINDING_UV_GLSL
#define ZEN_MATERIAL_BINDING_UV_GLSL
// Two independently interpolated coordinates share each four-component varying.
vec2 MaterialBindingUV(int slot)
{
    vec4 coordinates = inPackedBindingUV[slot / 2];
    return (slot & 1) == 0 ? coordinates.xy : coordinates.zw;
}
#define MATERIAL_BINDING_UV
#define MATERIAL_BINDING_UV_VALUE(slot) MaterialBindingUV(slot)
#endif
