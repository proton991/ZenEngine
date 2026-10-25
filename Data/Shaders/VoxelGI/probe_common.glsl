#ifndef ZEN_PROBE_COMMON
#define ZEN_PROBE_COMMON
#include "hybrid_guides.glsl"
#include "hybrid_sampling.glsl"
#include "../Common/scene_lighting.glsl"
#include "Graphics/Shared/ScreenProbe.h"

// Set 2 belongs to scene lighting; sets 3--6 hold GI, traversal and material inputs.
layout(std140, set = 7, binding = 0) uniform uProbeData
{
    uvec4 grid;       // uniform probe counts x/y, pixel spacing, valid history
    vec4  parameters; // plane threshold, history limit, responsive radiance, frames since history restarted
    uvec4 adaptive;   // subtile counts x/y, adaptive probe rows after the uniform grid, history slots
}
probe;

// 208 bytes, matching ScreenProbeGather. SH order: Y00, Y1x, Y1y, Y1z.
// L1 irradiance weighs incident light by 1/4 + cos/2 instead of cos, which biases even a plane
// under a non-uniform sky. Each signal therefore also keeps its exact cosine-weighted irradiance
// along the probe normal, divided by pi; SH supplies only the change toward other normals.
struct ProbeRecord
{
    vec4 position;     // world position, valid receiver
    vec4 normal;       // geometric normal, sky history frames
    vec4 screen;       // chosen pixel, receiver identity bits, bounce history frames
    vec4 sky[4];       // RGB incoming radiance; alpha visibility
    vec4 bounce[4];    // RGB incoming hit radiance; alpha rejected-hit fraction
    vec4 skyAnchor;    // irradiance / pi along the normal; alpha cosine-weighted visibility
    vec4 bounceAnchor; // irradiance / pi along the normal; alpha cosine-weighted rejected hits
};

// Where a probe traces from: the leading fields of ProbeRecord (48 bytes).
struct ProbePlacement
{
    vec4 position;
    vec4 normal;
    vec4 screen;
};

ProbeRecord ProbeEmpty()
{
    ProbeRecord result;
    result.position = vec4(0);
    result.normal   = vec4(0);
    result.screen   = vec4(0);
    for (int coefficient = 0; coefficient < 4; ++coefficient)
    {
        result.sky[coefficient]    = vec4(0);
        result.bounce[coefficient] = vec4(0);
    }
    result.skyAnchor    = vec4(0);
    result.bounceAnchor = vec4(0);
    return result;
}

bool ProbeValidReceiver(ivec2 pixel)
{
    return HybridInView(pixel) && texelFetch(receiverDepth, pixel, 0).r < 1.0 && texelFetch(receiverSurface, pixel, 0).g != 0u;
}

// Whether the previous frame's probes are valid history for this frame.
bool ProbeHistoryValid()
{
    return probe.grid.w != 0u && hybrid.sampling.w != 0u && hybrid.rejection.z > 0.0;
}

// Batches of 64 rays a probe traces this frame, from its sky history in batches. A probe with a
// short history traces more, so content entering the view converges in fewer frames; the temporal
// pass counts each batch as one frame of history. Placements carry the count in screen.w. New
// uniform probes trace the most (probe_place.comp); new adaptive probes trace one, since they are
// numerous and re-chosen often while the camera moves: four batches for them too doubled the trace
// while moving for little gain.
// Only a probe clearly younger than the view traces more: one on content that entered it since the
// probe history restarted (probe.parameters.w frames ago). Right after a restart (startup, a camera
// cut) every probe is as young as the view, and extra batches for all of them made the trace about
// four times as long for two frames (glTF Sponza, 1600x900: 4.2-4.5 ms against 1.1 ms). Without
// valid history every probe starts over and traces one batch, so geometry changing every frame,
// which restarts history every frame, adds nothing either.
float ProbeRayBatches(float history)
{
    bool younger = ProbeHistoryValid() && history + 2.0 < probe.parameters.w;
    return !younger ? 1.0 : history < 8.0 ? 4.0 : history < 24.0 ? 2.0 : 1.0;
}

ProbePlacement ProbePlaceAt(ivec2 pixel)
{
    ProbePlacement result;
    result.position = vec4(HybridPosition(pixel), 1);
    result.normal   = vec4(HybridGeometricNormal(pixel), 1);
    result.screen   = vec4(vec2(pixel), uintBitsToFloat(texelFetch(receiverSurface, pixel, 0).g), 1);
    return result;
}

bool ProbeGridInside(ivec2 coordinate)
{
    return all(greaterThanEqual(coordinate, ivec2(0))) && all(lessThan(coordinate, ivec2(probe.grid.xy)));
}

