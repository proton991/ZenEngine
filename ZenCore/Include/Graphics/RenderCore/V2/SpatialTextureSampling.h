#pragma once
#include "Math/Math.h"
#include "Templates/VectorView.h"

namespace zen::rc
{
enum class SpatialTextureRounding : uint32_t
{
    eUnqualified = 0,
    eHalfUp      = 1,
    eTiesToEven  = 2
};

// Triplets contain native, half-up and ties-to-even samples. Both coordinate
// axes must match exactly; ambiguous or nonfinite observations are unsupported.
SpatialTextureRounding ClassifySpatialTextureSampling(VectorView<const Vec4> observations);
} // namespace zen::rc
