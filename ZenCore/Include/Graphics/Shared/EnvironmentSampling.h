#ifndef ZEN_ENVIRONMENT_SAMPLING_H
#define ZEN_ENVIRONMENT_SAMPLING_H

// Matches the maximum generated prefiltered environment face resolution.
#define ZEN_ENVIRONMENT_IMPORTANCE_SIZE 512u
#define ZEN_ENVIRONMENT_IMPORTANCE_ROWS (6u * ZEN_ENVIRONMENT_IMPORTANCE_SIZE)
// Order-2 spherical-harmonic projection of the same environment: one partial sum of nine
// RGB coefficients (float4 each) per 64-row column group, followed by the total.
#define ZEN_ENVIRONMENT_HARMONIC_COEFFICIENTS 9u
#define ZEN_ENVIRONMENT_HARMONIC_GROUPS       (ZEN_ENVIRONMENT_IMPORTANCE_ROWS / 64u)

#endif
