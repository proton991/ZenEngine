#ifndef ZEN_MATERIAL_COVERAGE_BOUNDS
#define ZEN_MATERIAL_COVERAGE_BOUNDS

// Conservative bounds for the same LOD-zero coverage used by ray candidates.
// Callers define MATERIAL_COVERAGE_FAILURE() to reject invalid bake input.
const uint MATERIAL_COVERAGE_MAX_FOOTPRINT = 64u;

struct MaterialCoverageInput
{
    vec3             vertexAlpha;
    vec2             vertexUV[3];
    float            factor;
    float            cutoff;
    int              textureIndex;
    uint             samplerIndex;
    bool             linearFilter;
    float            fractionError;
    vec2             uvArithmeticError;
    TextureTransform transform;
    vec2             sampling;
};

vec3 MaterialPatchWeights(vec2 barycentric)
{
    return vec3(1.0 - barycentric.x - barycentric.y, barycentric);
}

vec2 MaterialCoverageUV(MaterialCoverageInput inputData, vec2 barycentric)
{
    vec3 weights = MaterialPatchWeights(barycentric);
    vec2 uv      = vec2(0);
    for (uint corner = 0u; corner < 3u; ++corner)
    {
        uv += weights[corner] * inputData.vertexUV[corner];
    }
    return vec2(dot(inputData.transform.row0.xyz, vec3(uv, 1)), dot(inputData.transform.row1.xyz, vec3(uv, 1)));
}

float MaterialCoverageVertexAlpha(MaterialCoverageInput inputData, vec2 barycentric)
{
    vec3  weights = MaterialPatchWeights(barycentric);
    float alpha   = 0.0;
    for (uint corner = 0u; corner < 3u; ++corner)
    {
        alpha += weights[corner] * inputData.vertexAlpha[corner];
    }
    return alpha * inputData.factor;
}

float MaterialCoverageAt(MaterialCoverageInput inputData, vec2 barycentric)
{
    float alpha = MaterialCoverageVertexAlpha(inputData, barycentric);
    if (inputData.textureIndex >= 0)
    {
        alpha *= MaterialBindingTextureLod0(inputData.textureIndex, inputData.transform, inputData.sampling,
                                            MaterialCoverageUV(inputData, barycentric))
                     .a;
    }
    return alpha;
}

bool MaterialCoveredAt(MaterialCoverageInput inputData, vec2 barycentric)
{
    // Near u+v=1 the larger coordinate is at least one half, so this
    // subtraction is exact (Sterbenz); 1-u-v can admit rounded outside points.
    bool inside = all(greaterThanEqual(barycentric, vec2(0)))
               && min(barycentric.x, barycentric.y) <= 1.0 - max(barycentric.x, barycentric.y);
    return inside && MaterialCoverageAt(inputData, barycentric) >= inputData.cutoff;
}

float MaterialCoverageBilinear(vec4 texels, vec2 fraction)
{
    // textureGather order: (i0,j1), (i1,j1), (i1,j0), (i0,j0).
    return mix(mix(texels.w, texels.z, fraction.x), mix(texels.x, texels.y, fraction.x), fraction.y);
}

