#include "../Common/bindless_heap.glsl"
#include "../Common/scene_lighting.glsl"
#include "../Common/camera_ray.glsl"
layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;
layout(set = 1, binding = 0) uniform uCameraData
{
    mat4 uProjViewMatrix;
    mat4 uProjMatrix;
    mat4 uViewMatrix;
};
layout(set = 3, binding = 4) uniform samplerCube envSourceMap;
#ifdef GBUFFER_DEPTH
layout(set = 3, binding = 5) uniform sampler2D depthMap;
#endif
void main()
{
    vec3 direction = CameraBackgroundRay(inUV, uProjViewMatrix, uProjMatrix, uViewMatrix, sceneUbo.viewPosition.xyz);
    vec3 color     = textureLod(envSourceMap, EnvironmentSourceDirection(direction), 0.0).rgb * sceneUbo.environment.x
                   * sceneUbo.environment.w;
    outColor       = vec4(color, 1.0);
#ifdef GBUFFER_DEPTH
    // Prefills the hybrid receivers' depth so forward opaque triangles shade each pixel once.
    gl_FragDepth = texelFetch(depthMap, ivec2(gl_FragCoord.xy), 0).r;
#endif
}
