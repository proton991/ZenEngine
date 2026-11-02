#version 450
#extension GL_GOOGLE_include_directive : require
#include "../Common/material_surface.glsl"
#include "../Common/material_lighting.glsl"
#define HYBRID_RECEIVER
#define GBUFFER_ALPHA_MASK
#include "offscreen_surface.glsl"
