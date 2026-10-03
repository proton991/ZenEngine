#include "Graphics/RHI/DynamicRHI.h"
#include "Graphics/RHI/RHIThread.h"

namespace zen
{
DynamicRHI::DynamicRHI()
{
    GetRHIThread().OpenAdmission();
}

DynamicRHI::~DynamicRHI()
{
    if (GDynamicRHI == this)
    {
        GDynamicRHI = nullptr;
    }
}
} // namespace zen
