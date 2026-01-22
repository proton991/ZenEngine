#pragma once
#include <cstdint>
#include <type_traits>

namespace zen
{

template <uint32_t BitCount, typename StorageTypte = uint32_t> class BitMask
{
    static_assert(BitCount > 0, "BitCount must be > 0");
    static_assert(BitCount <= sizeof(StorageTypte) * 8, "BitCount exceeds storage capacity");

public:
    using storage_type = StorageTypte;

    constexpr BitMask() = default;

    // ---------------- Basic Ops ----------------
    void Set(uint32_t idx)
    {
        mask_ |= (storage_type(1) << idx);
    }

    void Clear(uint32_t idx)
    {
        mask_ &= ~(storage_type(1) << idx);
    }

    bool Test(uint32_t idx) const
    {
        return (mask_ & (storage_type(1) << idx)) != 0;
    }

    void Reset()
    {
        mask_ = 0;
    }

    storage_type Raw() const
    {
        return mask_;
    }

    uint32_t Count() const
    {
#if defined(_MSC_VER)
        return static_cast<uint32_t>(__popcnt(mask_));
#elif defined(__GNUC__) || defined(__clang__)
        return static_cast<uint32_t>(__builtin_popcount(mask_));
#else
        uint32_t count = 0;
        storage_type v = mask_;
        while (v)
        {
            v &= (v - 1); // Brian Kernighan
            ++count;
        }
        return count;
#endif
    }

    bool Any() const
    {
        return mask_ != 0;
    }

    bool None() const
    {
        return mask_ == 0;
    }

    bool All() const
    {
        return mask_ == ((storage_type(1) << BitCount) - 1);
    }

    // ---------------- Iterator ----------------
    class Iterator
    {
    public:
        Iterator(storage_type mask, uint32_t idx) : mask_(mask), idx_(idx)
        {
            Advance();
        }

        uint32_t operator*() const
        {
            return idx_;
        }

        Iterator& operator++()
        {
            ++idx_;
            Advance();
            return *this;
        }

        bool operator!=(const Iterator& other) const
        {
            return idx_ != other.idx_;
        }

    private:
        void Advance()
        {
            while (idx_ < BitCount)
            {
                if (mask_ & (storage_type(1) << idx_))
                    break;
                ++idx_;
            }
        }

        storage_type mask_;
        uint32_t idx_;
    };

    Iterator begin() const
    {
        return Iterator{mask_, 0};
    }

    Iterator end() const
    {
        return Iterator{mask_, BitCount};
    }

private:
    storage_type mask_ = 0;
};

template <typename EnumType, typename StorageType = uint32_t> using EnumBitMask =
    BitMask<static_cast<uint32_t>(EnumType::eMax), StorageType>;

} // namespace zen
