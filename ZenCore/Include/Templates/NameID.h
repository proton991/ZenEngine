#pragma once
#include "HashMap.h"
#include "HeapVector.h"
#include "Utils/Mutex.h"

#include <functional>

namespace zen
{
namespace detail
{
class NameRegistry
{
public:
    NameRegistry(const NameRegistry&) = delete;

    NameRegistry& operator=(const NameRegistry&) = delete;

    static NameRegistry& GetInstance()
    {
        static NameRegistry sRegistry;
        return sRegistry;
    }

    uint32_t GenerateId(const char* pName, uint32_t len)
    {
        uint32_t nameId = 0;

        if (pName != nullptr && len != 0)
        {
            const uint32_t hash = HashBytes(pName, len);

            LockAuto lock(&m_mutex);

            std::unordered_map<uint32_t, uint32_t>::iterator it = m_lut.find(hash);

            if (it != m_lut.end())
            {
                const uint8_t* pData = GetRecordRawData(it->second);

                if (GetRecordLength(pData) == len &&
                    std::memcmp(GetRecordChars(pData), pName, len) == 0)
                {
                    nameId = it->second;
                }
                else
                {
                    VERIFY_EXPR_MSG(false, "Name ID has collision detected");
                }
            }
            else
            {
                nameId      = WriteRecord(pName, len, hash);
                m_lut[hash] = nameId;
            }
        }

        return nameId;
    }

    const char* CStr(uint32_t nameId)
    {
        return GetRecordChars(GetRecordRawData(nameId));
    }

    uint32_t Length(uint32_t nameId)
    {
        return GetRecordLength(GetRecordRawData(nameId));
    }

    uint32_t Hash(uint32_t nameId)
    {
        return GetRecordHash(GetRecordRawData(nameId));
    }

private:
    NameRegistry()
    {
        m_blocks.reserve(kMaxBlocks);
        AllocBlock();

        const uint32_t nodeId = WriteRecord("", 0, HashBytes("", 0));
        VERIFY_EXPR(nodeId == 0);
    }

    ~NameRegistry()
    {
        for (RecordBlock& block : m_blocks)
        {
            ZEN_MEM_FREE(block.pData);
        }
    }

    // Storage: [RecordHeader][string content][\0]
    struct RecordHeader
    {
        uint32_t hash;
        uint32_t len;
    };

    struct RecordBlock
    {
        uint8_t* pData{nullptr};
        uint32_t used{0};
    };

    static constexpr uint32_t kOffsetBits = 16;
    static constexpr uint32_t kOffsetMask = (1u << kOffsetBits) - 1;
    static constexpr uint32_t kBlockSize  = 1u << kOffsetBits;
    static constexpr uint32_t kMaxBlocks  = 1u << (32 - kOffsetBits);
    static constexpr uint32_t kMaxStringLen =
        kBlockSize - static_cast<uint32_t>(sizeof(RecordHeader)) - 1;

    static_assert(sizeof(RecordHeader) == 8, "RecordHeader size does not equal to 8");

    static uint32_t MakeId(uint32_t blockIdx, uint32_t offset)
    {
        return (blockIdx << kOffsetBits) | offset;
    }

    const uint8_t* GetRecordRawData(uint32_t id)
    {
        const uint32_t blockIdx = id >> kOffsetBits;
        const uint32_t offset   = id & kOffsetMask;

        return m_blocks[blockIdx].pData + offset;
    }

    static RecordHeader GetRecordHeader(const uint8_t* pData)
    {
        RecordHeader header{};
        std::memcpy(&header, pData, sizeof(header));

        return header;
    }

    static uint32_t GetRecordHash(const uint8_t* pData)
    {
        return GetRecordHeader(pData).hash;
    }

    static uint32_t GetRecordLength(const uint8_t* pData)
    {
        return GetRecordHeader(pData).len;
    }

    static const char* GetRecordChars(const uint8_t* pData)
    {
        return reinterpret_cast<const char*>(pData + sizeof(RecordHeader));
    }

    static uint32_t HashBytes(const char* pStr, uint32_t len)
    {
        uint32_t hash = 2166136261u;

        for (uint32_t i = 0; i < len; ++i)
        {
            hash ^= static_cast<uint8_t>(pStr[i]);
            hash *= 16777619u;
        }

        return hash;
    }

    void AllocBlock()
    {
        VERIFY_EXPR_MSG(m_blocks.size() < kMaxBlocks, "NameID pool exhausted (too many blocks)");

        RecordBlock block{};
        block.pData = static_cast<uint8_t*>(ZEN_MEM_ALLOC(kBlockSize));
        block.used  = 0;
        m_blocks.push_back(block);
    }

    uint32_t WriteRecord(const char* pStr, uint32_t len, uint32_t hash)
    {
        const uint32_t recordSize = static_cast<uint32_t>(sizeof(RecordHeader)) + len + 1;

        RecordBlock* pBlock = &m_blocks.back();

        if (pBlock->used + recordSize > kBlockSize)
        {
            AllocBlock();
            pBlock = &m_blocks.back();
        }

        const uint32_t blockIdx = static_cast<uint32_t>(m_blocks.size() - 1);
        const uint32_t offset   = pBlock->used;
        uint8_t* rec            = pBlock->pData + offset;

        const RecordHeader header{hash, len};
        std::memcpy(rec, &header, sizeof(header));
        std::memcpy(rec + sizeof(header), pStr, len);
        rec[sizeof(header) + len] = '\0';

        pBlock->used += recordSize;

        return MakeId(blockIdx, offset);
    }

    mutable Mutex m_mutex;
    HeapVector<RecordBlock> m_blocks;
    HashMap<uint32_t, uint32_t> m_lut; /// hash -> nameId look up table
};
} // namespace detail

class NameID
{
public:
    NameID() = default;

    NameID(const char* pName)
    {
        m_id = detail::NameRegistry::GetInstance().GenerateId(
            pName, static_cast<uint32_t>(std::strlen(pName)));
    }

    NameID(const char* pName, uint32_t len)
    {
        m_id = detail::NameRegistry::GetInstance().GenerateId(pName, len);
    }

    NameID(const std::string& str)
    {
        m_id = detail::NameRegistry::GetInstance().GenerateId(str.data(),
                                                              static_cast<uint32_t>(str.size()));
    }

    bool IsNone() const
    {
        return m_id == 0;
    }

    const char* CStr() const
    {
        return detail::NameRegistry::GetInstance().CStr(m_id);
    }

    uint32_t Length() const
    {
        return detail::NameRegistry::GetInstance().Length(m_id);
    }

    std::string ToString() const
    {
        return {CStr(), Length()};
    }

    bool operator==(const NameID& rhs) const
    {
        return m_id == rhs.m_id;
    }

    bool operator!=(const NameID& rhs) const
    {
        return m_id != rhs.m_id;
    }

    bool operator<(const NameID& rhs) const
    {
        return std::strcmp(CStr(), rhs.CStr()) < 0;
    }

private:
    uint32_t GetId() const
    {
        return m_id;
    }

    uint32_t m_id{0};

    friend struct std::hash<NameID>;
};

static_assert(sizeof(NameID) == 4, "NameID must be 4 byte");
} // namespace zen

template <> struct std::hash<zen::NameID>
{
    size_t operator()(const zen::NameID& nameID) const noexcept
    {
        return zen::detail::NameRegistry::GetInstance().Hash(nameID.GetId());
    }
};
