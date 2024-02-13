#pragma once
#include <vector>
#include <stdexcept>

namespace zen::util
{
template <class T> inline void HashCombine(size_t& seed, const T& v)
{
    std::hash<T> hasher;
    seed ^= hasher(v) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
}

template <class T> inline std::vector<uint8_t> ToBytes(const T& value)
{
    return std::vector<uint8_t>{reinterpret_cast<const uint8_t*>(&value),
                                reinterpret_cast<const uint8_t*>(&value) + sizeof(T)};
}

template <class T> inline uint32_t ToU32(T value)
{
    static_assert(std::is_arithmetic<T>::value, "T must be numeric");

    if (static_cast<uintmax_t>(value) >
        static_cast<uintmax_t>(std::numeric_limits<uint32_t>::max()))
    {
        throw std::runtime_error("to_u32() failed, value is too big to be converted to uint32_t");
    }

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
    using UintHandle =
        typename std::conditional<sizeof(THandle) == sizeof(uint32_t), uint32_t, uint64_t>::type;

    return static_cast<uint64_t>(reinterpret_cast<UintHandle>(handle));
}
} // namespace zen::util