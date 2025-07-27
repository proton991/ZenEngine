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

layout(set = 0, binding = 0, rgba8) uniform image3D voxelTexture;

layout(set = 1, binding = 0) uniform uSceneInfo
{
    vec4 aabbMin;
    vec4 aabbMax;
} ubo;

layout(set = 2, binding = 0) readonly buffer VertexBuffer
{
    Vertex vertices[];
};

layout(set = 2, binding = 1) readonly buffer IndexBuffer
{
    uint indices[];
};

struct NodeData {
    mat4 modelMatrix;
    mat4 normalMatrix;
};

layout(std140, set = 2, binding = 2) readonly buffer NodeBuffer {
    NodeData nodesData[];
};


layout(set = 3, binding = 0) uniform sampler2D uTextureArray[1024];

//layout(set = 3, binding = 0) uniform sampler2D s_Diffuse_unbound[];
//layout(set = 4, binding = 1) uniform sampler2D s_Normal_unbound[];
//layout(set = 4, binding = 2) uniform sampler2D s_Metallic_unbound[];
//layout(set = 4, binding = 3) uniform sampler2D s_Roughness_unbound[];
//layout(set = 4, binding = 4) uniform sampler2D s_Emissive_unbound[];

layout(set = 5, binding = 0) buffer TriangleMap
{
   uint triangleMap[];
};

struct VkDispatchIndirectCommand
{
    uint x;
    uint y;
    uint z;
};

layout(std140, set = 4, binding = 0) buffer IndirectBuffer
{
    VkDispatchIndirectCommand command;
};

struct LargeTriangle
{
    uint triangleIndex;
    uint innerTriangleIndex;
    mat4 modelMatrix;
};

layout(set = 4, binding = 1) buffer LargeTriangleArray
{
    LargeTriangle largeTriangles[];
};

layout(push_constant) uniform constants
{
    uint nodeIndex;
    uint triangleCount;
    uint largeTriangleThreshold;
} pc;

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

bool voxel_triangle_collision_test(vec3 u0, vec3 u1, vec3 u2, ivec3 voxel, float voxel_width, vec3 voxel_grid_min)
{
    float a  = voxel_width / 2.0;// extent
    vec3  e0 = u1 - u0;// edge 1
    vec3  e1 = u2 - u1;// edge 2
    vec3  e2 = u2 - u0;// edge 3
    vec3  n  = cross(e0, e1);// normal
    vec3  c  = voxel_grid_min + vec3(voxel) * voxel_width + vec3(a);// voxel center
    vec3  d  = u0 - c;// vector from triangle to voxel center
    vec3  A0 = vec3(1.0, 0.0, 0.0);
    vec3  A1 = vec3(0.0, 1.0, 0.0);
    vec3  A2 = vec3(0.0, 0.0, 1.0);

    u0 -= c;
    u1 -= c;
    u2 -= c;

    if (test_axis(A0, u0, u1, u2, voxel_width)) return false;
    if (test_axis(A1, u0, u1, u2, voxel_width)) return false;
    if (test_axis(A2, u0, u1, u2, voxel_width)) return false;
    if (test_axis(normalize(cross(A0, e0)), u0, u1, u2, voxel_width)) return false;
    if (test_axis(normalize(cross(A0, e1)), u0, u1, u2, voxel_width)) return false;
    if (test_axis(normalize(cross(A0, e2)), u0, u1, u2, voxel_width)) return false;
    if (test_axis(normalize(cross(A1, e0)), u0, u1, u2, voxel_width)) return false;
    if (test_axis(normalize(cross(A1, e1)), u0, u1, u2, voxel_width)) return false;
    if (test_axis(normalize(cross(A1, e2)), u0, u1, u2, voxel_width)) return false;
    if (test_axis(normalize(cross(A2, e0)), u0, u1, u2, voxel_width)) return false;
    if (test_axis(normalize(cross(A2, e1)), u0, u1, u2, voxel_width)) return false;
    if (test_axis(normalize(cross(A2, e2)), u0, u1, u2, voxel_width)) return false;
    if (test_axis(normalize(n), u0, u1, u2, voxel_width)) return false;
    return true;
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

vec3 find_closest_point_on_line(vec3 a, vec3 b, vec3 p)
{
    vec3  ab = b - a;
    float t  = dot(p - a, ab) / dot(ab, ab);
    return a + t * ab;
}

vec3 get_closest_position(vec3 a, vec3 b, vec3 c, vec3 p)
{
    vec3 ab     = b - a;
    vec3 ac     = c - a;
    vec3 normal = normalize(cross(ab, ac));
    vec3 ap     = p - a;

    float dist         = dot(ap, normal);
    vec3  planar_point = p - dist * normal;

    // check whether planar point is inside or outside the triangle
    vec3 barycentric = get_barycentric_coordinates(a, b, c, planar_point);
    if (barycentric.x >= 0.0f && barycentric.y >= 0.0f && barycentric.z >= 0.0f)
    return planar_point;

    // find the closest point on the edges
    vec3 closest_point_edge_1 = find_closest_point_on_line(a, b, planar_point);
    vec3 closest_point_edge_2 = find_closest_point_on_line(b, c, planar_point);
    vec3 closest_point_edge_3 = find_closest_point_on_line(c, a, planar_point);

    // find the distance to the closest point on the edges
    float dist_edge_1 = distance(planar_point, closest_point_edge_1);
    float dist_edge_2 = distance(planar_point, closest_point_edge_2);
    float dist_edge_3 = distance(planar_point, closest_point_edge_3);

    vec3 vertex_1_of_closest_edge, vertex_2_of_closest_edge, closest_point_to_edge;

    // find the vertices of the closest edge
    if (dist_edge_1 < dist_edge_2 && dist_edge_1 < dist_edge_3)
    {
        vertex_1_of_closest_edge = a;
        vertex_2_of_closest_edge = b;
        closest_point_to_edge    = closest_point_edge_1;
    }
    else if (dist_edge_2 < dist_edge_1 && dist_edge_2 < dist_edge_3)
    {
        vertex_1_of_closest_edge = b;
        vertex_2_of_closest_edge = c;
        closest_point_to_edge    = closest_point_edge_2;
    }
    else
    {
        vertex_1_of_closest_edge = c;
        vertex_2_of_closest_edge = a;
        closest_point_to_edge    = closest_point_edge_3;
    }

    if (dot(vertex_1_of_closest_edge - closest_point_to_edge, vertex_2_of_closest_edge - closest_point_to_edge) < 0.0f)
    {
        return closest_point_to_edge;
    }

    // find the closest vertex from vertex_1_of_closest_edge and vertex_2_of_closest_edge
    float dist_vertex_1 = distance(planar_point, vertex_1_of_closest_edge);
    float dist_vertex_2 = distance(planar_point, vertex_2_of_closest_edge);

    if (dist_vertex_1 < dist_vertex_2)
    {
        return vertex_1_of_closest_edge;
    }
    else
    {
        return vertex_2_of_closest_edge;
    }
}