#ifndef ZEN_HYBRID_RAY_COUNTS
#define ZEN_HYBRID_RAY_COUNTS
// Only diagnostic variants count rays. Production shaders have no counters or
// extra register/shared-memory pressure. Slots: sky, specular, bounce, reflection.
#ifdef HYBRID_COUNT_RAYS
uvec4 issuedComputeRays = uvec4(0), issuedHardwareRays = uvec4(0);
uint  issuedRayFeature = 0u;
#endif
#endif
