#pragma once
#include <vector>
#include "Utils/Errors.h"
#include <limits>
#include <cmath>

namespace zen::util
{
// todo: refactor Hash functions
inline void HashCombine32(uint32_t& seed, uint32_t value)
{
    // 32-bit hash combine (boost-like)
    seed ^= value + 0x9e3779b9u + (seed << 6) + (seed >> 2);
}

template <class T> inline void HashCombine32T(uint32_t& seed, const T& v)
{
    std::hash<T> hasher;
    seed ^= hasher(v) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
}

template <class T> inline void HashCombine(size_t& seed, const T& v)
{
    std::hash<T> hasher;
    seed ^= hasher(v) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
}

template <class T> inline std::vector<uint8_t> ToBytes(const T& value)
{
    return std::vector<uint8_t>{reinterpret_cast<const uint8_t*>(&value), reinterpret_cast<const uint8_t*>(&value) + sizeof(T)};
}

template <class T> inline uint32_t ToU32(T value)
{
    static_assert(std::is_arithmetic<T>::value, "T must be numeric");

    const long double numeric = static_cast<long double>(value);

    VERIFY_EXPR_MSG(std::isfinite(numeric) && numeric >= 0 && numeric <= std::numeric_limits<uint32_t>::max(),
                    "ToU32 value is outside the uint32_t range");

    return static_cast<uint32_t>(value);
}

template <class VkType, class Type> inline VkType ToVkType(Type type)
{
    return static_cast<VkType>(type);
}

template <class THandle> uint64_t VkHandleToU64(THandle handle)
{
    // See https://github.com/KhronosGroup/Vulkan-Docs/issues/368 .
    // Dispatchable and non-dispatchable handle types are *not* necessarily binary-compatible!
    // Non-dispatchable handles _might_ be only 32-bit long. This is because, on 32-bit machines, they might be a typedef to a 32-bit pointer.
    using UintHandle = typename std::conditional<sizeof(THandle) == sizeof(uint32_t), uint32_t, uint64_t>::type;

    return static_cast<uint64_t>(reinterpret_cast<UintHandle>(handle));
}
} // namespace zen::util