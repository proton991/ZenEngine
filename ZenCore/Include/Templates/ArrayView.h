#pragma once

#include <tcb/span.hpp>
#include <type_traits>
namespace zen
{
template <typename T> using ArrayView = tcb::span<T, tcb::dynamic_extent>;

template <typename T> using ArrayViewElement =
    std::conditional_t<std::is_const_v<std::remove_reference_t<T>>,
                       const typename std::decay_t<T>::value_type,
                       typename std::decay_t<T>::value_type>;

template <typename T> constexpr ArrayView<ArrayViewElement<T>> MakeView(T&& value)
{
    return ArrayView<ArrayViewElement<T>>{value.data(), value.size()};
}

template <class T> size_t GetArrayViewSize(T&& value)
{
    return value.size() * sizeof(ArrayViewElement<T>);
}
} // namespace zen
