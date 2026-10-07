#version 450
#extension GL_GOOGLE_include_directive : require
#include "../Common/material_surface.glsl"
#include "../Common/material_lighting.glsl"
#define HYBRID_RECEIVER
#include "offscreen_surface.glsl"
