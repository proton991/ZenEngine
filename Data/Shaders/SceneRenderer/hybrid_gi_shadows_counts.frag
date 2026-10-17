#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_ray_query : require
#define HYBRID_COUNT_RAYS
#define GI_COUNT_RAY_SLOT 2
#define HYBRID_HARDWARE_PROVIDER
#define HARDWARE_SHADOWS
#define VOXEL_GI
#define HYBRID_GI
#include "deferred_lighting.glsl"
