#include "Graphics/RHI/RHIResource.h"
#include "Graphics/RHI/DynamicRHI.h"

namespace zen
{
RHIResource::~RHIResource()
{
    VERIFY_EXPR(m_counter.GetValue() == 0);
    // The last owner may be an RHI batch or a bindless registration. Publish only
    // identity: RenderCore must never dereference a resource after final release.
    if (GDynamicRHI != nullptr &&
        (m_resourceType == RHIResourceType::eBuffer || m_resourceType == RHIResourceType::eTexture))
    {
        GDynamicRHI->NotifyResourceDestroyed(m_stableId);
    }
}
} // namespace zen
