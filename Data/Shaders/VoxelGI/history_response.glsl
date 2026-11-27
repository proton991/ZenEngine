#ifndef ZEN_HISTORY_RESPONSE
#define ZEN_HISTORY_RESPONSE
// Relative drift has units of change per observation. The step term permits a full
// replacement when the light disappears, even if the new Monte Carlo sample is black.
float WorldHistoryCap(float change, float sigma, float limit)
{
    if (change <= 0.0)
    {
        return limit;
    }
    float ratio = sigma * sigma / max(change * change, 1e-12);
    return min(limit, min(max(4.0, pow(.25 * ratio, 1.0 / 3.0)), 1.0 + ratio));
}
#endif
