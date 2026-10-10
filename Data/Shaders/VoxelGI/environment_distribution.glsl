#ifndef ZEN_ENVIRONMENT_DISTRIBUTION
#define ZEN_ENVIRONMENT_DISTRIBUTION
#include "Graphics/Shared/EnvironmentSampling.h"

vec3 EnvironmentCubeDirection(uint face, vec2 uv)
{
    vec3 direction;
    if(face == 0u) direction = vec3(1, -uv.y, -uv.x);
    else if(face == 1u) direction = vec3(-1, -uv.y, uv.x);
    else if(face == 2u) direction = vec3(uv.x, 1, uv.y);
    else if(face == 3u) direction = vec3(uv.x, -1, -uv.y);
    else if(face == 4u) direction = vec3(uv.x, -uv.y, 1);
    else direction = vec3(-uv.x, -uv.y, -1);
    return normalize(direction);
}

float EnvironmentCubeJacobian(vec2 uv)
{
    float squaredLength = 1.0 + dot(uv, uv);
    return 1.0 / (squaredLength * sqrt(squaredLength));
}

// Real spherical harmonics through order 2: L00, L1-1, L10, L11, L2-2, L2-1, L20, L21, L22.
void EnvironmentHarmonicBasis(vec3 d, out float basis[ZEN_ENVIRONMENT_HARMONIC_COEFFICIENTS])
{
    basis[0] = 0.282095;
    basis[1] = 0.488603 * d.y;
    basis[2] = 0.488603 * d.z;
    basis[3] = 0.488603 * d.x;
    basis[4] = 1.092548 * d.x * d.y;
    basis[5] = 1.092548 * d.y * d.z;
    basis[6] = 0.315392 * (3.0 * d.z * d.z - 1.0);
    basis[7] = 1.092548 * d.x * d.z;
    basis[8] = 0.546274 * (d.x * d.x - d.y * d.y);
}

// Irradiance, integral of L * max(dot(n, w), 0), from the order-2 projection
// (Ramamoorthi and Hanrahan, "An Efficient Representation for Irradiance Environment Maps", 2001).
// The clamped-cosine kernel makes the omitted orders small, so the result is smooth in n
// even for small, very bright sources.
vec3 EnvironmentHarmonicIrradiance(vec3 d, vec3 L[ZEN_ENVIRONMENT_HARMONIC_COEFFICIENTS])
{
    const float c1 = 0.429043, c2 = 0.511664, c3 = 0.743125, c4 = 0.886227, c5 = 0.247708;
    return c1 * L[8] * (d.x * d.x - d.y * d.y) + c3 * L[6] * d.z * d.z + c4 * L[0] - c5 * L[6]
        + 2.0 * c1 * (L[4] * d.x * d.y + L[7] * d.x * d.z + L[5] * d.y * d.z)
        + 2.0 * c2 * (L[3] * d.x + L[1] * d.y + L[2] * d.z);
}
#endif
