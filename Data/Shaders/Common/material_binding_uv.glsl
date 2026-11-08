#ifndef ZEN_MATERIAL_BINDING_UV_GLSL
#define ZEN_MATERIAL_BINDING_UV_GLSL
// Two independently interpolated coordinates share each four-component varying.
vec2 MaterialBindingUV(int slot)
{
    vec4 coordinates = inPackedBindingUV[slot / 2];
    return (slot & 1) == 0 ? coordinates.xy : coordinates.zw;
}
#define MATERIAL_BINDING_UV
vec2 MaterialBindingUV(int slot, int uvSet)
{
    vec2 coordinates;
#ifdef ZEN_MATERIAL_UV_LAYOUT_GLSL
    if (materialUVSetCount != 0u)
    {
        uint set    = uint(max(uvSet, 0));
        coordinates = set < materialUVSetCount ? MaterialBindingUV(int(set)) : vec2(0.0);
    }
    else
#endif
    {
        coordinates = MaterialBindingUV(slot);
    }
    return coordinates;
}
#define MATERIAL_BINDING_UV_VALUE(slot, uvSet) MaterialBindingUV(slot, uvSet)
#endif
