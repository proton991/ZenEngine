#ifndef ZEN_VOXEL_GEOMETRY
#define ZEN_VOXEL_GEOMETRY
#ifndef VOXEL_GEOMETRY_SET
#    define VOXEL_GEOMETRY_SET 3
#endif
#ifndef VOXEL_MATERIAL_PRECISION
#    define VOXEL_MATERIAL_PRECISION
#endif
#include "../Common/scene_geometry.glsl"
// index offset, stable node slot, material index, enabled class (zero means disabled)
layout(std430, set = VOXEL_GEOMETRY_SET, binding = 4) readonly buffer TriangleRecords
{
    uvec4 triangles[];
};
float VoxelSurfaceDot(vec3 a, vec3 b)
{
#ifdef VOXEL_PRECISE_CONTRIBUTION
    precise float value = a.x * b.x + a.y * b.y + a.z * b.z;
#else
    float value = dot(a, b);
#endif
    return value;
}
vec3 TriangleBarycentrics(vec3 a, vec3 b, vec3 c, vec3 p)
{
    VOXEL_MATERIAL_PRECISION vec3  ab = b - a, ac = c - a, ap = p - a;
    VOXEL_MATERIAL_PRECISION float d00 = VoxelSurfaceDot(ab, ab), d01 = VoxelSurfaceDot(ab, ac), d11 = VoxelSurfaceDot(ac, ac);
    VOXEL_MATERIAL_PRECISION float denominator = d00 * d11 - d01 * d01;
    VOXEL_MATERIAL_PRECISION vec3  weights     = vec3(1, 0, 0);
    if (abs(denominator) > 1e-20)
    {
        VOXEL_MATERIAL_PRECISION float v  = (d11 * VoxelSurfaceDot(ap, ab) - d01 * VoxelSurfaceDot(ap, ac)) / denominator;
        VOXEL_MATERIAL_PRECISION float w  = (d00 * VoxelSurfaceDot(ap, ac) - d01 * VoxelSurfaceDot(ap, ab)) / denominator;
        weights                           = max(vec3(1.0 - v - w, v, w), vec3(0));
        weights                          /= max(VoxelSurfaceDot(weights, vec3(1)), 1e-8);
    }
    return weights;
}
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
void TriangleVertices(uint                              record,
                      out Vertex                        a,
                      out Vertex                        b,
                      out Vertex                        c,
                      out VOXEL_MATERIAL_PRECISION vec3 pa,
                      out VOXEL_MATERIAL_PRECISION vec3 pb,
                      out VOXEL_MATERIAL_PRECISION vec3 pc)
{
    uvec4 triangle = triangles[record];
    a              = vertices[indices[triangle.x]];
    b              = vertices[indices[triangle.x + 1]];
    c              = vertices[indices[triangle.x + 2]];
    mat4 model     = nodesData[triangle.y].modelMatrix;
    pa             = (model * vec4(a.position.xyz, 1)).xyz;
    pb             = (model * vec4(b.position.xyz, 1)).xyz;
    pc             = (model * vec4(c.position.xyz, 1)).xyz;
}
#endif
