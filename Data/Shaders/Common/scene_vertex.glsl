#ifndef ZEN_SCENE_VERTEX
#define ZEN_SCENE_VERTEX
// Storage-buffer layout of asset::Vertex, shared by visibility and receiver motion.
struct Vertex
{
    vec4 position;
    vec4 normal;
    vec4 tangent;
    vec2 texcoord;
    vec2 uv1;
    vec4 joint0;
    vec4 weight0;
    vec4 color;
};
#endif
