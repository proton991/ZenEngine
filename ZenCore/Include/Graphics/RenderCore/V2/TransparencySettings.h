#pragma once
#include <cstdint>

namespace zen::rc
{
enum class TransparencyCoreMode : uint32_t
{
    eAutomatic,
    eDisabled,
    eForce
};

struct TransparencySettings
{
    TransparencyCoreMode coreMode{TransparencyCoreMode::eAutomatic};
    float                coreCutoff{0.99f};
    bool                 operator==(const TransparencySettings&) const = default;
};

bool ValidateTransparencySettings(const TransparencySettings& settings);

} // namespace zen::rc
