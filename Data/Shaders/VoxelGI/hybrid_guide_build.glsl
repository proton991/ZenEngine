#if !defined(HYBRID_NO_GUIDE) && !defined(HYBRID_RAY_PIPELINE)
void BuildGuide(uint lane)
{
    guideBase   = (gl_WorkGroupID.y * gl_NumWorkGroups.x + gl_WorkGroupID.x) * (ZEN_HYBRID_GUIDE_TILES + 1u);
    guideValid  = hybrid.provider.y != 0u;
    float power = 0.0;
    for (uint t = lane; t < ZEN_HYBRID_GUIDE_TILES; t += TRACE_THREADS)
    {
        uint perFace      = ZEN_HYBRID_GUIDE_PER_FACE * ZEN_HYBRID_GUIDE_PER_FACE;
        guideDirection[t] = GuideCellDirection(t);
        // Rays are drawn from the guide smoothed over the neighbouring screen tiles (a 5x5 binomial
        // kernel): each tile learns from its own receivers, and sampling borrows their neighbours'
        // statistics, which are noisier per tile than they are different between nearby tiles.
        float weight = 0.0;
        if (guideValid)
        {
            for (int dy = -2; dy <= 2; ++dy)
            {
                for (int dx = -2; dx <= 2; ++dx)
                {
                    ivec2 neighbour = clamp(ivec2(gl_WorkGroupID.xy) + ivec2(dx, dy), ivec2(0), ivec2(gl_NumWorkGroups.xy) - 1);
                    uint  base = (uint(neighbour.y) * gl_NumWorkGroups.x + uint(neighbour.x)) * (ZEN_HYBRID_GUIDE_TILES + 1u);
                    const float poolKernel[5]  = float[5](1, 4, 6, 4, 1);
                    weight                    += poolKernel[dx + 2] * poolKernel[dy + 2] * guidePrevious[base + t];
                }
            }
        }
        // A cosine ray can learn radiance in a dim cell whose CDF mass rounded to zero.
        // Such a cell cannot be sampled by the guide; the cosine proposal retains its support.
        uint cdfEnd      = ZEN_HYBRID_GUIDE_CDF_OFFSET + t * (ZEN_HYBRID_GUIDE_CELL_SIZE + 1u) + ZEN_HYBRID_GUIDE_CELL_SIZE;
        guideWeight[t]   = environmentTiles[cdfEnd] > 0.0 ? weight / 256.0 : 0.0;
        power           += guideWeight[t];
        uint ratio       = ZEN_HYBRID_GUIDE_PER_FACE / ZEN_ENVIRONMENT_TILES_PER_FACE;
        uint sourceTile  = ((t / perFace) * ZEN_ENVIRONMENT_TILES_PER_FACE + (t % perFace) / ZEN_HYBRID_GUIDE_PER_FACE / ratio)
                             * ZEN_ENVIRONMENT_TILES_PER_FACE
                         + (t % ZEN_HYBRID_GUIDE_PER_FACE) / ratio;
        if (EnvironmentSourceTile(sourceTile))
        {
            guideWeight[t] = -guideWeight[t];
        }
        if (HYBRID_EXACT_GUIDE)
        {
            GuideSumClear(t);
        }
        else
        {
            guideSumWords[t] = 0u;
        }
    }
    // Separate power scratch lets the learning bins initialize with the guide,
    // removing the barrier previously needed before clearing reused storage.
    guidePartialPower[lane] = power;
    if (lane == 0u)
    {
        guideReceivers = 0u;
    }
    barrier();
    if (lane == 0u)
    {
        guidePower = 0.0;
        for (uint i = 0u; i < TRACE_THREADS; ++i)
        {
            guidePower += guidePartialPower[i];
        }
    }
    if (lane < 6u * ZEN_HYBRID_GUIDE_PER_FACE)
    {
        vec4 moment = vec4(0);
        for (uint x = 0u; x < ZEN_HYBRID_GUIDE_PER_FACE; ++x)
        {
            uint t  = lane * ZEN_HYBRID_GUIDE_PER_FACE + x;
            moment += max(guideWeight[t], 0.0) * vec4(GuideDirection(t), 1);
        }
        guideRowMoment[lane] = moment;
    }
    barrier();
}
#endif
