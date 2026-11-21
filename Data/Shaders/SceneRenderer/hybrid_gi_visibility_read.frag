#version 460
#extension GL_GOOGLE_include_directive : require
#define PIPELINE_SHADOWS
#define VOXEL_GI
#define HYBRID_GI
#include "deferred_lighting.glsl"
