#version 460
#extension GL_GOOGLE_include_directive : require
#define SOFTWARE_SHADOWS
#define VOXEL_GI
#define HYBRID_GI
#define LIGHTING_CAPTURE
#define RECEIVER_LIGHT_VISIBILITY
#include "deferred_lighting.glsl"
