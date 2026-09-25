#version 450
#extension GL_GOOGLE_include_directive : require
#include "../Common/bindless_heap.glsl"
#include "../Common/scene_lighting.glsl"

layout(set=1, binding=0) uniform uCameraData
{
    mat4 uProjViewMatrix;
    mat4 uProjMatrix;
    mat4 uViewMatrix;
};
layout(push_constant) uniform MarkerSettings
{
    float size;
} marker;
layout(location=0) flat out vec3 markerColor;
layout(location=1) out vec3 boxPosition;

// A unit box drawn without a separate model, material, or per-light vertex buffer.
const vec3 corners[8] = vec3[8](
    vec3(-0.5,-0.5,-0.5), vec3(0.5,-0.5,-0.5),
    vec3(-0.5, 0.5,-0.5), vec3(0.5, 0.5,-0.5),
    vec3(-0.5,-0.5, 0.5), vec3(0.5,-0.5, 0.5),
    vec3(-0.5, 0.5, 0.5), vec3(0.5, 0.5, 0.5));
const int indices[36] = int[36](
    0,2,1, 1,2,3, 4,5,6, 5,7,6,
    0,4,2, 2,4,6, 1,3,5, 3,7,5,
    2,6,3, 3,6,7, 0,1,4, 1,5,4);
const float faceBrightness[6] = float[6](0.85,0.85,0.7,0.7,1.0,0.65);

void main()
{
    SceneLight light = sceneUbo.lights[gl_InstanceIndex];
    markerColor = light.colorIntensity.rgb * light.colorIntensity.w *
        faceBrightness[gl_VertexIndex / 6];
    boxPosition = corners[indices[gl_VertexIndex]];
    gl_Position = vec4(2.0,2.0,2.0,1.0);
    // Directional lights have no finite source position. Disabled lights are absent from the snapshot.
    if (int(light.directionType.w) != 0 && any(greaterThan(markerColor,vec3(0.0))))
    {
        vec3 position = light.positionRange.xyz + boxPosition * marker.size;
        gl_Position = uProjViewMatrix * vec4(position,1.0);
    }
}
