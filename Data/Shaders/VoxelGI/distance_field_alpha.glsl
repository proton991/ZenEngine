#ifndef ZEN_DISTANCE_FIELD_ALPHA
#define ZEN_DISTANCE_FIELD_ALPHA

// Native material coverage extension; not an algorithm supplied by Brixelizer.
// Bounds use base-level texels with the material's actual sampler addressing.
// Unknown patches are never discarded. A retained boundary patch needs a covered
// witness on the original triangle within a quarter-sample world-distance bound.
const uint FIELD_ALPHA_MAX_DEPTH = ZEN_FIELD_COVERAGE_MAX_DEPTH;
const uint FIELD_ALPHA_MAX_NODES = ZEN_FIELD_COVERAGE_MAX_NODES;
const uint FIELD_ALPHA_STACK     = ZEN_FIELD_COVERAGE_STACK_WORDS;

layout(std430, set = 6, binding = 5) readonly buffer FieldSamplerInfo
{
    uint fieldSamplerInfo[]; // slot count, subTexelPrecisionBits, then flags per slot
};

#define MATERIAL_COVERAGE_FAILURE() FieldFail(ZEN_FIELD_FAILURE_COVERAGE)
#include "../Common/material_coverage_bounds.glsl"
#undef MATERIAL_COVERAGE_FAILURE

bool FieldCoverageWitness(MaterialCoverageInput inputData,
                          vec2                  a,
                          vec2                  b,
                          vec2                  c,
                          vec3                  originalA,
                          vec3                  originalB,
                          vec3                  originalC)
{
    bool covered = MaterialCoveredAt(inputData, a) || MaterialCoveredAt(inputData, b) || MaterialCoveredAt(inputData, c)
                || MaterialCoveredAt(inputData, (a + b) * 0.5) || MaterialCoveredAt(inputData, (a + c) * 0.5)
                || MaterialCoveredAt(inputData, (b + c) * 0.5) || MaterialCoveredAt(inputData, (a + b + c) / 3.0);
    if (!covered)
    {
        // A fixed arithmetic/texel guard can overlap coverage just outside a
        // transparent patch forever. A nearby covered point on the ORIGINAL
        // triangle is also a valid witness if every retained point is within
        // the same quarter-sample error budget. Never accept an unknown leaf
        // merely because it is small, or sample outside the original triangle.
        vec2       centerBary    = (a + b + c) / 3.0;
        vec3       center        = FieldPatchPosition(originalA, originalB, originalC, centerBary);
        float      radius        = max(length(FieldPatchPosition(originalA, originalB, originalC, a) - center),
                                       max(length(FieldPatchPosition(originalA, originalB, originalC, b) - center),
                                           length(FieldPatchPosition(originalA, originalB, originalC, c) - center)));
        float      budget        = 1.0 / 28.0 - 1e-4;
        float      remaining     = max(budget - radius, 0.0);
        vec3       centerWeights = MaterialPatchWeights(centerBary);
        const vec2 directions[8] =
            vec2[8](vec2(1, 0), vec2(-1, 0), vec2(0, 1), vec2(0, -1), vec2(1, 1), vec2(-1, -1), vec2(1, -1), vec2(-1, 1));
        // Search in the triangle's world metric. Barycentric compass directions collapse
        // toward the long edge of a sliver and can all miss nearby coverage across its width.
        vec3  edge        = originalB - originalA;
        float edgeLength  = length(edge);
        vec3  tangent     = edge / max(edgeLength, 1e-20);
        float projection  = dot(originalC - originalA, tangent);
        float height      = length(originalC - originalA - tangent * projection);
        vec2  tangentBary = vec2(1.0 / max(edgeLength, 1e-20), 0);
        vec2  acrossBary  = vec2(-projection / max(edgeLength, 1e-20), 1.0) / max(height, 1e-20);
        for (uint ring = 0u; ring < 3u && !covered && remaining > 0.0; ++ring)
        {
            float fraction = exp2(float(ring) - 2.0);
            for (uint direction = 0u; direction < 8u && !covered; ++direction)
            {
                vec2  delta      = edgeLength > 1e-20 && height > 1e-20
                                     ? tangentBary * directions[direction].x + acrossBary * directions[direction].y
                                     : directions[direction];
                vec3  worldDelta = (originalB - originalA) * delta.x + (originalC - originalA) * delta.y;
                float magnitude  = length(worldDelta);
                if (magnitude > 0.0)
                {
                    float step         = fraction * remaining / magnitude;
                    vec3  deltaWeights = vec3(-delta.x - delta.y, delta);
                    for (uint corner = 0u; corner < 3u; ++corner)
                    {
                        if (deltaWeights[corner] < 0.0)
                        {
                            step = min(step, centerWeights[corner] / -deltaWeights[corner]);
                        }
                    }
                    vec2 candidate = centerBary + delta * (step * 0.99999);
                    vec3 point     = FieldPatchPosition(originalA, originalB, originalC, candidate);
                    if (radius + length(point - center) <= budget)
                    {
                        covered = MaterialCoveredAt(inputData, candidate);
                    }
                }
            }
        }
    }
    return covered;
}