vec2 MaterialCoverageTextureBounds(MaterialCoverageInput inputData, vec2 lowerUV, vec2 upperUV)
{
    vec2 result = vec2(0, 1);
    // Bound interpolation/affine cancellation using the original UV and matrix
    // magnitudes, not just the possibly tiny transformed endpoint coordinates.
    lowerUV -= inputData.uvArithmeticError;
    upperUV += inputData.uvArithmeticError;
    if (inputData.transform.atlasRegion.z != 0u)
    {
        // The spatial atlas remap is monotone, including clamp and quantization.
        lowerUV = MaterialAtlasUV(inputData.transform, inputData.sampling, lowerUV);
        upperUV = MaterialAtlasUV(inputData.transform, inputData.sampling, upperUV);
    }
    ivec2 dimensions = textureSize(
        sampler2D(uTexture2DHeap[nonuniformEXT(inputData.textureIndex)], uSamplerHeap[nonuniformEXT(inputData.samplerIndex)]),
        0);
    vec2  lower     = lowerUV * vec2(dimensions);
    vec2  upper     = upperUV * vec2(dimensions);
    float magnitude = max(1.0, max(max(abs(lower.x), abs(lower.y)), max(abs(upper.x), abs(upper.y))));
    bool  finite    = !any(isnan(lower)) && !any(isinf(lower)) && !any(isnan(upper)) && !any(isinf(upper));
    if (finite && magnitude < 1048576.0)
    {
        // Include native fractional-filter rounding and float coordinate arithmetic.
        float margin  = (inputData.linearFilter ? inputData.fractionError : 0.0) + 8.0 * 1.1920929e-7 * magnitude;
        lower        -= vec2(margin);
        upper        += vec2(margin);
        float origin  = inputData.linearFilter ? 0.5 : 0.0;
        ivec2 first   = ivec2(floor(lower - vec2(origin)));
        ivec2 last    = ivec2(floor(upper - vec2(origin)));
        uvec2 extent  = uvec2(last - first + 1);
        if (all(lessThanEqual(extent, uvec2(MATERIAL_COVERAGE_MAX_FOOTPRINT)))
            && extent.x * extent.y <= MATERIAL_COVERAGE_MAX_FOOTPRINT)
        {
            result = vec2(1, 0);
            for (int y = first.y; y <= last.y; ++y)
            {
                for (int x = first.x; x <= last.x; ++x)
                {
                    vec2 cell = vec2(x, y);
                    // Stay away from gather's texel-selection boundaries.
                    vec2 point  = (cell + vec2(inputData.linearFilter ? 1.0 : 0.75)) / vec2(dimensions);
                    vec4 texels = textureGather(sampler2D(uTexture2DHeap[nonuniformEXT(inputData.textureIndex)],
                                                          uSamplerHeap[nonuniformEXT(inputData.samplerIndex)]),
                                                point, 3);
                    if (any(isnan(texels)) || any(isinf(texels)))
                    {
                        MATERIAL_COVERAGE_FAILURE();
                    }
                    else if (inputData.linearFilter)
                    {
                        vec2 lo = clamp(lower - cell - vec2(0.5), vec2(0), vec2(1));
                        vec2 hi = clamp(upper - cell - vec2(0.5), vec2(0), vec2(1));
                        vec4 corners =
                            vec4(MaterialCoverageBilinear(texels, lo), MaterialCoverageBilinear(texels, vec2(lo.x, hi.y)),
                                 MaterialCoverageBilinear(texels, vec2(hi.x, lo.y)), MaterialCoverageBilinear(texels, hi));
                        float error = all(equal(texels, vec4(texels.x))) ? 0.0 : 2e-6;
                        result.x    = min(result.x, min(min(corners.x, corners.y), min(corners.z, corners.w)) - error);
                        result.y    = max(result.y, max(max(corners.x, corners.y), max(corners.z, corners.w)) + error);
                    }
                    else
                    {
                        result.x = min(result.x, texels.w);
                        result.y = max(result.y, texels.w);
                    }
                }
            }
            result = clamp(result, vec2(0), vec2(1));
        }
    }
    else
    {
        MATERIAL_COVERAGE_FAILURE();
    }
    return result;
}

vec2 MaterialCoverageBounds(MaterialCoverageInput inputData, vec2 a, vec2 b, vec2 c)
{
    vec3 alpha  = vec3(MaterialCoverageVertexAlpha(inputData, a), MaterialCoverageVertexAlpha(inputData, b),
                       MaterialCoverageVertexAlpha(inputData, c));
    vec2 bounds = vec2(min(alpha.x, min(alpha.y, alpha.z)), max(alpha.x, max(alpha.y, alpha.z)));
    // Even equal vertex alphas can cross a nearby cutoff after rounded weighted
    // accumulation; constant inputs are not exempt from the arithmetic guard.
    float sourceMagnitude =
        max(inputData.vertexAlpha.x, max(inputData.vertexAlpha.y, inputData.vertexAlpha.z)) * inputData.factor;
    float error = 16.0 * 1.1920929e-7 * sourceMagnitude;
    bounds      = vec2(max(bounds.x - error, 0.0), bounds.y + error);
    if (inputData.textureIndex >= 0)
    {
        vec2 ua  = MaterialCoverageUV(inputData, a);
        vec2 ub  = MaterialCoverageUV(inputData, b);
        vec2 uc  = MaterialCoverageUV(inputData, c);
        bounds  *= MaterialCoverageTextureBounds(inputData, min(ua, min(ub, uc)), max(ua, max(ub, uc)));
    }
    return bounds;
}

#endif
