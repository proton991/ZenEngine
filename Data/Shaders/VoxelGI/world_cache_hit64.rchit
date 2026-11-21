#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_ray_tracing : require
#define GI_CACHE_KEY64
#define NATIVE_WORLD_CACHE
#define NATIVE_CACHE_MISS_TRANSPORT
#define WRC_LIGHTING_WRITE
#define HYBRID_HARDWARE_PROVIDER
#define HYBRID_RAY_PIPELINE
#define HYBRID_CLOSEST_HIT
#define HYBRID_RESTIR
#include "../Common/ray_pipeline.glsl"
#include "hybrid_hit.glsl"
