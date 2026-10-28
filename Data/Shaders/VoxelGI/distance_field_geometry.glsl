#ifndef ZEN_DISTANCE_FIELD_GEOMETRY
#define ZEN_DISTANCE_FIELD_GEOMETRY

#define VOXEL_GEOMETRY_SET 5
#define SCENE_COMPACT_POSITION_ALPHA
#include "../Common/scene_geometry.glsl"
#include "../Common/material.glsl"
#include "distance_field_distance.glsl"

layout(std430, set = 5, binding = 4) readonly buffer TriangleRecords
{
    uvec4 triangles[];
};
layout(std140, set = 6, binding = 3) readonly buffer MaterialBuffer
{
    Material materialData[];
};
layout(std430, set = 6, binding = 4) readonly buffer UVBuffer
{
    vec4 uvValues[];
};

void FieldTriangleVertices(uint triangle, out vec3 alpha, out vec3 a, out vec3 b, out vec3 c)
{
    uvec4 record = triangles[triangle];
    vec4  va     = fieldVertices[indices[record.x]];
    vec4  vb     = fieldVertices[indices[record.x + 1u]];
    vec4  vc     = fieldVertices[indices[record.x + 2u]];
    mat4  model  = nodesData[record.y].modelMatrix;
    alpha        = vec3(va.w, vb.w, vc.w);
    a            = (model * vec4(va.xyz, 1.0)).xyz;
    b            = (model * vec4(vb.xyz, 1.0)).xyz;
    c            = (model * vec4(vc.xyz, 1.0)).xyz;
}
#endif
