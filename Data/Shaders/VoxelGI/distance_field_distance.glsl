#ifndef ZEN_DISTANCE_FIELD_DISTANCE
#define ZEN_DISTANCE_FIELD_DISTANCE

vec3 FieldDistanceCross(vec3 a, vec3 b)
{
    // A fused multiply/subtract can leave a residual for cross(v, v). Exact
    // duplicate vertices must not become an artificial nonzero-area triangle.
    precise vec3 result = vec3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
    return result;
}

float FieldSegmentDistance(vec3 position, vec3 a, vec3 b)
{
    vec3  relative      = position - a;
    vec3  edge          = b - a;
    float lengthSquared = dot(edge, edge);
    float parameter     = lengthSquared > 0.0 ? clamp(dot(relative, edge) / lengthSquared, 0.0, 1.0) : 0.0;
    return length(relative - parameter * edge);
}

float FieldTriangleDistance(vec3 position, vec3 a, vec3 b, vec3 c)
{
    // Finite segment projections include duplicate endpoints. The determinant
    // region formula loses all of va/vb/vc for some real-scene slivers and can
    // divide by zero even when the original vertices and sample are finite.
    float result = min(FieldSegmentDistance(position, a, b),
                       min(FieldSegmentDistance(position, b, c), FieldSegmentDistance(position, c, a)));
    vec3  normal = FieldDistanceCross(b - a, c - a);
    float scale  = max(abs(normal.x), max(abs(normal.y), abs(normal.z)));
    if (scale > 0.0)
    {
        // Scale before the squared norm to retain small nonzero-area faces.
        normal      /= scale;
        bool inside  = dot(FieldDistanceCross(b - a, position - a), normal) >= 0.0
                    && dot(FieldDistanceCross(c - b, position - b), normal) >= 0.0
                    && dot(FieldDistanceCross(a - c, position - c), normal) >= 0.0;
        if (inside)
        {
            result = min(result, abs(dot(position - a, normal)) / sqrt(dot(normal, normal)));
        }
    }
    return result;
}
#endif
