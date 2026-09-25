#ifndef ZEN_VOXEL_GEOMETRY
#define ZEN_VOXEL_GEOMETRY
#ifndef VOXEL_GEOMETRY_SET
#define VOXEL_GEOMETRY_SET 3
#endif
#ifndef VOXEL_MATERIAL_PRECISION
#define VOXEL_MATERIAL_PRECISION
#endif
struct Vertex
{
    vec4 position; vec4 normal; vec4 tangent; vec2 texcoord; vec2 uv1;
    vec4 joint0; vec4 weight0; vec4 color;
};
struct NodeData { mat4 modelMatrix; mat4 normalMatrix; };
layout(std430, set = VOXEL_GEOMETRY_SET, binding = 0) readonly buffer VertexBuffer { Vertex vertices[]; };
layout(std430, set = VOXEL_GEOMETRY_SET, binding = 1) readonly buffer IndexBuffer { uint indices[]; };
layout(std140, set = VOXEL_GEOMETRY_SET, binding = 2) readonly buffer NodeBuffer { NodeData nodesData[]; };
// index offset, stable node slot, material index, enabled class (zero means disabled)
layout(std430, set = VOXEL_GEOMETRY_SET, binding = 4) readonly buffer TriangleRecords { uvec4 triangles[]; };
float VoxelSurfaceDot(vec3 a, vec3 b)
{
#ifdef VOXEL_PRECISE_CONTRIBUTION
    precise float value=a.x*b.x+a.y*b.y+a.z*b.z;
#else
    float value=dot(a,b);
#endif
    return value;
}
vec3 TriangleBarycentrics(vec3 a, vec3 b, vec3 c, vec3 p)
{
    VOXEL_MATERIAL_PRECISION vec3 ab = b-a, ac = c-a, ap = p-a;
    VOXEL_MATERIAL_PRECISION float d00 = VoxelSurfaceDot(ab,ab), d01 = VoxelSurfaceDot(ab,ac), d11 = VoxelSurfaceDot(ac,ac);
    VOXEL_MATERIAL_PRECISION float denominator = d00*d11-d01*d01;
    VOXEL_MATERIAL_PRECISION vec3 weights = vec3(1,0,0);
    if (abs(denominator) > 1e-20)
    {
        VOXEL_MATERIAL_PRECISION float v = (d11*VoxelSurfaceDot(ap,ab)-d01*VoxelSurfaceDot(ap,ac))/denominator;
        VOXEL_MATERIAL_PRECISION float w = (d00*VoxelSurfaceDot(ap,ac)-d01*VoxelSurfaceDot(ap,ab))/denominator;
        weights = max(vec3(1.0-v-w,v,w), vec3(0));
        weights /= max(VoxelSurfaceDot(weights,vec3(1)), 1e-8);
    }
    return weights;
}
void TriangleVertices(uint record, out Vertex a, out Vertex b, out Vertex c,
                      out VOXEL_MATERIAL_PRECISION vec3 pa, out VOXEL_MATERIAL_PRECISION vec3 pb, out VOXEL_MATERIAL_PRECISION vec3 pc)
{
    uvec4 triangle = triangles[record];
    a = vertices[indices[triangle.x]];
    b = vertices[indices[triangle.x+1]];
    c = vertices[indices[triangle.x+2]];
    mat4 model = nodesData[triangle.y].modelMatrix;
    pa = (model * vec4(a.position.xyz,1)).xyz;
    pb = (model * vec4(b.position.xyz,1)).xyz;
    pc = (model * vec4(c.position.xyz,1)).xyz;
}
#endif
