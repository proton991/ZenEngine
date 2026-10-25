#ifndef ZEN_PROBE_CLAIM
#define ZEN_PROBE_CLAIM
// Uniform tiles and the previous probes they keep, shared by probe_claim.comp and probe_place.comp.
// The including shader declares ProbeHistory (probeHistory) after probe_common.glsl.

// probe_claim.comp's entry for a tile with receivers but no kept probe; a tile without receivers
// is ZEN_PROBE_INVALID, and a tile that keeps a previous probe holds its history slot.
#define ZEN_PROBE_CLAIM_FREE 0xfffffffeu

ivec2 ProbeTileOrigin(ivec2 tile)
{
    return tile * int(probe.grid.z);
}

ivec2 ProbeTileExtent(ivec2 tile)
{
    return min(ivec2(probe.grid.z), textureSize(receiverDepth, 0) - ProbeTileOrigin(tile));
}

vec2 ProbeTileCenter(ivec2 tile)
{
    return vec2(ProbeTileOrigin(tile)) + 0.5 * vec2(ProbeTileExtent(tile) - 1);
}

// The first receiver among a tile's eight candidate pixels, or (-1, -1). Candidates are fixed per
// tile rather than per frame: a probe that moved every frame changed every receiver's interpolation
// every frame, which the short pixel history could not hide (on Sponza, 6.5% of pixels changed sky
// light by more than 2% per frame, against 0.5% with fixed candidates).
ivec2 ProbeCandidate(ivec2 tile)
{
    ivec2 result = ivec2(-1);
    for (uint candidate = 0u; candidate < 8u && result.x < 0; ++candidate)
    {
        vec2  sampleValue = HybridSample(uvec2(tile), candidate, 0x504c4143u);
        ivec2 pixel       = ProbeTileOrigin(tile) + ivec2(sampleValue * vec2(ProbeTileExtent(tile)));
        if (ProbeValidReceiver(pixel))
        {
            result = pixel;
        }
    }
    return result;
}

// The previous uniform probe a tile keeps: probes now in the tile were last frame around where the
// tile centre's motion points. The longest history wins, then the probe nearest the tile centre.
uint ProbeKeptSlot(ivec2 tile)
{
    uint result = ZEN_PROBE_INVALID;
    if (ProbeHistoryValid())
    {
        mat4  projectionView = inverse(hybrid.inverseViewProjection);
        ivec2 origin         = ProbeTileOrigin(tile);
        ivec2 extent         = ProbeTileExtent(tile);
        vec2  center         = ProbeTileCenter(tile);
        ivec2 previousTile   = ivec2(floor(ProbePreviousPixel(center) / float(probe.grid.z)));
        float bestLength     = 0.0;
        float bestDistance   = 0.0;
        for (int y = -2; y <= 2; ++y)
        {
            for (int x = -2; x <= 2; ++x)
            {
                ivec2 coordinate = previousTile + ivec2(x, y);
                uint  slot       = ProbeGridInside(coordinate) ? ProbeIndex(coordinate) : ZEN_PROBE_INVALID;
                if (slot < ProbeHistorySlotCount())
                {
                    vec4  normal = probeHistory[slot].normal;
                    ivec2 pixel;
                    if (ProbeReproject(projectionView, probeHistory[slot].position, normal, probeHistory[slot].screen.z, pixel)
                        && all(greaterThanEqual(pixel, origin)) && all(lessThan(pixel, origin + extent))
                        && (result == ZEN_PROBE_INVALID || normal.w > bestLength
                            || (normal.w == bestLength && distance(vec2(pixel), center) < bestDistance)))
                    {
                        result       = slot;
                        bestLength   = normal.w;
                        bestDistance = distance(vec2(pixel), center);
                    }
                }
            }
        }
    }
    return result;
}
#endif
