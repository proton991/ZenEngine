#ifndef ZEN_CAMERA_RAY_GLSL
#define ZEN_CAMERA_RAY_GLSL
vec3 CameraBackgroundRay(vec2 uv, mat4 projectionView, mat4 projection, mat4 view, vec3 eye)
{
    // Perspective x/y reconstruction depends only on the focal scales and
    // projection offsets. Avoid the depth terms so finite and infinite far
    // planes produce identical rays, even for translated cameras.
    vec2 ndc = uv * 2.0 - 1.0;
    vec2 offsets = vec2(projection[2][0], projection[2][1]);
    vec2 focalScales = vec2(projection[0][0], projection[1][1]);
    vec3 viewDirection = vec3((ndc + offsets) / focalScales, -1.0);
    return projection[3][3] > 0.5 ? -normalize(transpose(mat3(view))[2]) :
        normalize(transpose(mat3(view)) * viewDirection);
}
#endif
