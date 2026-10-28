#ifndef ZEN_TRIANGLE_DISTANCE
#define ZEN_TRIANGLE_DISTANCE

// Point of triangle abc closest to p (Ericson, Real-Time Collision Detection, 5.1.5).
vec3 TriangleClosestPoint(vec3 p, vec3 a, vec3 b, vec3 c)
{
    vec3  ab = b - a, ac = c - a, ap = p - a, bp = p - b, cp = p - c;
    float d1 = dot(ab, ap), d2 = dot(ac, ap), d3 = dot(ab, bp), d4 = dot(ac, bp), d5 = dot(ab, cp), d6 = dot(ac, cp);
    float va = d3 * d6 - d5 * d4, vb = d5 * d2 - d1 * d6, vc = d1 * d4 - d3 * d2;
    vec3  result;
    if (d1 <= 0.0 && d2 <= 0.0)
    {
        result = a;
    }
    else if (d3 >= 0.0 && d4 <= d3)
    {
        result = b;
    }
    else if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0)
    {
        result = a + ab * (d1 / (d1 - d3));
    }
    else if (d6 >= 0.0 && d5 <= d6)
    {
        result = c;
    }
    else if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0)
    {
        result = a + ac * (d2 / (d2 - d6));
    }
    else if (va <= 0.0 && d4 - d3 >= 0.0 && d5 - d6 >= 0.0)
    {
        result = b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
    }
    else
    {
        float denominator = 1.0 / (va + vb + vc);
        result            = a + ab * (vb * denominator) + ac * (vc * denominator);
    }
    return result;
}
#endif
