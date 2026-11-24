uint ProbeRecordSlot(uint index, uint ray, uint batch)
{
    return (batch * screenProbe.grid.w + index) * uint(ZEN_SCREEN_PROBE_RAYS) + ray;
}
// One six-word record per recorded sample: flags (with the geometry index in bits 8-31), then
// instance, primitive and packed barycentrics of a recorded hit, or the radiance of a resolved
// sample; then the sample direction and its probability density. Zero flags mark a sample still
// to be traced. Slots without a sample this frame are never read.
const uint kHitRecorded = 1u, kHitResolved = 2u, kHitSurface = 4u, kHitValid = 8u;
#if defined(SCREEN_PROBE_SAMPLE_HITS)
#    define SCREEN_PROBE_HITS_ACCESS writeonly
#elif defined(SCREEN_PROBE_ACCUMULATE) || defined(SCREEN_PROBE_PREFILTER_HITS)
#    define SCREEN_PROBE_HITS_ACCESS readonly
#else
#    define SCREEN_PROBE_HITS_ACCESS
#endif
layout(std430, set = 1, binding = 97) SCREEN_PROBE_HITS_ACCESS buffer ScreenProbeHits
{
    uint probeHits[];
};
uint ProbeRecordAddress(uint slot)
{
    return slot * uint(ZEN_SCREEN_PROBE_HIT_WORDS);
}
#ifndef SCREEN_PROBE_SAMPLE_HITS
// Direction and density of a recorded sample.
vec4 ProbeHitSample(uint address)
{
    return vec4(DecodeGBufferNormal(unpackUnorm2x16(probeHits[address + 4u])), uintBitsToFloat(probeHits[address + 5u]));
}
vec3 ProbeHitRadiance(uint address)
{
    return uintBitsToFloat(uvec3(probeHits[address + 1u], probeHits[address + 2u], probeHits[address + 3u]));
}
#endif
