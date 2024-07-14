#pragma once

namespace zen::rhi
{
class RHICommandList
{
public:
    virtual void Begin() = 0;

    virtual void End() = 0;
};
} // namespace zen::rhi