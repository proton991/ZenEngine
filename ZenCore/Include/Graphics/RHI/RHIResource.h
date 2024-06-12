#pragma once
#include <atomic>
#include "Common/RefCountPtr.h"
#include "RHICommon.h"
#include "Common/Errors.h"

namespace zen::rhi
{
enum class ResourceType
{
    eNone,
    eSampler,
    eMax
};

class RHIResource
{
public:
    RHIResource() = default;

    explicit RHIResource(ResourceType resourceType) : m_resourceType(resourceType) {}

    uint32_t AddRef()
    {
        uint32_t newValue = m_counter.AddRef();
        return newValue;
    }

    uint32_t Release()
    {
        uint32_t newValue = m_counter.Release();
        if (newValue == 0) { delete this; }
        return newValue;
    }

    uint32_t GetRefCount() const { return m_counter.GetValue(); }

private:
    class AtomicCounter
    {
    public:
        uint32_t AddRef()
        {
            uint32_t oldValue = m_count.fetch_add(1, std::memory_order_acquire);
            return oldValue + 1;
        }

        uint32_t Release()
        {
            uint32_t oldValue = m_count.fetch_sub(1, std::memory_order_release);
            return oldValue - 1;
        }

        uint32_t GetValue() { return m_count.load(std::memory_order_relaxed); }

    private:
        std::atomic_uint m_count{0};
    };
    mutable AtomicCounter m_counter;
    ResourceType          m_resourceType{ResourceType::eMax};
};

class ShaderGroupSource : public RHIResource
{
public:
    ShaderGroupSource() = default;

    void SetStageSource(ShaderStage stage, const std::string& source)
    {
        VERIFY_EXPR(stage < ShaderStage::eMax);
        m_source[ToUnderlying(stage)] = source;
    }

    const std::string& GetStageSource(ShaderStage stage) const
    {
        VERIFY_EXPR(stage < ShaderStage::eMax);
        return m_source[ToUnderlying(stage)];
    }

private:
    ShaderLanguage m_shaderLanguage{ShaderLanguage::eGLSL};
    std::string    m_source[ToUnderlying(ShaderStage::eMax)];
};

class ShaderGroupSPIRV : public RHIResource
{
public:
    ShaderGroupSPIRV() = default;

    void SetStageSPIRV(ShaderStage stage, const std::vector<uint8_t>& source)
    {
        VERIFY_EXPR(stage < ShaderStage::eMax);
        m_spirv[static_cast<uint32_t>(stage)] = source;
    }

    const std::vector<uint8_t>& GetStageSPIRV(ShaderStage stage) const
    {
        VERIFY_EXPR(stage < ShaderStage::eMax);
        return m_spirv[ToUnderlying(stage)];
    }

    void SetStageCompileError(ShaderStage stage, const std::string& error)
    {
        VERIFY_EXPR(stage < ShaderStage::eMax);
        m_compileErrors[ToUnderlying(stage)] = error;
    }

    const std::string& GetStageCompileError(ShaderStage stage) const
    {
        VERIFY_EXPR(stage < ShaderStage::eMax);
        return m_compileErrors[ToUnderlying(stage)];
    }

private:
    ShaderLanguage       m_shaderLanguage{ShaderLanguage::eGLSL};
    std::vector<uint8_t> m_spirv[ToUnderlying(ShaderStage::eMax)];
    std::string          m_compileErrors[ToUnderlying(ShaderStage::eMax)];
};

using ShaderGroupSourcePtr = RefCountPtr<ShaderGroupSource>;
using ShaderGroupSPIRVPtr  = RefCountPtr<ShaderGroupSPIRV>;
} // namespace zen::rhi