uint ProbeIndex(ivec2 coordinate)
{
    return uint(coordinate.y) * probe.grid.x + uint(coordinate.x);
}

// Uniform probes, then adaptive slots in extra rows of the same width.
uint ProbeUniformCount()
{
    return probe.grid.x * probe.grid.y;
}

uint ProbeSlotCount()
{
    return probe.grid.x * (probe.grid.y + probe.adaptive.z);
}

// Slots of the previous frame's probes. After the adaptive pool was resized it differs from
// ProbeSlotCount() for one frame.
uint ProbeHistorySlotCount()
{
    return probe.adaptive.w;
}

bool ProbeSubtileInside(ivec2 subtile)
{
    return all(greaterThanEqual(subtile, ivec2(0))) && all(lessThan(subtile, ivec2(probe.adaptive.xy)));
}

uint ProbeSubtileIndex(ivec2 subtile)
{
    return uint(subtile.y) * probe.adaptive.x + uint(subtile.x);
}

vec4 ProbeSHBasis(vec3 direction)
{
    return vec4(0.2820947918, 0.4886025119 * direction);
}

// World size of one pixel at a receiver, from the view's reconstruction; zero where undefined.
float ProbePixelFootprint(ivec2 pixel)
{
    ivec2 size   = textureSize(receiverDepth, 0);
    float depth  = texelFetch(receiverDepth, pixel, 0).r;
    vec3  a      = ReconstructGBufferPosition(vec2(pixel) + 0.5, size, depth, hybrid.inverseViewProjection);
    vec3  b      = ReconstructGBufferPosition(vec2(pixel) + vec2(1.5, 0.5), size, depth, hybrid.inverseViewProjection);
    float result = length(b - a);
    return isnan(result) || isinf(result) ? 0.0 : result;
}

// Whether a probe can represent a receiver: both points on one smooth surface. The plane test
// rejects either plane's separation beyond a camera-scaled base tolerance or a quarter of the
// points' distance, which accepts everything the normal test accepts on a smooth convex surface.
// The distance counts at most two probe tiles of the receiver's pixel footprint: beyond that the
// separation is mostly depth, and a parallel surface (a table top in front of the floor) must not
// pass. A zero footprint leaves the distance uncapped.
float ProbeSurfaceWeight(vec3 position, vec3 normal, float footprint, vec4 otherPosition, vec4 otherNormal)
{
    vec3  delta     = otherPosition.xyz - position;
    float plane     = max(abs(dot(delta, normal)), abs(dot(delta, otherNormal.xyz)));
    float alignment = dot(normal, otherNormal.xyz);
    float base      = max(probe.parameters.x, 1e-6) * max(1.0, length(position - sceneUbo.viewPosition.xyz));
    float reach     = footprint > 0.0 ? min(length(delta), 2.0 * float(probe.grid.z) * footprint) : length(delta);
    float threshold = max(base, 0.25 * reach);
    float weight    = 0.0;
    if (otherPosition.w > 0.5 && alignment >= 0.9 && plane <= threshold)
    {
        weight = pow(max(alignment, 0.0), 8.0) * exp(-plane / threshold);
    }
    return weight;
}

float ProbeSurfaceWeight(vec3 position, vec3 normal, float footprint, ProbeRecord other)
{
    return ProbeSurfaceWeight(position, normal, footprint, other.position, other.normal);
}

// Where a pixel's surface point was last frame, in pixels; the pixel itself without valid motion.
vec2 ProbePreviousPixel(vec2 pixel)
{
    ivec2 size   = textureSize(receiverDepth, 0);
    vec4  motion = texelFetch(receiverMotion, clamp(ivec2(pixel), ivec2(0), size - 1), 0);
    return motion.z > 0.0 ? pixel + motion.xy * 0.5 * vec2(size) : pixel;
}

// The pixel a previous probe's world position projects to this frame, if the receiver there is
// still the probe's surface rather than an occluder in front of it. A kept probe stays on its
// surface while the camera moves, with its own history (probe_temporal.comp). projectionView is
// inverse(hybrid.inverseViewProjection), which maps positions relative to hybrid.worldOrigin.
bool ProbeReproject(mat4 projectionView, vec4 position, vec4 normal, float identity, out ivec2 pixel)
{
    vec4 clip   = projectionView * vec4(position.xyz - hybrid.worldOrigin.xyz, 1);
    bool result = false;
    pixel       = ivec2(-1);
    if (position.w > 0.5 && clip.w > 0.0)
    {
        vec2 screen = (clip.xy / clip.w * 0.5 + 0.5) * vec2(textureSize(receiverDepth, 0)) - 0.5;
        pixel       = ivec2(round(screen));
        result      = ProbeValidReceiver(pixel) && texelFetch(receiverSurface, pixel, 0).g == floatBitsToUint(identity)
                   && ProbeSurfaceWeight(HybridPosition(pixel), HybridGeometricNormal(pixel), ProbePixelFootprint(pixel), position,
                                         normal)
                          > 0.0;
    }
    return result;
}

