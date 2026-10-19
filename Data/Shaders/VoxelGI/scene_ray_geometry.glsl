#ifndef ZEN_SCENE_RAY_GEOMETRY
#define ZEN_SCENE_RAY_GEOMETRY
#define VOXEL_GEOMETRY_SET 5
#include "../Common/scene_geometry.glsl"
#include "../Common/material.glsl"
#ifdef RAY_FORWARD_MATERIAL
// Match the forward vertex stage's bindings, shared by raster and ray-hit material evaluation.
layout(std140, set = 1, binding = 2) readonly buffer MaterialBuffer
{
    Material materialData[];
};
layout(std430, set = 1, binding = 3) readonly buffer UVBuffer
{
    vec4 uvValues[];
};
#else
layout(std140, set = 6, binding = 3) readonly buffer MaterialBuffer
{
    Material materialData[];
};
layout(std430, set = 6, binding = 4) readonly buffer UVBuffer
{
    vec4 uvValues[];
};
#endif
// A few ULPs cover surface transform arithmetic. No whole instance is ignored.
vec3 HybridTraceOrigin(vec3 position, vec3 normal, float error)
{
    vec3  shifted = position + normal * (error + 1e-6);
    ivec3 bits    = floatBitsToInt(shifted);
    ivec3 offset  = ivec3(normal * 32.0);
    return intBitsToFloat(bits + mix(offset, -offset, lessThan(shifted, vec3(0))));
}

vec3 RayGeometricNormal(uvec4 triangle)
{
    vec3 a = vertices[indices[triangle.x]].position.xyz;
    vec3 b = vertices[indices[triangle.x + 1u]].position.xyz;
    vec3 c = vertices[indices[triangle.x + 2u]].position.xyz;
    return normalize(mat3(nodesData[triangle.y].normalMatrix) * cross(b - a, c - a));
}

vec3 RaySurfacePosition(uvec4 triangle, vec2 barycentric)
{
    vec3 a = vertices[indices[triangle.x]].position.xyz;
    vec3 b = vertices[indices[triangle.x + 1u]].position.xyz;
    vec3 c = vertices[indices[triangle.x + 2u]].position.xyz;
    // Interpolate on the triangle, preserving constant coordinates exactly.
    // origin+t*direction can round to either side of a cache-cell boundary.
    vec3 local = a + (b - a) * barycentric.x + (c - a) * barycentric.y;
    return (nodesData[triangle.y].modelMatrix * vec4(local, 1)).xyz;
}

// Only alpha-mask geometry is non-opaque in the BLAS. Both faces of single-sided
// surfaces block: raster back-face culling is view-dependent, and ignoring back
// faces would admit sky into closed single-sided shells.
bool RayCandidateAccepted(uvec4 triangle, vec2 barycentric)
{
    Material material = materialData[triangle.z];
    bool     accepted = true;
    if (material.surfaceProperties.y == 1.0)
    {
        vec3  weights            = vec3(1.0 - barycentric.x - barycentric.y, barycentric);
        bool  specularGlossiness = material.surfaceProperties.w > 0.5;
        uint  set    = uint(max(specularGlossiness ? int(material.featureTextures[2].properties.y) : material.bcTexSet, 0));
        uint  stride = uint(uvValues[0].x);
        vec2  uv     = vec2(0);
        float alpha  = 0.0;
        for (uint corner = 0u; corner < 3u; ++corner)
        {
            uint vertex = indices[triangle.x + corner];
            if (set < stride)
            {
                uv += weights[corner] * uvValues[1u + vertex * stride + set].xy;
            }
            alpha += weights[corner] * vertices[vertex].color.a;
        }
        // Ray hits have neither the current fragment's interpolated UVs nor its derivatives.
        // Always apply the hit's transform and level-zero alpha, including in fragment shaders.
        TextureTransform transform = specularGlossiness ? material.featureTextures[2].transform : material.textureTransforms[0];
        int  textureIndex          = specularGlossiness ? int(material.featureTextures[2].properties.x) : material.bcTexIndex;
        vec2 transformed           = vec2(dot(transform.row0.xyz, vec3(uv, 1)), dot(transform.row1.xyz, vec3(uv, 1)));
        alpha *= specularGlossiness ? material.diffuseFactor.a : material.baseColorFactor.a;
        if (textureIndex >= 0)
        {
            alpha *= textureLod(sampler2D(uTexture2DHeap[nonuniformEXT(textureIndex)],
                                          uSamplerHeap[nonuniformEXT(int(transform.row0.w))]),
                                transformed, 0.0)
                         .a;
        }
        accepted = MaterialVisible(material, alpha);
    }
    // Blend/transmission are opaque blockers under the query lighting contract.
    return accepted;
}

#endif