void FieldAppendPatchJobs(uint triangle, uint code, uint depth, vec3 a, vec3 b, vec3 c);
uint FieldAppendCoveragePatchJobs(uint triangle, uint code, uint depth, vec3 a, vec3 b, vec3 c, uint regionFirst);

bool FieldCoverageInputForTriangle(uint triangle, vec3 vertexAlpha, out MaterialCoverageInput inputData)
{
    Material material           = materialData[triangles[triangle].z];
    bool     specular           = material.surfaceProperties.w > 0.5;
    inputData.vertexAlpha       = vertexAlpha;
    inputData.factor            = specular ? material.diffuseFactor.a : material.baseColorFactor.a;
    inputData.cutoff            = material.surfaceProperties.y == 1.0 ? material.surfaceProperties.x : 0.5;
    inputData.textureIndex      = specular ? int(material.featureTextures[2].properties.x) : material.bcTexIndex;
    inputData.transform         = specular ? material.featureTextures[2].transform : material.textureTransforms[0];
    inputData.sampling          = material.textureSampling.xy;
    inputData.samplerIndex      = uint(max(inputData.transform.row0.w, 0.0));
    inputData.linearFilter      = true;
    inputData.fractionError     = 0.0;
    inputData.uvArithmeticError = vec2(0);
    float maximum               = max(vertexAlpha.x, max(vertexAlpha.y, vertexAlpha.z)) * inputData.factor;
    float maximumBound          = maximum + 16.0 * 1.1920929e-7 * abs(maximum);
    bool  valid = !any(isnan(vertexAlpha)) && !any(isinf(vertexAlpha)) && all(greaterThanEqual(vertexAlpha, vec3(0)))
               && !isnan(inputData.factor) && !isinf(inputData.factor) && inputData.factor >= 0.0 && !isnan(inputData.cutoff)
               && !isinf(inputData.cutoff) && !isnan(maximumBound) && !isinf(maximumBound);
    return valid;
}

#ifdef FIELD_JOBS
void FieldCreateCoverageJobs(uint triangle, uint localTriangle, vec3 a, vec3 b, vec3 c, vec3 vertexAlpha)
{
    Material material = materialData[triangles[triangle].z];
    if (material.surfaceProperties.y == 0.0)
    {
        FieldAppendPatchJobs(triangle, 1u, 0u, a, b, c);
    }
    else
    {
        MaterialCoverageInput inputData;
        if (FieldCoverageInputForTriangle(triangle, vertexAlpha, inputData))
        {
            float maximum = max(vertexAlpha.x, max(vertexAlpha.y, vertexAlpha.z)) * inputData.factor;
            float error   = 16.0 * 1.1920929e-7 * maximum;
            float minimum = min(vertexAlpha.x, min(vertexAlpha.y, vertexAlpha.z)) * inputData.factor;
            if (maximum + error >= inputData.cutoff)
            {
                if (inputData.textureIndex < 0 && minimum - error >= inputData.cutoff)
                {
                    FieldAppendPatchJobs(triangle, 1u, 0u, a, b, c);
                }
                else if (inputData.textureIndex < 0 && all(equal(vertexAlpha, vec3(vertexAlpha.x))))
                {
                    // Subdivision cannot shrink a constant-alpha arithmetic uncertainty band.
                    FieldAppendPatchJobs(triangle, 1u, ZEN_FIELD_PATCH_UNAVAILABLE, a, b, c);
                }
                else
                {
                    // Refinement state lives in a compact slot per queued triangle, so storage scales
                    // with the scene's coverage triangles rather than the batch. A batch with more
                    // than the slots retries with fewer triangles.
                    uint slot = atomicAdd(fieldCounters[ZEN_FIELD_COUNTER_COVERAGE_COUNT], 1u);
                    if (slot < uint(fieldCoverageQueue.length()))
                    {
                        uint offset                = slot * ZEN_FIELD_COVERAGE_STATE_WORDS;
                        fieldCoverage[offset]      = 1u;
                        fieldCoverage[offset + 1u] = 1u;
                        fieldCoverage[offset + 2u] = 0u;
                        fieldCoverage[offset + 3u] = 0u;
                        fieldCoverage[offset + 4u] = 1u;
                        fieldCoverageQueue[slot]   = localTriangle;
                    }
                    else
                    {
                        FieldFail(ZEN_FIELD_FAILURE_COVERAGE_SLOTS);
                    }
                }
            }
        }
        else
        {
            FieldFail(ZEN_FIELD_FAILURE_COVERAGE);
        }
    }
}
#endif

