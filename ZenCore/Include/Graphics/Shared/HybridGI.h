#ifndef ZEN_HYBRID_GI_H
#define ZEN_HYBRID_GI_H
// Largest plane distance between a receiver and a reused GI sample, per unit of view distance
// (at least one). HybridGIUniformData::rejection.x carries it to the hybrid passes; forward
// layers that reuse the receiver's GI read it directly.
#define ZEN_HYBRID_PLANE_REJECTION 0.001
// GI samples without this many frames of history (fresh texels, reconstruction fallback
// receivers, self-traced forward layers) reach the screen almost unfiltered. Their luminance is
// bounded to ZEN_HYBRID_SAMPLE_BOUND times a local reference from receivers that have history.
#define ZEN_HYBRID_BOUNDED_HISTORY 4.0
#define ZEN_HYBRID_SAMPLE_BOUND    8.0
#endif
