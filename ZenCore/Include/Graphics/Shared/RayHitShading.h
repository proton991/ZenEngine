#pragma once
#include <cstddef>
#include "SceneGraph/Material.h"

namespace zen::rc
{
// Source-precision secondary-hit attributes. Position stays in the BLAS/raster stream.
struct RayHitVertex
{
    Vec4 normal;
    Vec4 tangent;
    Vec4 color;
};

// Four bindings: base/diffuse, metallic-roughness/specular, normal, emission.
// Mirrors ray_hit_material.glsl, std430. No fragment-only material extensions.
struct RayHitMaterial
{
    Vec4                    baseColorFactor;
    Vec4                    emissiveFactor;
    Vec4                    surfaceProperties;
    Vec4                    specularFactor;
    glm::uvec4              normalEncoding;
    Vec4                    textureSampling;
    Vec4                    factors; // metallic factor in x
    sg::MaterialTextureData textures[4];
};

static_assert(sizeof(RayHitVertex) == 48);
static_assert(sizeof(RayHitMaterial) == 368);
static_assert(offsetof(RayHitMaterial, textures) == 112);
} // namespace zen::rc
