#ifndef ZEN_LIGHTING_CAPTURE_H
#define ZEN_LIGHTING_CAPTURE_H

// Interleaved float4 records: combined, direct, diffuse, specular, emission,
// escaped environment diffuse, bounced diffuse. Alpha is surface validity.
#define ZEN_LIGHTING_CAPTURE_COMPONENTS      7u
#define ZEN_LIGHTING_CAPTURE_BYTES_PER_PIXEL (16u * ZEN_LIGHTING_CAPTURE_COMPONENTS)
#define ZEN_LIGHTING_CAPTURE_GROUP_SIZE      64u

// M3 diagnostic tuples: position, shading normal, geometric normal,
// uint-bitcast instance/class/native texel XY, albedo/metallic, AO/roughness/ready/0.
#define ZEN_SURFACE_CAPTURE_COMPONENTS      6u
#define ZEN_SURFACE_CAPTURE_BYTES_PER_PIXEL (16u * ZEN_SURFACE_CAPTURE_COMPONENTS)

#endif
