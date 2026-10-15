#version 460
#extension GL_GOOGLE_include_directive : require
#define SOFTWARE_SHADOWS
#define VOXEL_GI
#define RAY_FORWARD_MATERIAL
#include "forward_scatter.glsl"
