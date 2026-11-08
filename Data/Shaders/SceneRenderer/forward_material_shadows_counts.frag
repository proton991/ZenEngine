#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_ray_query : require
#include "../Common/opacity_micromap.glsl"
#define HYBRID_COUNT_RAYS
#define GI_COUNT_RAY_SLOT 3
#define HYBRID_HARDWARE_PROVIDER
#define HARDWARE_SHADOWS
#define VOXEL_GI
#define HYBRID_GI
#define RAY_FORWARD_MATERIAL
#include "forward_material.glsl"
