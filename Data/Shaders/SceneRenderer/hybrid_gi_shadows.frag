#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_ray_query : require
#include "../Common/opacity_micromap.glsl"
#define HYBRID_HARDWARE_PROVIDER
#define HARDWARE_SHADOWS
#define VOXEL_GI
#define HYBRID_GI
#include "deferred_lighting.glsl"
