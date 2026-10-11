#ifndef ZEN_SCENE_GEOMETRY
#define ZEN_SCENE_GEOMETRY
#include "scene_vertex.glsl"
struct NodeData { mat4 modelMatrix; mat4 normalMatrix; vec4 surfaceScale; };
layout(std430, set = VOXEL_GEOMETRY_SET, binding = 0) readonly buffer VertexBuffer { Vertex vertices[]; };
layout(std430, set = VOXEL_GEOMETRY_SET, binding = 1) readonly buffer IndexBuffer { uint indices[]; };
#ifdef RAY_FORWARD_MATERIAL
layout(std140, set = 1, binding = 1) readonly buffer NodeBuffer { NodeData nodesData[]; };
#else
layout(std140, set = VOXEL_GEOMETRY_SET, binding = 2) readonly buffer NodeBuffer { NodeData nodesData[]; };
#endif
#endif
