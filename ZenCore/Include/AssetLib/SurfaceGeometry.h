#pragma once
#include "Math/Math.h"
#include "AssetLib/Types.h"
#include "Templates/HeapVector.h"
#include <cmath>

namespace zen::asset
{
// Requires validated triangle indices and one source mapping per input vertex.
void GenerateFlatNormals(HeapVector<Vertex>& vertices, HeapVector<uint32_t>& indices, HeapVector<uint32_t>& sourceVertices);

inline bool NormalizeSurfaceNormal(Vec3& normal)
{
    const float lengthSquared = glm::dot(normal, normal);

    const bool valid          = std::isfinite(lengthSquared) && lengthSquared > 1e-12f;

    if (valid)
    {
        normal /= std::sqrt(lengthSquared);
    }

    return valid;
}

} // namespace zen::asset
