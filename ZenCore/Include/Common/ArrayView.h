#pragma once

#include <span>
namespace zen
{
template <typename T>
using ArrayView = std::span<T, std::dynamic_extent>;

template <typename T>
constexpr auto MakeView(T&& v)
{
    using ValueType = typename std::decay_t<T>::value_type;
    using FinalType = std::conditional_t<std::is_const_v<std::remove_reference_t<T>>, const ValueType, ValueType>;
    return ArrayView<FinalType>{v.data(), v.size()};
}
} // namespace zen