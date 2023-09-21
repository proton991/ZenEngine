#pragma once
#include "Graphics/Val/ZenVal.h"

namespace zen
{
class RenderDevice
{
public:
    RenderDevice(val::Device& valDevice) :
        m_valDevice(valDevice) {}

private:
    val::Device& m_valDevice;
};
} // namespace zen