#ifndef ZEN_RAY_PIPELINE
#define ZEN_RAY_PIPELINE
// Traversal helpers for raygen and closest-hit; only raygen calls the reorder helpers.
#if defined(ZEN_INVOCATION_REORDER_EXT)
#    extension GL_EXT_shader_invocation_reorder : require
#    define ZenHitObject  hitObjectEXT
#    define ZenTraceHit   hitObjectTraceRayEXT
#    define ZenReorderHit reorderThreadEXT
#    define ZenExecuteHit hitObjectExecuteShaderEXT
#    define ZenHitIsHit   hitObjectIsHitEXT
#elif defined(ZEN_INVOCATION_REORDER_NV)
#    extension GL_NV_shader_invocation_reorder : require
#    define ZenHitObject  hitObjectNV
#    define ZenTraceHit   hitObjectTraceRayNV
#    define ZenReorderHit reorderThreadNV
#    define ZenExecuteHit hitObjectExecuteShaderNV
#    define ZenHitIsHit   hitObjectIsHitNV
#endif
#if (defined(ZEN_INVOCATION_REORDER_EXT) || defined(ZEN_INVOCATION_REORDER_NV)) && !defined(ZEN_NO_PRIMARY_REORDER)
#    define ZenTraceRay(scene, flags, mask, offset, stride, miss, origin, minimum, direction, maximum, payload)       \
        {                                                                                                             \
            ZenHitObject hit;                                                                                         \
            ZenTraceHit(hit, scene, flags, mask, offset, stride, miss, origin, minimum, direction, maximum, payload); \
            ZenReorderHit(hit);                                                                                       \
            ZenExecuteHit(hit, payload);                                                                              \
        }
#else
#    define ZenTraceRay traceRayEXT
#endif
#endif
