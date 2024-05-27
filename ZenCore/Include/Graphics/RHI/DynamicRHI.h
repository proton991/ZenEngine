#pragma once
#include "RHIResource.h"

namespace zen
{
enum class GraphicsAPIType
{
    eVulkan,
    Count
};

class DynamicRHI
{
public:
    virtual ~DynamicRHI() = default;

    virtual void Init() = 0;

    virtual void Destroy() = 0;

    virtual GraphicsAPIType GetAPIType() = 0;

    virtual const char* GetName() = 0;

    virtual RHISamplerPtr CreateSampler(const RHISamplerSpec& samplerSpec) = 0;
};
} // namespace zen