// A kept probe's placement: its exact previous position and normal at this frame's pixel. The
// negative normal.w names the history slot for probe_temporal.comp, which restores it; normal.w of
// the previous record is its sky history.
ProbePlacement ProbeKeptPlacement(uint slot, vec4 position, vec4 normal, float identity, ivec2 pixel)
{
    ProbePlacement result;
    result.position = vec4(position.xyz, 1);
    result.normal   = vec4(normal.xyz, -float(slot) - 1.0);
    result.screen   = vec4(vec2(pixel), identity, ProbeRayBatches(normal.w));
    return result;
}

// Visibility against geometry closer than one probe tile, which sparse probes cannot resolve
// (contact shadow at the foot of a column). Eight receivers on a spiral over the tile, rotated per
// pixel and frame, occlude by how far they rise above the receiver's plane and fade with distance.
// One on open surfaces; zero footprint disables it.
float ProbeContactVisibility(ivec2 pixel, vec3 position, vec3 geometric, float footprint)
{
    float radius   = float(probe.grid.z) * footprint;
    float occluded = 0.0;
    if (radius > 0.0)
    {
        uint  seed  = HybridHash(HybridMorton(uvec2(pixel)) ^ HybridHash(hybrid.sampling.x));
        float angle = float(seed >> 8) / 16777216.0 * 2.0 * HYBRID_PI;
        for (uint i = 0u; i < 8u; ++i)
        {
            float distancePixels = sqrt((float(i) + 0.5) / 8.0) * float(probe.grid.z);
            float phi            = angle + float(i) * 2.39996323;
            ivec2 sampleValue    = pixel + ivec2(round(distancePixels * vec2(cos(phi), sin(phi))));
            if (ProbeValidReceiver(sampleValue))
            {
                vec3  toward = HybridPosition(sampleValue) - position;
                float d      = length(toward);
                if (d > 1e-6 && d < radius)
                {
                    occluded += max(dot(toward, geometric) / d - 0.1, 0.0) / 0.9 * (1.0 - d / radius);
                }
            }
        }
    }
    return clamp(1.0 - occluded / 8.0, 0.0, 1.0);
}

// A probe's stratified uniform-hemisphere direction for one lane (stratum) and ray batch; each batch
// jitters its strata anew, seeded by the probe's pixel and the frame.
vec3 ProbeRayDirection(vec4 screen, vec3 normal, uint lane, uint batch)
{
    vec2  jitter  = HybridSample(uvec2(screen.xy), hybrid.sampling.x, 0x48454d49u + lane + 64u * batch);
    vec2  stratum = (vec2(lane & 7u, lane >> 3u) + jitter) / 8.0;
    float cosine  = stratum.y;
    float sine    = sqrt(max(1.0 - cosine * cosine, 0.0));
    float phi     = 2.0 * HYBRID_PI * stratum.x;
    return HybridBasis(normal) * vec3(sine * cos(phi), sine * sin(phi), cosine);
}

// Screen falloff of a probe's interpolation weight, in probe spacings.
float ProbeScreenWeight(vec2 probePixel, vec2 pixel, float sharpness)
{
    vec2 delta = (probePixel - pixel) / float(probe.grid.z);
    return exp(-sharpness * dot(delta, delta));
}

// Lambert convolution divided by pi: L0 multiplier 1, L1 multiplier 2/3.
vec4 ProbeIrradiance(vec4 coefficients[4], vec3 normal)
{
    vec4 basis = ProbeSHBasis(normal);
    return coefficients[0] * basis.x
         + (2.0 / 3.0) * (coefficients[1] * basis.y + coefficients[2] * basis.z + coefficients[3] * basis.w);
}

// Irradiance / pi toward a normal: the exact anchor along the probe normal plus the L1 change.
vec4 ProbeSkyIrradiance(ProbeRecord record, vec3 normal)
{
    return record.skyAnchor + ProbeIrradiance(record.sky, normal) - ProbeIrradiance(record.sky, record.normal.xyz);
}

vec4 ProbeBounceIrradiance(ProbeRecord record, vec3 normal)
{
    return record.bounceAnchor + ProbeIrradiance(record.bounce, normal) - ProbeIrradiance(record.bounce, record.normal.xyz);
}
#endif
