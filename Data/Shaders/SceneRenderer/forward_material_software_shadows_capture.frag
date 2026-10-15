#version 460
#extension GL_GOOGLE_include_directive : require
#define SOFTWARE_SHADOWS
#define VOXEL_GI
#define HYBRID_GI
#define RAY_FORWARD_MATERIAL
#define LIGHTING_CAPTURE
#include "forward_material.glsl"
