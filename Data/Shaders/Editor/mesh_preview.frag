#version 450

// Matches MeshPreviewShading, plus the wireframe overlay.
const uint kFlat      = 0;
const uint kSmooth    = 1;
const uint kNormals   = 2;
const uint kWireframe = 3;

layout(push_constant) uniform MeshPreviewConstants
{
    mat4 viewProjection;
    vec4 eye;
    uint mode;
} constants;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;

layout(location = 0) out vec4 outColor;

// Written directly as SDR encoded color, like the rest of the editor UI.
void main()
{
    const vec3 toEye      = normalize(constants.eye.xyz - inPosition);

    const vec3 faceNormal = normalize(cross(dFdx(inPosition), dFdy(inPosition)));

    // Missing vertex normals fall back to the face normal.
    const vec3 vertexNormal = dot(inNormal, inNormal) > 1e-8 ? normalize(inNormal) : faceNormal;

    vec3 color;

    if (constants.mode == kWireframe)
    {
        color = vec3(0.95, 0.72, 0.25);
    }
    else if (constants.mode == kNormals)
    {
        color = vertexNormal * 0.5 + 0.5;
    }
    else
    {
        vec3 normal = constants.mode == kFlat ? faceNormal : vertexNormal;

        // Two-sided headlight: back faces are lit as if facing the camera.
        normal      = dot(normal, toEye) < 0.0 ? -normal : normal;

        color       = vec3(0.78) * (0.22 + 0.78 * max(dot(normal, toEye), 0.0));
    }

    outColor = vec4(color, 1.0);
}
