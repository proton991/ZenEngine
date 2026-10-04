#version 450
#extension GL_GOOGLE_include_directive : require
#include "../Common/bindless_heap.glsl"
#include "../Common/scene_lighting.glsl"
layout(set=1,binding=0) uniform uCameraData
{
    mat4 uProjViewMatrix;
    mat4 uProjMatrix;
    mat4 uViewMatrix;
};
layout(location=0) out vec3 sphereNormal;
layout(location=1) flat out vec3 sphereColor;

void main()
{
    // 24 longitude segments, 12 latitude bands, two triangles per cell.
    const ivec2 offsets[6]=ivec2[6](ivec2(0,0),ivec2(1,0),ivec2(0,1),
                                  ivec2(0,1),ivec2(1,0),ivec2(1,1));
    int cell=gl_VertexIndex/6;
    ivec2 grid=ivec2(cell%24,cell/24)+offsets[gl_VertexIndex%6];
    float longitude=float(grid.x)*6.28318530718/24.0;
    float latitude=float(grid.y)*3.14159265359/12.0;
    sphereNormal=vec3(sin(latitude)*cos(longitude),cos(latitude),sin(latitude)*sin(longitude));
    sphereColor=sceneUbo.cameraLight.colorIntensity.rgb;
    vec3 position=sceneUbo.cameraLight.positionRange.xyz+sphereNormal*sceneUbo.cameraLight.coneShadow.w;
    gl_Position=uProjViewMatrix*vec4(position,1);
}
