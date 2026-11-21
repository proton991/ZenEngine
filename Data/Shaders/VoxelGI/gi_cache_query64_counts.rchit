#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_ray_tracing : require
#define HYBRID_HARDWARE_PROVIDER
#define HYBRID_RAY_PIPELINE
#define HYBRID_RADIANCE_CACHE
#define GI_CACHE_KEY64
#define HYBRID_CLOSEST_HIT
#define HYBRID_RESTIR
#define HYBRID_COUNT_RAYS
#include "../Common/ray_pipeline.glsl"
#include "hybrid_hit.glsl"
