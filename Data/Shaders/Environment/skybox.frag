#version 450
#extension GL_GOOGLE_include_directive : require
#include "../Common/bindless_heap.glsl"
#include "../Common/scene_lighting.glsl"
layout(location=0) in vec3 inUVW;
layout(location=0) out vec4 outColor;
layout(set=1,binding=1) uniform samplerCube samplerEnv;
void main()
{
    vec3 color=textureLod(samplerEnv,EnvironmentSourceDirection(normalize(inUVW)),0).rgb *
        sceneUbo.environment.x * sceneUbo.environment.w;
    color=color/(color+vec3(1));
    outColor=vec4(pow(max(color,vec3(0)),vec3(1.0/2.2)),1);
    gl_FragDepth=1.0;
}
