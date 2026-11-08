#ifndef ZEN_FRUSTUM_BOUNDS
#define ZEN_FRUSTUM_BOUNDS
bool IntersectsFrustum(vec3 minimum, vec3 maximum, mat4 projectionView)
{
    bool visible = true;
    // Missing/invalid bounds must never hide geometry.
    bool valid = all(lessThanEqual(minimum, maximum)) && !any(isnan(minimum)) && !any(isnan(maximum)) && !any(isinf(minimum))
              && !any(isinf(maximum));
    if (valid)
    {
        mat4 rows = transpose(projectionView);
        // Vulkan clip depth is [0, w]. No perspective divide is needed, so boxes
        // crossing the eye/near plane remain conservative for both camera types.
        vec4 planes[6] =
            vec4[6](rows[3] + rows[0], rows[3] - rows[0], rows[3] + rows[1], rows[3] - rows[1], rows[2], rows[3] - rows[2]);
        for (uint plane = 0; plane < 6; ++plane)
        {
            vec4 p       = planes[plane];
            vec3 support = mix(minimum, maximum, greaterThanEqual(p.xyz, vec3(0.0)));
            // Scale the tolerance by the uncancelled products, including the
            // translation term, to avoid rejecting a box on a clip plane.
            float tolerance = 1e-5 * (dot(abs(p.xyz), abs(support)) + abs(p.w) + 1.0);
            visible         = visible && !(dot(p.xyz, support) + p.w < -tolerance);
        }
    }
    return visible;
}

#endif
