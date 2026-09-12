#pragma once
#include <cstring>
#include <type_traits>
#include "Graphics/RHI/RHICommon.h"
#include "RHIResource.h"
#include "Templates/VectorView.h"

namespace zen
{
struct RHIShaderValueParameter
{
    uint32_t set{0};
    uint32_t binding{0};
    uint32_t byteOffset{0};
    uint32_t byteSize{0};

    RHIShaderValueParameter() = default;

    RHIShaderValueParameter(uint32_t inSet,
                            uint32_t inBinding,
                            uint32_t inByteOffset,
                            uint32_t inByteSize) :
        set(inSet), binding(inBinding), byteOffset(inByteOffset), byteSize(inByteSize)
    {}
};

struct RHIShaderResourceParameter
{
    uint32_t set{0};
    uint32_t binding{0};
    uint32_t arrayIndex{0};

    // Combined resources use the texture/buffer as pResource and its sampler as pAuxResource.
    RHIResource* pResource{nullptr};
    RHIResource* pAuxResource{nullptr};
    RHIShaderResourceType resourceType{RHIShaderResourceType::eMax};

    RHIShaderResourceParameter() = default;

    RHIShaderResourceParameter(uint32_t inSet,
                               uint32_t inBinding,
                               uint32_t inArrayIndex,
                               RHIResource* pInResource,
                               RHIResource* pInAuxResource,
                               RHIShaderResourceType inResourceType) :
        set(inSet),
        binding(inBinding),
        arrayIndex(inArrayIndex),
        pResource(pInResource),
        pAuxResource(pInAuxResource),
        resourceType(inResourceType)
    {}
};

class RHIBatchedShaderParameters
{
public:
    RHIBatchedShaderParameters() = default;

    RHIBatchedShaderParameters(RHIBatchedShaderParameters&&) = default;

    RHIBatchedShaderParameters& operator=(RHIBatchedShaderParameters&&) = default;

    RHIBatchedShaderParameters(const RHIBatchedShaderParameters&) = delete;

    RHIBatchedShaderParameters& operator=(const RHIBatchedShaderParameters&) = delete;

    void AddValueParam(const RHIShaderResourceDescriptor& srd, const void* pData, uint32_t numBytes)
    {
        if (pData != nullptr && numBytes > 0)
        {
            const uint32_t offset = static_cast<uint32_t>(m_valueData.size());
            const uint8_t* pBytes = static_cast<const uint8_t*>(pData);
            m_valueData.insert(m_valueData.end(), pBytes, pBytes + numBytes);
            m_valueParameters.emplace_back(srd.set, srd.binding, offset, numBytes);
        }
    }

    template <typename T> void AddValueParam(const RHIShaderResourceDescriptor& srd, const T& value)
    {
        AddValueParam(srd, &value, static_cast<uint32_t>(sizeof(T)));
    }

    void AddResourceParam(const RHIShaderResourceDescriptor& srd,
                          RHIResource* pResource,
                          RHIResource* pAuxResource,
                          uint32_t arrayIndex)
    {
        if (srd.bindless)
        {
            m_bindlessParameters.emplace_back(srd.set, srd.binding, arrayIndex, pResource,
                                              pAuxResource, srd.type);
        }
        else
        {
            m_resourceParameters.emplace_back(srd.set, srd.binding, arrayIndex, pResource,
                                              pAuxResource, srd.type);
        }
    }

    bool HasAnyParameter() const
    {
        return !m_valueParameters.empty() || !m_resourceParameters.empty() ||
            !m_bindlessParameters.empty();
    }

    void Reset()
    {
        m_valueData.clear();
        m_valueParameters.clear();
        m_resourceParameters.clear();
        m_bindlessParameters.clear();
    }

    // Preserve offsets and descriptor metadata while giving recorded commands their own bytes.
    void CopyFrom(const RHIBatchedShaderParameters& source)
    {
        if (this != &source)
        {
            CopyParameters(m_valueData, source.m_valueData);
            CopyParameters(m_valueParameters, source.m_valueParameters);
            CopyParameters(m_resourceParameters, source.m_resourceParameters);
            CopyParameters(m_bindlessParameters, source.m_bindlessParameters);
        }
    }

    size_t GetStorageBytes() const
    {
        return m_valueData.capacity() +
            m_valueParameters.capacity() * sizeof(RHIShaderValueParameter) +
            (m_resourceParameters.capacity() + m_bindlessParameters.capacity()) *
            sizeof(RHIShaderResourceParameter);
    }

    VectorView<const RHIShaderValueParameter> GetValueParams() const
    {
        return MakeVecView(m_valueParameters.data(), m_valueParameters.size());
    }

    VectorView<const uint8_t> GetValueBytes(const RHIShaderValueParameter& param) const
    {
        VectorView<const uint8_t> result{};

        const uint32_t end = static_cast<uint32_t>(param.byteOffset) + param.byteSize;

        if (param.byteSize == 0 || end > m_valueData.size())
        {
            result = VectorView<const uint8_t>();
        }
        else
        {
            result = MakeVecView(m_valueData.data() + param.byteOffset,
                                 static_cast<size_t>(param.byteSize));
        }

        return result;
    }

    VectorView<const RHIShaderResourceParameter> GetResourceParams() const
    {
        return MakeVecView(m_resourceParameters.data(), m_resourceParameters.size());
    }

    VectorView<const RHIShaderResourceParameter> GetBindlessParams() const
    {
        return MakeVecView(m_bindlessParameters.data(), m_bindlessParameters.size());
    }

private:
    template <typename T>
    static void CopyParameters(HeapVector<T>& destination, const HeapVector<T>& source)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        destination.resize(source.size());

        if (!source.empty())
        {
            std::memcpy(destination.data(), source.data(), source.size() * sizeof(T));
        }
    }

    HeapVector<uint8_t> m_valueData;
    HeapVector<RHIShaderValueParameter> m_valueParameters;
    HeapVector<RHIShaderResourceParameter> m_resourceParameters;
    HeapVector<RHIShaderResourceParameter> m_bindlessParameters;
};
} // namespace zen
