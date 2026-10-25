#ifndef ZEN_SCREEN_PROBE_H
#define ZEN_SCREEN_PROBE_H

// Screen-probe layout shared by ScreenProbeGather.cpp and probe_common.glsl.
// Adaptive probes are placed per square subtile of this many pixels where no uniform probe
// covers a receiver, one after another until the subtile is covered or holds this many.
#define ZEN_PROBE_SUBTILE              8u
#define ZEN_PROBE_ADAPTIVE_PER_SUBTILE 2u
// A subtile places its next adaptive probe only while at least this many of its receivers are
// uncovered. Smaller gaps (edges, carvings, surface detail) take probe_integrate.comp's fallback:
// on New Sponza this removed 72% of the requests at unchanged quality (Doc/ComputeGI/Verification.md).
#define ZEN_PROBE_ADAPTIVE_MIN_UNCOVERED 16u
// Subtile map entry (one uvec2 per subtile, ZEN_PROBE_ADAPTIVE_PER_SUBTILE) without an adaptive probe.
#define ZEN_PROBE_INVALID 0xffffffffu
// A receiver whose summed uniform-probe interpolation weight is below this is uncovered.
#define ZEN_PROBE_COVERAGE 0.05
// Adaptive probe slots follow the uniform grid as extra rows. The pool starts at, and never shrinks
// below, one row per this many grid rows (one adaptive probe per uniform probe on average); it
// grows to the requested count read back from earlier frames.
#define ZEN_PROBE_ADAPTIVE_ROW_RATIO 1u
// Subtiles per workgroup of probe_allocate_count.comp and probe_allocate.comp.
#define ZEN_PROBE_ALLOCATE_GROUP 256u
// Queue entries per probe slot for rays the brick probe trace leaves to probe_retrace_bricks.comp
// (brick hits next to a lamp). A ray that finds the queue full is shaded from its brick hit.
#define ZEN_PROBE_RETRACE_RAYS_PER_SLOT 8u

#endif // ZEN_SCREEN_PROBE_H