#ifdef FIELD_COVERAGE
void FieldContinueCoverage(uint slot)
{
    uint offset        = slot * ZEN_FIELD_COVERAGE_STATE_WORDS;
    uint localTriangle = fieldCoverageQueue[slot];
    if (fieldCoverage[offset] == 1u)
    {
        uint triangle = FieldSourceTriangle(FieldTriangleFirst() + localTriangle);
        vec3 vertexAlpha, a, b, c;
        FieldTriangleVertices(triangle, vertexAlpha, a, b, c);
        a = (a - distanceField.gridMinCellSize.xyz) / distanceField.gridMinCellSize.w;
        b = (b - distanceField.gridMinCellSize.xyz) / distanceField.gridMinCellSize.w;
        c = (c - distanceField.gridMinCellSize.xyz) / distanceField.gridMinCellSize.w;
        MaterialCoverageInput inputData;
        bool                  valid    = FieldCoverageInputForTriangle(triangle, vertexAlpha, inputData);
        Material              material = materialData[triangles[triangle].z];
        bool                  specular = material.surfaceProperties.w > 0.5;
        uint                  stride   = uint(uvValues[0].x);
        uint uvSet = uint(max(specular ? int(material.featureTextures[2].properties.y) : material.bcTexSet, 0));
        for (uint corner = 0u; corner < 3u; ++corner)
        {
            uint vertex                = indices[triangles[triangle].x + corner];
            inputData.vertexUV[corner] = uvSet < stride ? uvValues[1u + vertex * stride + uvSet].xy : vec2(0);
        }
        vec2 rawMagnitude = max(abs(inputData.vertexUV[0]), max(abs(inputData.vertexUV[1]), abs(inputData.vertexUV[2])));
        vec2 linearMagnitude =
            vec2(dot(abs(inputData.transform.row0.xy), rawMagnitude), dot(abs(inputData.transform.row1.xy), rawMagnitude));
        vec2 affineMagnitude        = linearMagnitude + abs(vec2(inputData.transform.row0.z, inputData.transform.row1.z));
        inputData.uvArithmeticError = 32.0 * 1.1920929e-7 * (linearMagnitude + affineMagnitude);
        if (inputData.textureIndex >= 0)
        {
            valid = valid && inputData.samplerIndex < fieldSamplerInfo[0] && fieldSamplerInfo[1] > 0u;
            if (valid)
            {
                uint flags              = fieldSamplerInfo[2u + inputData.samplerIndex];
                valid                   = (flags & 2u) != 0u;
                inputData.linearFilter  = (flags & 1u) != 0u;
                inputData.fractionError = 1.0 / float(1u << min(fieldSamplerInfo[1], 20u));
            }
        }
        if (valid)
        {
            if (FieldCoverageJobLimit() == 0u)
            {
                FieldFail(ZEN_FIELD_FAILURE_REFERENCES);
            }
            uint stack[FIELD_ALPHA_STACK];
            uint top           = fieldCoverage[offset + 1u];
            uint visited       = fieldCoverage[offset + 2u];
            uint pendingRegion = fieldCoverage[offset + 3u];
            for (uint index = 0u; index < FIELD_ALPHA_STACK; ++index)
            {
                stack[index] = fieldCoverage[offset + 4u + index];
            }
            uint processed = 0u;
            while (top > 0u && processed < ZEN_FIELD_COVERAGE_NODES_PER_DISPATCH
                   && atomicAdd(fieldCounters[ZEN_FIELD_COUNTER_JOBS], 0u) < FieldCoverageJobLimit()
                   && atomicAdd(fieldControl[ZEN_FIELD_CONTROL_BATCH_FAILURE], 0u) == 0u
                   && atomicAdd(fieldCounters[ZEN_FIELD_COUNTER_FAILURE], 0u) == 0u)
            {
                uint code  = stack[--top];
                uint depth = uint(findMSB(code)) / 2u;
                if (pendingRegion == 0u)
                {
                    ++visited;
                }
                ++processed;
                vec2 ba, bb, bc;
                FieldPatchBarycentrics(code, depth, ba, bb, bc);
                vec3 pa            = FieldPatchPosition(a, b, c, ba);
                vec3 pb            = FieldPatchPosition(a, b, c, bb);
                vec3 pc            = FieldPatchPosition(a, b, c, bc);
                bool affectsVolume = FieldPatchAffectsVolume(pa, pb, pc);
                vec2 bounds        = vec2(inputData.cutoff);
                bool unavailable   = (pendingRegion & ZEN_FIELD_PATCH_UNAVAILABLE) != 0u || visited >= FIELD_ALPHA_MAX_NODES;
                if (affectsVolume && pendingRegion == 0u && !unavailable)
                {
                    bounds = MaterialCoverageBounds(inputData, ba, bb, bc);
                }
                if (affectsVolume && bounds.y >= inputData.cutoff)
                {
                    float diameter2 = max(dot(pb - pa, pb - pa), max(dot(pc - pa, pc - pa), dot(pc - pb, pc - pb)));
                    bool  small     = diameter2 <= 1.0 / (28.0 * 28.0);
                    bool  retain    = pendingRegion != 0u || bounds.x >= inputData.cutoff
                                   || (small && FieldCoverageWitness(inputData, ba, bb, bc, a, b, c));
                    unavailable = unavailable || (!retain && (depth >= FIELD_ALPHA_MAX_DEPTH || top + 4u > FIELD_ALPHA_STACK));
                    if (retain || unavailable)
                    {
                        // A finite refinement budget cannot decide every native-sampler cutoff.
                        // Preserve the unresolved region explicitly; never turn it into a covered
                        // surface or empty space. The normal bounded job windows drain it too.
                        uint cursor   = pendingRegion & ~ZEN_FIELD_PATCH_UNAVAILABLE;
                        uint flags    = unavailable ? ZEN_FIELD_PATCH_UNAVAILABLE : 0u;
                        pendingRegion = FieldAppendCoveragePatchJobs(triangle, code, depth | flags, pa, pb, pc,
                                                                     cursor != 0u ? cursor - 1u : 0u);
                        if (pendingRegion != 0u)
                        {
                            // Resume this accepted patch's remaining region jobs
                            // after Count/Allocate/Emit drains the current window.
                            stack[top++]   = code;
                            pendingRegion |= flags;
                        }
                    }
                    else
                    {
                        for (uint child = 0u; child < 4u; ++child)
                        {
                            stack[top++] = (code << 2u) | child;
                        }
                    }
                }
            }
            fieldCoverage[offset]      = top > 0u ? 1u : 2u;
            fieldCoverage[offset + 1u] = top;
            fieldCoverage[offset + 2u] = visited;
            fieldCoverage[offset + 3u] = pendingRegion;
            for (uint index = 0u; index < FIELD_ALPHA_STACK; ++index)
            {
                fieldCoverage[offset + 4u + index] = stack[index];
            }
            if (top > 0u)
            {
                atomicAdd(fieldCounters[ZEN_FIELD_COUNTER_COVERAGE_PENDING], 1u);
            }
        }
        else
        {
            FieldFail(ZEN_FIELD_FAILURE_COVERAGE);
        }
    }
}
#endif
#endif
