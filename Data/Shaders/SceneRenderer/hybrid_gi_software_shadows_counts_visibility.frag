#version 460
#extension GL_GOOGLE_include_directive : require
#define HYBRID_COUNT_RAYS
#define GI_COUNT_RAY_SLOT 2
#define SOFTWARE_SHADOWS
#define VOXEL_GI
#define HYBRID_GI
#define RECEIVER_LIGHT_VISIBILITY
#include "deferred_lighting.glsl"
