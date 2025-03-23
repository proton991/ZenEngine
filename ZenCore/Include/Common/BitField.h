#pragma once
#include <cstdint>

namespace zen
{
template <typename T> class BitField
{
public:
    constexpr BitField() = default;

    BitField<T>& SetFlag(T flag)
    {
        m_value |= static_cast<int64_t>(flag);
        return *this;
    }

    template <typename... Flags> BitField<T>& SetFlags(Flags... flags)
    {
        uint32_t values = (... | (uint32_t)flags);
        m_value |= values;
        return *this;
    }

    BitField<T>& SetFlag(const BitField<T>& b)
    {
        m_value |= b.m_value;
        return *this;
    }

    bool HasFlag(T flag) const
    {
        return m_value & static_cast<int64_t>(flag);
    }

    bool IsEmpty() const
    {
        return m_value == 0;
    }

    void ClearFlag(T flag)
    {
        m_value &= ~static_cast<int64_t>(flag);
    }

    void Clear()
    {
        m_value = 0;
    }

    constexpr explicit BitField(int64_t value)
    {
        m_value = value;
    }

    constexpr explicit BitField(T value)
    {
        m_value = static_cast<int64_t>(value);
    }

    BitField<T> operator^(const BitField<T>& b) const
    {
        return BitField<T>(m_value ^ b.m_value);
    }

    operator int64_t() const
    {
        return m_value;
    }

private:
    int64_t m_value = 0;
};
} // namespace zen