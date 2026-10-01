#version 450
#extension GL_GOOGLE_include_directive : require
#include "../Common/bindless_heap.glsl"
#include "../Common/scene_lighting.glsl"
#include "../Common/linear_to_srgb.glsl"
#include "../Common/camera_ray.glsl"
layout(location=0) in vec2 inUV;
layout(location=0) out vec4 outColor;
layout(set=1,binding=0) uniform uCameraData
{
    mat4 uProjViewMatrix;
    mat4 uProjMatrix;
    mat4 uViewMatrix;
};
layout(set=1,binding=1) uniform samplerCube samplerEnv;
void main()
{
    vec3 direction = CameraBackgroundRay(inUV, uProjViewMatrix, uProjMatrix, uViewMatrix,
                                         sceneUbo.viewPosition.xyz);
    vec3 color=textureLod(samplerEnv,EnvironmentSourceDirection(direction),0).rgb *
        sceneUbo.environment.x * sceneUbo.environment.w;
    color=color/(color+vec3(1));
    outColor=vec4(LinearToSRGB(max(color,vec3(0))),1);
    gl_FragDepth=1.0;
}
