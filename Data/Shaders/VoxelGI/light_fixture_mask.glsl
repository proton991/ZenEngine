#ifndef ZEN_LIGHT_FIXTURE_MASK
#define ZEN_LIGHT_FIXTURE_MASK
// Cube layout of a local light's fixture mask (LightFixtureMasks): six faces of
// LIGHT_FIXTURE_MASK_SIZE^2 texels per light, layer lightIndex * 6 + face. A face is its major axis
// times two plus one for the negative direction; its texel coordinates are the two other axes in
// increasing order, divided by the major component.
#define LIGHT_FIXTURE_MASK_SIZE 64
// Stored where some sampled ray escapes the shell, and for lights without a mask.
#define LIGHT_FIXTURE_MASK_OPEN 1e30

ivec2 LightFixtureMinorAxes(int axis)
{
    return axis == 0 ? ivec2(1, 2) : (axis == 1 ? ivec2(0, 2) : ivec2(0, 1));
}

// Texel and face holding a direction from the light.
ivec2 LightFixtureMaskTexel(vec3 direction, out int face)
{
    vec3 a       = abs(direction);
    int  axis    = a.x >= a.y && a.x >= a.z ? 0 : (a.y >= a.z ? 1 : 2);
    face         = axis * 2 + (direction[axis] < 0.0 ? 1 : 0);
    ivec2 uvAxes = LightFixtureMinorAxes(axis);
    vec2  uv     = vec2(direction[uvAxes.x], direction[uvAxes.y]) / max(a[axis], 1e-30);
    return clamp(ivec2(floor((uv * 0.5 + 0.5) * float(LIGHT_FIXTURE_MASK_SIZE))), ivec2(0), ivec2(LIGHT_FIXTURE_MASK_SIZE - 1));
}

// Unnormalized direction through a face position; texel units, which may lie beyond the face.
vec3 LightFixtureMaskDirection(int face, vec2 texelPosition)
{
    int   axis       = face / 2;
    ivec2 uvAxes     = LightFixtureMinorAxes(axis);
    vec2  uv         = texelPosition / float(LIGHT_FIXTURE_MASK_SIZE) * 2.0 - 1.0;
    vec3  result     = vec3(0);
    result[axis]     = (face % 2) == 0 ? 1.0 : -1.0;
    result[uvAxes.x] = uv.x;
    result[uvAxes.y] = uv.y;
    return result;
}
#endif
