#version 460
#extension GL_GOOGLE_include_directive : require
#define PIPELINE_SHADOWS
#define VOXEL_GI
#define HYBRID_GI
#define LIGHTING_CAPTURE
#include "deferred_lighting.glsl"
