#version 460
#extension GL_GOOGLE_include_directive : require
#define HYBRID_COUNT_RAYS
#define GI_COUNT_RAY_SLOT 3
#define SOFTWARE_SHADOWS
#define VOXEL_GI
#define RAY_FORWARD_MATERIAL
#include "forward_scatter.glsl"
