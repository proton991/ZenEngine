#ifndef ZEN_SCENE_GEOMETRY
#define ZEN_SCENE_GEOMETRY
struct Vertex
{
    vec4 position; vec4 normal; vec4 tangent; vec2 texcoord; vec2 uv1;
    vec4 joint0; vec4 weight0; vec4 color;
};
struct NodeData { mat4 modelMatrix; mat4 normalMatrix; vec4 surfaceScale; };
layout(std430, set = VOXEL_GEOMETRY_SET, binding = 0) readonly buffer VertexBuffer { Vertex vertices[]; };
layout(std430, set = VOXEL_GEOMETRY_SET, binding = 1) readonly buffer IndexBuffer { uint indices[]; };
layout(std140, set = VOXEL_GEOMETRY_SET, binding = 2) readonly buffer NodeBuffer { NodeData nodesData[]; };
#endif
