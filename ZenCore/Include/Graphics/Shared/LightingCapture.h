#ifndef ZEN_LIGHTING_CAPTURE_H
#define ZEN_LIGHTING_CAPTURE_H

// Interleaved float4 records: combined, direct, diffuse, specular, emission,
// escaped environment diffuse, bounced diffuse. Alpha is surface validity.
#define ZEN_LIGHTING_CAPTURE_COMPONENTS      10u
#define ZEN_LIGHTING_CAPTURE_BYTES_PER_PIXEL (16u * ZEN_LIGHTING_CAPTURE_COMPONENTS)
#define ZEN_LIGHTING_CAPTURE_GROUP_SIZE      64u

// Components 7..9 are unmaterialed D_direct, D_sky, D_bounce (irradiance / pi).
// The original seven components retain their meanings.
// 13..17: optional denoiser slow/fast age, reprojection, clamp sigmas, repair source.
// Cache captures store sampled hits/misses, scheduled cells and frame in 13;
// overflow samples, dropped requests and queued requests in 14; irradiance/pi and validity in 15,
// anchor position and update count in 16, and anchor shading normal and cell size in 17.
#define ZEN_HYBRID_CAPTURE_COMPONENTS      18u
#define ZEN_HYBRID_CAPTURE_BYTES_PER_PIXEL (16u * ZEN_HYBRID_CAPTURE_COMPONENTS)
#endif
