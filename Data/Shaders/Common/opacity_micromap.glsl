#ifndef ZEN_OPACITY_MICROMAP_GLSL
#define ZEN_OPACITY_MICROMAP_GLSL
#ifdef ZEN_OPACITY_MICROMAP
#    extension GL_EXT_opacity_micromap_ray_query_mode : require
const bool gl_EnableOpacityMicromapEXT = true;
#endif
#endif
