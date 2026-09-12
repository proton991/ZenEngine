#pragma once

#include <concepts>
#include <cstddef>
#include <iterator>
#include <type_traits>
#include <utility>

namespace zen
{
template <typename Container>
concept VectorViewCompatibleContainer = requires(Container& container) {
    std::data(container);
    { std::size(container) } -> std::convertible_to<size_t>;
} && std::is_pointer_v<decltype(std::data(std::declval<Container&>()))>;

// A non-owning view over contiguous elements. Constness is controlled by T.
template <typename T> class VectorView
{
public:
    T& operator[](size_t i)
    {
        return m_pData[i];
    }

    const T& operator[](size_t i) const
    {
        return m_pData[i];
    }

    bool empty() const
    {
        return m_size == 0;
    }

    size_t size() const
    {
        return m_size;
    }

    T* data()
    {
        return m_pData;
    }

    const T* data() const
    {
        return m_pData;
    }

    T* begin()
    {
        return m_pData;
    }

    T* end()
    {
        return m_pData + m_size;
    }

    const T* begin() const
    {
        return m_pData;
    }

    const T* end() const
    {
        return m_pData + m_size;
    }

    T& front()
    {
        return m_pData[0];
    }

    const T& front() const
    {
        return m_pData[0];
    }

    T& back()
    {
        return m_pData[m_size - 1];
    }

    const T& back() const
    {
        return m_pData[m_size - 1];
    }

    // Copying a view copies only its pointer and element count.
    VectorView(const VectorView&) = default;

    VectorView& operator=(const VectorView&) = default;

    VectorView() = default;

    VectorView(T& element) : m_pData(&element), m_size(1) {}

    VectorView(T* pPtr, size_t size) : m_pData(pPtr), m_size(size) {}

    template <typename U>
        requires std::is_convertible_v<U*, T*>
    VectorView(const VectorView<U>& other) : m_pData(other.data()), m_size(other.size())
    {}

    template <typename Container>
        requires(std::is_lvalue_reference_v<Container &&> &&
                 VectorViewCompatibleContainer<std::remove_reference_t<Container>> &&
                 std::is_convertible_v<decltype(std::data(std::declval<Container&>())), T*>)
    VectorView(Container&& container) :
        m_pData(std::data(container)), m_size(static_cast<size_t>(std::size(container)))
    {}

private:
    T* m_pData{nullptr};
    size_t m_size{0};
};

// Pointer + size
template <typename T> VectorView<T> MakeVecView(T* pPtr, size_t size)
{
    return VectorView<T>(pPtr, size);
}

// Const pointer + size
template <typename T> VectorView<const T> MakeVecView(const T* pPtr, size_t size)
{
    return VectorView<const T>(pPtr, size);
}

// Any contiguous, sized container with a data() function.
template <VectorViewCompatibleContainer Container> using VectorViewElement =
    std::remove_pointer_t<decltype(std::data(std::declval<Container&>()))>;

template <VectorViewCompatibleContainer Container>
VectorView<VectorViewElement<Container>> MakeVecView(Container& container)
{
    return VectorView<VectorViewElement<Container>>(container);
}

// Single element
template <typename T>
    requires(!VectorViewCompatibleContainer<T>)
VectorView<T> MakeVecView(T& element)
{
    return VectorView<T>(element);
}
} // namespace zen
