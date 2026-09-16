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

layout(set = 1, binding = 0, rgba8) uniform writeonly image3D voxelTexture;

layout(set = 2, binding = 0) uniform uSceneInfo
{
    vec4 aabbMin;
    vec4 aabbMax;
}
ubo;

layout(set = 3, binding = 0) readonly buffer VertexBuffer
{
    Vertex vertices[];
};

layout(set = 3, binding = 1) readonly buffer IndexBuffer
{
    uint indices[];
};

struct NodeData
{
    mat4 modelMatrix;
    mat4 normalMatrix;
};

layout(std140, set = 3, binding = 2) readonly buffer NodeBuffer
{
    NodeData nodesData[];
};



layout(set = 6, binding = 0) readonly buffer TriangleMap
{
    uint triangleMap[];
};

struct VkDispatchIndirectCommand
{
    uint x;
    uint y;
    uint z;
};

struct LargeTriangle
{
    uint triangleIndex;
    uint innerTriangleIndex;
    mat4 modelMatrix;
};

layout(push_constant) uniform constants
{
    uint nodeIndex;
    uint triangleCount;
    uint largeTriangleThreshold;
}
pc;

bool test_axis(vec3 axis, vec3 u0, vec3 u1, vec3 u2, float extent)
{
    vec3 A0 = vec3(1.0, 0.0, 0.0);
    vec3 A1 = vec3(0.0, 1.0, 0.0);
    vec3 A2 = vec3(0.0, 0.0, 1.0);

    float p0 = dot(axis, u0);
    float p1 = dot(axis, u1);
    float p2 = dot(axis, u2);
    float R  = extent * (abs(dot(A0, axis)) + abs(dot(A1, axis)) + abs(dot(A2, axis)));
    return (min(min(p0, p1), p2) > R || max(max(p0, p1), p2) < -R);
}

bool voxel_triangle_collision_test(vec3 u0,
                                   vec3 u1,
                                   vec3 u2,
                                   ivec3 voxel,
                                   float voxel_width,
                                   vec3 voxel_grid_min)
{
    float a = voxel_width / 2.0;                                    // extent
    vec3 e0 = u1 - u0;                                              // edge 1
    vec3 e1 = u2 - u1;                                              // edge 2
    vec3 e2 = u2 - u0;                                              // edge 3
    vec3 n  = cross(e0, e1);                                        // normal
    vec3 c  = voxel_grid_min + vec3(voxel) * voxel_width + vec3(a); // voxel center
    vec3 A0 = vec3(1.0, 0.0, 0.0);
    vec3 A1 = vec3(0.0, 1.0, 0.0);
    vec3 A2 = vec3(0.0, 0.0, 1.0);

    u0 -= c;
    u1 -= c;
    u2 -= c;

    return !(test_axis(A0, u0, u1, u2, voxel_width) || test_axis(A1, u0, u1, u2, voxel_width) ||
             test_axis(A2, u0, u1, u2, voxel_width) ||
             test_axis(normalize(cross(A0, e0)), u0, u1, u2, voxel_width) ||
             test_axis(normalize(cross(A0, e1)), u0, u1, u2, voxel_width) ||
             test_axis(normalize(cross(A0, e2)), u0, u1, u2, voxel_width) ||
             test_axis(normalize(cross(A1, e0)), u0, u1, u2, voxel_width) ||
             test_axis(normalize(cross(A1, e1)), u0, u1, u2, voxel_width) ||
             test_axis(normalize(cross(A1, e2)), u0, u1, u2, voxel_width) ||
             test_axis(normalize(cross(A2, e0)), u0, u1, u2, voxel_width) ||
             test_axis(normalize(cross(A2, e1)), u0, u1, u2, voxel_width) ||
             test_axis(normalize(cross(A2, e2)), u0, u1, u2, voxel_width) ||
             test_axis(normalize(n), u0, u1, u2, voxel_width));
}

vec3 get_barycentric_coordinates(vec3 a, vec3 b, vec3 c, vec3 p)
{
    vec3 ab = b - a;
    vec3 ac = c - a;
    vec3 ap = p - a;

    float d00 = dot(ab, ab);
    float d01 = dot(ab, ac);
    float d11 = dot(ac, ac);
    float d20 = dot(ap, ab);
    float d21 = dot(ap, ac);

    float denom = d00 * d11 - d01 * d01;

    float v = (d11 * d20 - d01 * d21) / denom;
    float w = (d00 * d21 - d01 * d20) / denom;
    float u = 1.0f - v - w;

    return vec3(u, v, w);
}

vec3 world_pos_to_voxel_space(vec3 vertex_world, vec3 grid_min, float voxel_width)
{
    return (vertex_world - grid_min) / voxel_width;
}
