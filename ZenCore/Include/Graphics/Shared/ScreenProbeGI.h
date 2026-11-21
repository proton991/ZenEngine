#ifndef ZEN_SCREEN_PROBE_CONSTANTS
#define ZEN_SCREEN_PROBE_CONSTANTS
#define ZEN_GLOSSY_RECEIVER_SPACING     4
#define ZEN_SCREEN_PROBE_SPACING        8u
#define ZEN_SCREEN_PROBE_RAYS           16
#define ZEN_SCREEN_PROBE_SPECULAR_RAYS  (ZEN_SCREEN_PROBE_RAYS * 4)
#define ZEN_SCREEN_PROBE_ADAPTIVE_TILES 4
#define ZEN_SCREEN_PROBE_HISTORY_FRAMES 4
// 64 cumulative probabilities, eight six-bit light indices and the scheduled diffuse batch count. Six words store three learned direction distributions.
#define ZEN_SCREEN_PROBE_PROPOSAL_WORDS 40u
// The broad GGX levels retain 4x4 directions: a single octahedral texel would
// erase hemisphere visibility and directional response on rough surfaces.
#define ZEN_SCREEN_PROBE_SPECULAR_TEXELS 112
// Eight lighting vectors; per signal: luminance moments, history age and ray variance;
// then the two source signatures and three incident direction distributions.
#define ZEN_SCREEN_PROBE_LIGHTING_VECTORS 12
// Each active probe receives at most this many batches, including the first frame.
// Diffuse-only probes spend the directions otherwise used for specular on diffuse lighting.
#define ZEN_SCREEN_PROBE_MATURE_BATCHES  2u
#define ZEN_SCREEN_PROBE_MAX_BATCHES     3u
#define ZEN_SCREEN_PROBE_DIFFUSE_BATCHES 5u
#define ZEN_SCREEN_PROBE_HIT_WORDS       6u
#endif
