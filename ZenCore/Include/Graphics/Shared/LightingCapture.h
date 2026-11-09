#ifndef ZEN_LIGHTING_CAPTURE_H
#define ZEN_LIGHTING_CAPTURE_H

// Interleaved float4 records: combined, direct, diffuse, specular, emission,
// escaped environment diffuse, bounced diffuse. Alpha is surface validity.
#define ZEN_LIGHTING_CAPTURE_COMPONENTS      10u
#define ZEN_LIGHTING_CAPTURE_BYTES_PER_PIXEL (16u * ZEN_LIGHTING_CAPTURE_COMPONENTS)
#define ZEN_LIGHTING_CAPTURE_GROUP_SIZE      64u

// Components 7..9 are unmaterialed D_direct, D_sky, D_bounce (irradiance / pi).
// The original seven components retain their meanings.
#define ZEN_HYBRID_CAPTURE_COMPONENTS      13u
#define ZEN_HYBRID_CAPTURE_BYTES_PER_PIXEL (16u * ZEN_HYBRID_CAPTURE_COMPONENTS)
#endif
