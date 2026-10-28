#ifndef ZEN_DISTANCE_FIELD_PATCH
#define ZEN_DISTANCE_FIELD_PATCH

// A quadtree node starts at code 1. Each subdivision appends a two-bit child.
void FieldPatchBarycentrics(uint code, uint depth, out vec2 a, out vec2 b, out vec2 c)
{
    a = vec2(0, 0);
    b = vec2(1, 0);
    c = vec2(0, 1);
    for (uint level = 0u; level < depth; ++level)
    {
        uint child = (code >> (2u * (depth - level - 1u))) & 3u;
        vec2 ab    = (a + b) * 0.5;
        vec2 ac    = (a + c) * 0.5;
        vec2 bc    = (b + c) * 0.5;
        if (child == 0u)
        {
            b = ab;
            c = ac;
        }
        else if (child == 1u)
        {
            a = ab;
            c = bc;
        }
        else if (child == 2u)
        {
            a = ac;
            b = bc;
        }
        else
        {
            a = bc;
            b = ac;
            c = ab;
        }
    }
}

vec3 FieldPatchPosition(vec3 a, vec3 b, vec3 c, vec2 barycentric)
{
    return a + (b - a) * barycentric.x + (c - a) * barycentric.y;
}

void FieldPatchTriangle(uvec4 job, out vec3 a, out vec3 b, out vec3 c)
{
    FieldTriangle(job.x, a, b, c);
    uint depth = job.w & ~ZEN_FIELD_PATCH_UNAVAILABLE;
    if (depth > 0u)
    {
        vec3 originalA = a, originalB = b, originalC = c;
        vec2 ba, bb, bc;
        FieldPatchBarycentrics(job.z, depth, ba, bb, bc);
        a = FieldPatchPosition(originalA, originalB, originalC, ba);
        b = FieldPatchPosition(originalA, originalB, originalC, bb);
        c = FieldPatchPosition(originalA, originalB, originalC, bc);
    }
}
#endif
