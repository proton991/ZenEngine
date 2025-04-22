#version 450

layout(triangles) in;
layout(triangle_strip, max_vertices = 3) out;

// Input from vertex shader
layout(location = 0) in GS_IN {
    vec3 color;
    vec3 tangent;
    vec3 normal;
    vec2 texCoord;
} gs_in[];

// Output to fragment shader
layout( location = 0 ) out GS_OUT {
    vec3 wsPosition;
    vec2 texCoord;
    vec3 position;
    vec3 normal;
    vec3 tangent;
    vec4 triangleAABB;
    uint faxis;
} gs_out;


layout(set = 0, binding = 1) uniform uVoxelConfig
{
    mat4 viewProjections[3];
    mat4 viewProjectionsI[3];
    vec4 worldMinPointScale;
} ubo;

layout (push_constant) uniform uNodePushConstant
{
    uint nodeIndex;
    uint materialIndex;
    uint flagStaticVoxels;
    uint volumeDimension;
} pc;

int CalculateAxis()
{
    vec3 p1 = gl_in[1].gl_Position.xyz - gl_in[0].gl_Position.xyz;
    vec3 p2 = gl_in[2].gl_Position.xyz - gl_in[0].gl_Position.xyz;
    vec3 faceNormal = cross(p1, p2);

    float nDX = abs(faceNormal.x);
    float nDY = abs(faceNormal.y);
    float nDZ = abs(faceNormal.z);

    if (nDX > nDY && nDX > nDZ)
        return 0;
    else if (nDY > nDX && nDY > nDZ)
        return 1;
    else
        return 2;
}


vec4 AxisAlignedBoundingBox(vec4 pos[3], vec2 pixelDiagonal)
{
    vec4 aabb;

    aabb.xy = min(pos[2].xy, min(pos[1].xy, pos[0].xy));
    aabb.zw = max(pos[2].xy, max(pos[1].xy, pos[0].xy));

    // enlarge by half-pixel
    aabb.xy -= pixelDiagonal;
    aabb.zw += pixelDiagonal;

    return aabb;
}

bool ConservativeRasterization(in out vec4 pos[3])
{
    vec4 trianglePlane;
    trianglePlane.xyz = cross(pos[1].xyz - pos[0].xyz, pos[2].xyz - pos[0].xyz);
    trianglePlane.xyz = normalize(trianglePlane.xyz);
    trianglePlane.w = -dot(pos[0].xyz, trianglePlane.xyz);

    vec2 texCoord[3];
    for (int i = 0; i < gl_in.length(); i++)
    {
        texCoord[i] = gs_in[i].texCoord;
    }
    // change winding, otherwise there are artifacts for the back faces.
    if (dot(trianglePlane.xyz, vec3(0.0, 0.0, 1.0)) < 0.0)
    {
        vec4 vertexTemp = pos[2];
        vec2 texCoordTemp = texCoord[2];

        pos[2] = pos[1];
        texCoord[2] = texCoord[1];

        pos[1] = vertexTemp;
        texCoord[1] = texCoordTemp;
    }

    vec2 halfPixel = vec2(1.0f / pc.volumeDimension);

    if(trianglePlane.z == 0.0f) return false;
    // expanded aabb for triangle
    gs_out.triangleAABB = AxisAlignedBoundingBox(pos, halfPixel);
    // calculate the plane through each edge of the triangle
    // in normal form for dilatation of the triangle
    vec3 planes[3];
    planes[0] = cross(pos[0].xyw - pos[2].xyw, pos[2].xyw);
    planes[1] = cross(pos[1].xyw - pos[0].xyw, pos[0].xyw);
    planes[2] = cross(pos[2].xyw - pos[1].xyw, pos[1].xyw);
    planes[0].z -= dot(halfPixel, abs(planes[0].xy));
    planes[1].z -= dot(halfPixel, abs(planes[1].xy));
    planes[2].z -= dot(halfPixel, abs(planes[2].xy));
    // calculate intersection between translated planes
    vec3 intersection[3];
    intersection[0] = cross(planes[0], planes[1]);
    intersection[1] = cross(planes[1], planes[2]);
    intersection[2] = cross(planes[2], planes[0]);
    intersection[0] /= intersection[0].z;
    intersection[1] /= intersection[1].z;
    intersection[2] /= intersection[2].z;
    // calculate dilated triangle vertices
    float z[3];
    z[0] = -(intersection[0].x * trianglePlane.x + intersection[0].y * trianglePlane.y + trianglePlane.w) / trianglePlane.z;
    z[1] = -(intersection[1].x * trianglePlane.x + intersection[1].y * trianglePlane.y + trianglePlane.w) / trianglePlane.z;
    z[2] = -(intersection[2].x * trianglePlane.x + intersection[2].y * trianglePlane.y + trianglePlane.w) / trianglePlane.z;
    pos[0].xyz = vec3(intersection[0].xy, z[0]);
    pos[1].xyz = vec3(intersection[1].xy, z[1]);
    pos[2].xyz = vec3(intersection[2].xy, z[2]);

    return true;
}

void main(void)
{
    //Find the axis for the maximize the projected area of this triangle
    int axis = CalculateAxis();
    mat4 viewProjection = ubo.viewProjections[axis];
    mat4 viewProjectionI = ubo.viewProjectionsI[axis];

    vec4 pos[3];
    pos[0] = viewProjection * gl_in[0].gl_Position;
    pos[1] = viewProjection * gl_in[1].gl_Position;
    pos[2] = viewProjection * gl_in[2].gl_Position;


//    if (ConservativeRasterization(pos) == false) return;
    //generate Vertex
    for(uint i = 0; i < gl_in.length(); i++)
    {
        vec4 voxelPos = viewProjectionI * pos[i];
        voxelPos.xyz /= voxelPos.w;
        voxelPos.xyz -= ubo.worldMinPointScale.xyz;
        voxelPos *= ubo.worldMinPointScale.w;
        gs_out.wsPosition = voxelPos.xyz * pc.volumeDimension;

        gl_Position =  pos[i];
        gs_out.position = pos[i].xyz;
        gs_out.tangent = gs_in[i].tangent;
        gs_out.normal = gs_in[i].normal;
        gs_out.texCoord = gs_in[i].texCoord;


        EmitVertex();
    }

    EndPrimitive();
}