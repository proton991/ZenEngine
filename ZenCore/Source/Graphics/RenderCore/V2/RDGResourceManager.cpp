#include "Utils/Helpers.h"
#include "Graphics/RenderCore/V2/RenderGraph/RDGResourceManager.h"
#include "Graphics/RHI/DynamicRHI.h"
#include "Graphics/RHI/RHIResource.h"
#include "Graphics/RenderCore/V2/RenderDevice.h"

namespace zen::rc
{
RDGResourceManager::RDGResourceManager()
{
    static std::atomic<uint64_t> nextIdentity{1};
    m_identity = nextIdentity.fetch_add(1, std::memory_order_relaxed);
}

RDGResource RDGResourceManager::MakeResource(const Allocation* resource) const
{
    return resource != nullptr ? RDGResource(m_identity, m_generation, uint32_t(resource->id)) :
                                 RDGResource{};
}

const RDGResourceManager::Allocation* RDGResourceManager::Resolve(RDGResource value)
{
    const Allocation* result{};

    if (value.m_owner != m_identity || value.m_generation != m_generation ||
        value.m_index >= m_resources.size() ||
        (value.IsVersioned() &&
         (size_t(value.m_version) >= m_versions.size() ||
          uint32_t(m_versions[value.m_version].resourceId) != value.m_index)))
    {
        if (m_owner != nullptr)
        {
            m_owner->Fail(RDGErrorCode::eLifecycle,
                          "Resource value belongs to a different graph or expired build");
        }
        else
        {
            LOGE("RDG resources [{}]: expired or foreign resource value",
                 uint32_t(RDGErrorCode::eLifecycle));
        }

        result = nullptr;
    }
    else
    {
        result = m_resources[value.m_index];
    }

    return result;
}

bool RDGResourceManager::IsValid(RDGResource resource)
{
    return Resolve(resource) != nullptr;
}

bool RDGResourceManager::IsValid(RDGTexture texture, const RDGTextureViewDesc& view)
{
    return ResolveView(texture, view) != nullptr;
}

bool RDGResourceManager::GetResourceInfo(RDGResource resource, RDGResourceInfo& info)
{
    bool returnValue{};

    const Allocation* allocation = Resolve(resource);

    if (allocation != nullptr)
    {
        RDGResourceInfo result;
        result.name      = allocation->name;
        result.type      = allocation->type;
        result.lifeCycle = allocation->GetLifeCycle();

        if (resource.IsVersioned())
        {
            result.version = int32_t(m_versions[resource.m_version].number);
        }

        if (allocation->pTexture != nullptr)
        {
            result.physicalStableId = allocation->pTexture->GetStableId();
        }
        else if (allocation->pBuffer != nullptr)
        {
            result.physicalStableId = allocation->pBuffer->GetStableId();
        }

        info        = result;
        returnValue = true;
    }

    return returnValue;
}

bool RDGResourceManager::GetTextureDesc(RDGTexture texture, RDGTextureDesc& desc)
{
    bool returnValue{};

    const Allocation* allocation = Resolve(texture);

    if (allocation != nullptr)
    {
        RDGTextureDesc result;
        result.name       = allocation->name;
        result.texFormat  = allocation->texFormat;
        result.usageFlags = int64_t(allocation->usageFlags);
        desc              = result;
        returnValue       = true;
    }

    return returnValue;
}

bool RDGResourceManager::GetBufferDesc(RDGBuffer buffer, RDGBufferDesc& desc)
{
    bool returnValue{};

    const Allocation* allocation = Resolve(buffer);

    if (allocation != nullptr)
    {
        RDGBufferDesc result;
        result.name       = allocation->name;
        result.size       = allocation->bufferSize;
        result.usageFlags = int64_t(allocation->usageFlags);
        desc              = result;
        returnValue       = true;
    }

    return returnValue;
}

RDGResource RDGResourceManager::InitialResourceVersion(RDGResource value)
{
    RDGResource version;
    const Allocation* resource = CheckMutation() ? Resolve(value) : nullptr;

    if (resource != nullptr)
    {
        if (resource->initialVersion < 0)
        {
            if (m_versions.size() >= size_t(INT32_MAX))
            {
                Reject("Too many resource versions", RDGErrorCode::eVersion);
            }
            else
            {
                Allocation* mutableResource     = m_resources[resource->id];
                mutableResource->initialVersion = int32_t(m_versions.size());
                m_versions.push_back({resource->id, -1, -1, 0});
            }
        }

        if (resource->initialVersion >= 0)
        {
            value.m_version = resource->initialVersion;
            version         = value;
        }
    }

    return version;
}

RDGResource RDGResourceManager::CreateResourceVersion(RDGResource previous)
{
    RDGResource result{};

    if (!CheckMutation() || Resolve(previous) == nullptr)
    {
        result = {};
    }
    else
    {
        if (!previous.IsVersioned())
        {
            previous = InitialResourceVersion(previous);
        }

        if (!previous)
        {
            result = {};
        }
        else if (m_versions[previous.m_version].next >= 0)
        {
            Reject(
                "Resource version already has a successor; pass the latest version to CreateVersion",
                RDGErrorCode::eVersion);
            result = {};
        }
        else if (m_versions.size() >= size_t(INT32_MAX))
        {
            Reject("Too many resource versions", RDGErrorCode::eVersion);
            result = {};
        }
        else
        {
            const int32_t index               = int32_t(m_versions.size());
            const ResourceVersion predecessor = m_versions[previous.m_version];
            m_versions.push_back(
                {predecessor.resourceId, previous.m_version, -1, predecessor.number + 1});
            m_versions[previous.m_version].next = index;
            previous.m_version                  = index;
            result                              = previous;
        }
    }

    return result;
}

RDGTexture RDGResourceManager::InitialVersion(RDGTexture texture)
{
    return RDGTexture(InitialResourceVersion(texture));
}

RDGBuffer RDGResourceManager::InitialVersion(RDGBuffer buffer)
{
    return RDGBuffer(InitialResourceVersion(buffer));
}

RDGTexture RDGResourceManager::CreateVersion(RDGTexture previous)
{
    return RDGTexture(CreateResourceVersion(previous));
}

RDGBuffer RDGResourceManager::CreateVersion(RDGBuffer previous)
{
    return RDGBuffer(CreateResourceVersion(previous));
}

RDGTexture RDGResourceManager::ImportTexture(RHITexture* texture,
                                             const RDGTextureImportState& state)
{
    RDGTexture result{};

    RDGTexture value = ImportTexture(texture, state.contents);

    if (!value)
    {
        result = {};
    }
    else
    {
        Allocation* resource = const_cast<Allocation*>(Resolve(value));

        if (state.usage >= RHITextureUsage::eMax || state.stages.IsEmpty() ||
            state.accessMode < RHIAccessMode::eNone ||
            state.accessMode > RHIAccessMode::eReadWrite ||
            (state.usage == RHITextureUsage::eNone &&
             (state.contents == RDGImportContents::eDefined ||
              state.accessMode != RHIAccessMode::eNone)))
        {
            Reject("Invalid external texture state");
            result = {};
        }
        else if (state.usage != RHITextureUsage::eNone &&
                 (int64_t(resource->usageFlags) & (1u << (uint32_t(state.usage) - 1))) == 0)
        {
            Reject("External texture state requires an unavailable creation usage");
            result = {};
        }
        else
        {
            const RDGTextureImportState& old = resource->initialTextureState;

            if (resource->hasInitialState &&
                (old.accessMode != state.accessMode || old.usage != state.usage ||
                 int64_t(old.stages) != int64_t(state.stages)))
            {
                Reject("Conflicting external texture states");
                result = {};
            }
            else
            {
                resource->hasInitialState     = true;
                resource->initialTextureState = state;
                result                        = value;
            }
        }
    }

    return result;
}

RDGBuffer RDGResourceManager::ImportBuffer(RHIBuffer* buffer, const RDGBufferImportState& state)
{
    RDGBuffer result{};

    RDGBuffer value = ImportBuffer(buffer, state.contents);

    if (!value)
    {
        result = {};
    }
    else
    {
        Allocation* resource = const_cast<Allocation*>(Resolve(value));

        if (state.stages.IsEmpty() || (int64_t(state.usage) & ~0x1ff) != 0 ||
            state.accessMode < RHIAccessMode::eNone ||
            state.accessMode > RHIAccessMode::eReadWrite ||
            (state.accessMode != RHIAccessMode::eNone && state.usage.IsEmpty()))
        {
            Reject("Invalid external buffer state");
            result = {};
        }
        else if ((int64_t(resource->usageFlags) & int64_t(state.usage)) != int64_t(state.usage))
        {
            Reject("External buffer state requires an unavailable creation usage");
            result = {};
        }
        else
        {
            const RDGBufferImportState& old = resource->initialBufferState;

            if (resource->hasInitialState &&
                (old.accessMode != state.accessMode || int64_t(old.usage) != int64_t(state.usage) ||
                 int64_t(old.stages) != int64_t(state.stages)))
            {
                Reject("Conflicting external buffer states");
                result = {};
            }
            else
            {
                resource->hasInitialState    = true;
                resource->initialBufferState = state;
                result                       = value;
            }
        }
    }

    return result;
}

bool RDGResourceManager::SetImportContents(const Allocation* resource, RDGImportContents contents)
{
    bool result{};

    if (resource != nullptr)
    {
        if (contents == RDGImportContents::ePreserve)
        {
            result = true;
        }
        else if (contents != RDGImportContents::eDefined &&
                 contents != RDGImportContents::eUndefined &&
                 contents != RDGImportContents::eUnknown)
        {
            Reject("Invalid import content contract");
            result = false;
        }
        else
        {
            Allocation* mutableResource = m_resources[resource->id];

            if (mutableResource->initialContents != RDGImportContents::ePreserve &&
                mutableResource->initialContents != contents)
            {
                Reject("Conflicting import content contracts for '" + resource->name.ToString() +
                       "'");
                result = false;
            }
            else
            {
                mutableResource->initialContents = contents;
                result                           = true;
            }
        }
    }

    return result;
}

RHITextureSubResourceRange RDGResourceManager::ViewRange(const Allocation& texture,
                                                         const RDGTextureViewDesc& view)
{
    RHITextureSubResourceRange result{};

    if (view.range)
    {
        result = *view.range;
    }
    else
    {
        RHITextureSubResourceRange range;
        const DataFormat format = texture.texFormat.format;
        range.aspect = FormatIsDepthStencil(format) ? int64_t(RHITextureAspectFlagBits::eDepth) |
                int64_t(RHITextureAspectFlagBits::eStencil) :
            FormatIsDepthOnly(format)   ? int64_t(RHITextureAspectFlagBits::eDepth) :
            FormatIsStencilOnly(format) ? int64_t(RHITextureAspectFlagBits::eStencil) :
                                          int64_t(RHITextureAspectFlagBits::eColor);
        range.levelCount = texture.texFormat.mipmaps;
        range.layerCount = texture.texFormat.arrayLayers;
        result           = range;
    }

    return result;
}

const RDGResourceManager::Allocation* RDGResourceManager::ResolveView(
    RDGResource texture,
    const RDGTextureViewDesc& view)
{
    const Allocation* result{};

    const Allocation* resource = Resolve(texture);

    if (resource != nullptr)
    {
        const RHITextureSubResourceRange full  = ViewRange(*resource, {});
        const RHITextureSubResourceRange range = ViewRange(*resource, view);

        // Preserve current RHI view restrictions; aspect/base-layer expansion belongs to Phase 4.
        if (resource->type != RDGResourceType::eTexture ||
            int64_t(range.aspect) != int64_t(full.aspect) || range.baseArrayLayer != 0 ||
            range.layerCount == 0 || range.layerCount > resource->texFormat.arrayLayers ||
            range.levelCount == 0 ||
            (resource->texFormat.dimension == TextureDimension::eCube &&
             range.layerCount % 6 != 0) ||
            (resource->texFormat.dimension == TextureDimension::e3D && range.layerCount != 1) ||
            range.baseMipLevel >= resource->texFormat.mipmaps ||
            range.levelCount > resource->texFormat.mipmaps - range.baseMipLevel)
        {
            Reject("Unsupported or out-of-bounds logical texture view", RDGErrorCode::eRange);
            result = nullptr;
        }
        else
        {
            result = resource;
        }
    }

    return result;
}

RHITextureView* RDGResourceManager::MaterializeView(RDGResource textureValue,
                                                    const RDGTextureViewDesc& viewDesc)
{
    RHITextureView* pView      = nullptr;
    const Allocation* resource = ResolveView(textureValue, viewDesc);

    if (resource != nullptr && resource->pTexture != nullptr)
    {
        RHITexture* texture = resource->pTexture;

        if (!viewDesc.range)
        {
            pView = texture->GetDefaultView();
        }
        else
        {
            const RHITextureSubResourceRange range = ViewRange(*resource, viewDesc);

            for (CachedView const& cached : m_viewCache)
            {
                if (cached.textureId == texture->GetStableId() &&
                    cached.range.baseMipLevel == range.baseMipLevel &&
                    cached.range.levelCount == range.levelCount &&
                    cached.range.layerCount == range.layerCount)
                {
                    pView = cached.view;
                    break;
                }
            }

            if (pView == nullptr)
            {
                RHITextureViewCreateInfo info{};
                info.format       = texture->GetFormat();
                info.type         = texture->GetBaseInfo().type;
                info.arrayLayers  = range.layerCount;
                info.mipLevels    = range.levelCount;
                info.baseMipLevel = range.baseMipLevel;
                pView             = texture->CreateView(info);

                if (pView != nullptr)
                {
                    m_viewCache.push_back({texture->GetStableId(), range, pView});
                }
            }

            if (pView != nullptr)
            {
                Retain(pView);
            }
        }
    }

    return pView;
}

RDGExtractionState::~RDGExtractionState()
{
    if (resource != nullptr)
    {
        if (device != nullptr)
        {
            device->DeferReleaseResource(resource);
        }
        else
        {
            resource->ReleaseReference();
        }
    }
}

RefCountPtr<RDGExtractionState> RDGResourceManager::QueueExtraction(
    RDGResource value,
    RDGResourceType type,
    RHITextureUsage textureUsage,
    BitField<RHIBufferUsageFlagBits> bufferUsage)
{
    RefCountPtr<RDGExtractionState> result{};

    if (!CheckMutation())
    {
        result = {};
    }
    else
    {
        const Allocation* resource = Resolve(value);

        if (resource == nullptr)
        {
            result = {};
        }
        else if (resource->type != type || resource->exported)
        {
            Reject("Extraction requires a matching resource with no existing extraction owner");
            result = {};
        }
        else if ((type == RDGResourceType::eTexture && textureUsage != RHITextureUsage::eSampled &&
                  textureUsage != RHITextureUsage::eTransferSrc &&
                  textureUsage != RHITextureUsage::eStorage) ||
                 (type == RDGResourceType::eBuffer &&
                  (bufferUsage.IsEmpty() ||
                   bufferUsage.HasFlag(RHIBufferUsageFlagBits::eTransferDstBuffer))))
        {
            Reject("Extraction requires a supported final read usage");
            result = {};
        }
        else
        {
            RefCountPtr<RDGExtractionState> state = MakeRefCountPtr<RDGExtractionState>();
            Allocation* mutableResource           = m_resources[resource->id];
            mutableResource->exported             = true;
            m_extractions.push_back({mutableResource, value, textureUsage, bufferUsage, state});
            result = state;
        }
    }

    return result;
}

RDGExtractedTexture RDGResourceManager::QueueTextureExtraction(RDGTexture texture,
                                                               RHITextureUsage usage)
{
    return RDGExtractedTexture(QueueExtraction(texture, RDGResourceType::eTexture, usage, {}));
}

RDGExtractedBuffer RDGResourceManager::QueueBufferExtraction(RDGBuffer buffer,
                                                             BitField<RHIBufferUsageFlagBits> usage)
{
    return RDGExtractedBuffer(
        QueueExtraction(buffer, RDGResourceType::eBuffer, RHITextureUsage::eNone, usage));
}

bool RDGResourceManager::DeclareExtractions()
{
    bool valid = m_extractions.empty();

    if (m_owner != nullptr)
    {
        valid = true;

        for (Extraction const& extraction : m_extractions)
        {
            if (extraction.value.IsVersioned() && m_versions[extraction.value.m_version].next >= 0)
            {
                valid = m_owner->Fail(
                    RDGErrorCode::eExport,
                    "Extraction must select the final resource version; earlier values share its allocation");
                break;
            }

            RDGTransferPassCmdRecorder recorder = m_owner->AddTransferPass("RDG.Extract");
            RDGPassNode* node            = static_cast<RDGPassNode*>(m_owner->m_nodes.back());
            node->requireDefinedContents = true;
            const BitField<RHIPipelineStageFlagBits> stages =
                BitField<RHIPipelineStageFlagBits>(RHIPipelineStageFlagBits::eAllCommands);

            if (extraction.resource->type == RDGResourceType::eTexture)
            {
                const TextureFormat& format = extraction.resource->texFormat;
                RHITextureSubResourceRange range{};
                range            = FormatIsDepthStencil(format.format) ?
                    RHITextureSubResourceRange::DepthStencil() :
                    FormatIsDepthOnly(format.format)   ? RHITextureSubResourceRange::Depth() :
                    FormatIsStencilOnly(format.format) ? RHITextureSubResourceRange::Stencil() :
                                                         RHITextureSubResourceRange::Color();
                range.levelCount = format.mipmaps;
                range.layerCount = format.arrayLayers;

                if (!m_owner->DeclareTextureAccessForPass(
                        node, extraction.resource, extraction.textureUsage, range,
                        RHIAccessMode::eRead, stages, RDGContentEffect::eRead, true, false,
                        extraction.value))
                {
                    valid = false;
                    break;
                }
            }
            else if (!m_owner->DeclareBufferAccessForPass(
                         node, extraction.resource, extraction.bufferUsage, RHIAccessMode::eRead,
                         stages, RDGContentEffect::eRead, true, false, extraction.value))
            {
                valid = false;
                break;
            }
        }
    }

    return valid;
}

void RDGResourceManager::PublishExtractions(RenderDevice* device)
{
    for (Extraction const& extraction : m_extractions)
    {
        if (extraction.state->resource != nullptr)
        {
            continue;
        }

        RHIResource* resource = extraction.resource->type == RDGResourceType::eTexture ?
            static_cast<RHIResource*>(extraction.resource->pTexture) :
            extraction.resource->pBuffer;
        resource->AddReference();
        extraction.state->resource = resource;
        extraction.state->device   = device;
    }
}

void RDGResourceManager::StageExtractions(HeapVector<RDGDeferredExtraction>& output)
{
    for (const Extraction& extraction : m_extractions)
    {
        if (extraction.state->resource == nullptr)
        {
            RHIResource* resource = extraction.resource->type == RDGResourceType::eTexture ?
                static_cast<RHIResource*>(extraction.resource->pTexture) :
                extraction.resource->pBuffer;
            RHIResourcePtr<RHIResource> owner(resource);
            output.push_back({extraction.state, std::move(owner)});
        }
    }
}

void RDGResourceManager::Retain(RHIResource* resource)
{
    if (resource == nullptr)
    {
        return;
    }

    if (resource->GetResourceType() == RHIResourceType::eTextureView)
    {
        Retain(static_cast<RHITextureView*>(resource)->GetTexture());
    }

    if (m_retainedResources.try_emplace(resource->GetStableId(), resource).second)
    {
        resource->AddReference();
    }
}

static void ReleaseResource(RenderDevice* device, RHIResource* resource)
{
    if (device != nullptr)
    {
        device->DeferReleaseResource(resource);
    }
    else
    {
        resource->ReleaseReference();
    }
}

void RDGResourceManager::ReleaseRetainedResources(RenderDevice* device)
{
    // Release graph-owned views before their backing textures, including during retirement.
    for (FlatHashMap<uint64_t, RHIResource*>::iterator it = m_retainedResources.begin();
         it != m_retainedResources.end();)
    {
        RHIResource* resource = it->second;

        if (resource->GetResourceType() == RHIResourceType::eTextureView)
        {
            it = m_retainedResources.erase(it);
            ReleaseResource(device, resource);
        }
        else
        {
            ++it;
        }
    }

    for (FlatHashMap<uint64_t, RHIResource*>::value_type& retained : m_retainedResources)
    {
        ReleaseResource(device, retained.second);
    }

    m_retainedResources.clear();
}

void RDGResourceManager::Reject(const std::string& message, RDGErrorCode code)
{
    if (m_owner != nullptr)
    {
        m_owner->Fail(code, message);
    }
    else
    {
        LOGE("RDG resources: {}", message);
    }
}

bool RDGResourceManager::CheckMutation()
{
    bool valid = true;

    if (m_owner != nullptr)
    {
        valid = static_cast<bool>(m_owner->GetResult());

        if ((valid) && (m_owner->GetExecutionState() != RDGExecutionState::eBuilding))
        {
            valid = m_owner->Fail(RDGErrorCode::eLifecycle, "Resource declarations require Begin");
        }
    }

    if ((valid) && (m_pFrameArena == nullptr))
    {
        Reject("RDGResourceManager requires SetFrameArena");
        valid = false;
    }

    return valid;
}

RDGTexture RDGResourceManager::CreateTexture(const RDGTextureDesc& desc)
{
    return RDGTexture(MakeResource(CreateTextureAllocation(desc)));
}

const RDGResourceManager::Allocation* RDGResourceManager::CreateTextureAllocation(
    const RDGTextureDesc& desc)
{
    const Allocation* result{};

    if (CheckMutation())
    {
        Allocation* pResource = nullptr;

        if (ValidateRDGTextureDesc(desc))
        {
            pResource             = AllocAllocation();
            pResource->type       = RDGResourceType::eTexture;
            pResource->name       = desc.name;
            pResource->texFormat  = desc.texFormat;
            pResource->usageFlags = desc.usageFlags;
        }

        if (pResource == nullptr)
        {
            Reject("Invalid texture descriptor '" + desc.name.ToString() + "'");
        }

        result = pResource;
    }

    return result;
}

RDGBuffer RDGResourceManager::CreateBuffer(const RDGBufferDesc& desc)
{
    return RDGBuffer(MakeResource(CreateBufferAllocation(desc)));
}

const RDGResourceManager::Allocation* RDGResourceManager::CreateBufferAllocation(
    const RDGBufferDesc& desc)
{
    const Allocation* result{};

    if (CheckMutation())
    {
        Allocation* pResource = nullptr;

        if (ValidateRDGBufferDesc(desc))
        {
            pResource             = AllocAllocation();
            pResource->type       = RDGResourceType::eBuffer;
            pResource->name       = desc.name;
            pResource->bufferSize = desc.size;
            pResource->usageFlags = desc.usageFlags;
        }

        if (pResource == nullptr)
        {
            Reject("Invalid buffer descriptor '" + desc.name.ToString() + "'");
        }

        result = pResource;
    }

    return result;
}

RDGTexture RDGResourceManager::ImportTexture(RHITexture* texture, RDGImportContents contents)
{
    const Allocation* resource = ImportTextureAllocation(texture);
    return SetImportContents(resource, contents) ? RDGTexture(MakeResource(resource)) :
                                                   RDGTexture{};
}

const RDGResourceManager::Allocation* RDGResourceManager::ImportTextureAllocation(
    RHITexture* pTexture)
{
    const Allocation* result{};

    if (CheckMutation())
    {
        if (pTexture == nullptr)
        {
            Reject("Null imported texture");
            result = nullptr;
        }
        else
        {
            const Allocation* pResult = nullptr;

            if (pTexture != nullptr)
            {
                pResult = FindResourceByStableId(pTexture->GetStableId());

                if (pResult == nullptr)
                {
                    Allocation* pResource = AllocAllocation();

                    pResource->type     = RDGResourceType::eTexture;
                    pResource->imported = true;
                    pResource->name     = pTexture->GetResourceTag();
                    pResource->pTexture = pTexture;
                    Retain(pTexture);

                    pResource->texFormat.format      = pTexture->GetFormat();
                    pResource->texFormat.sampleCount = pTexture->GetBaseInfo().samples;
                    pResource->texFormat.width       = pTexture->GetWidth();
                    pResource->texFormat.height      = pTexture->GetHeight();
                    pResource->texFormat.depth       = pTexture->GetDepth();
                    pResource->texFormat.arrayLayers = pTexture->GetArrayLayers();
                    pResource->texFormat.mipmaps     = pTexture->GetNumMipmaps();
                    pResource->texFormat.dimension =
                        static_cast<TextureDimension>(pTexture->GetBaseInfo().type);
                    pResource->texFormat.mutableFormat = pTexture->GetBaseInfo().mutableFormat;
                    pResource->usageFlags              = pTexture->GetBaseInfo().usageFlags;

                    m_resourceTable[pTexture->GetStableId()] = pResource->id;
                    pResult                                  = pResource;
                }
            }

            result = pResult;
        }
    }

    return result;
}

RDGBuffer RDGResourceManager::ImportBuffer(RHIBuffer* buffer, RDGImportContents contents)
{
    const Allocation* resource = ImportBufferAllocation(buffer);
    return SetImportContents(resource, contents) ? RDGBuffer(MakeResource(resource)) : RDGBuffer{};
}

const RDGResourceManager::Allocation* RDGResourceManager::ImportBufferAllocation(RHIBuffer* pBuffer)
{
    const Allocation* result{};

    if (CheckMutation())
    {
        if (pBuffer == nullptr)
        {
            Reject("Null imported buffer");
            result = nullptr;
        }
        else
        {
            const Allocation* pResult = nullptr;

            if (pBuffer != nullptr)
            {
                pResult = FindResourceByStableId(pBuffer->GetStableId());

                if (pResult == nullptr)
                {
                    Allocation* pResource = AllocAllocation();

                    pResource->type       = RDGResourceType::eBuffer;
                    pResource->imported   = true;
                    pResource->name       = pBuffer->GetResourceTag();
                    pResource->bufferSize = pBuffer->GetRequiredSize();
                    pResource->usageFlags = pBuffer->GetUsageFlags();
                    pResource->pBuffer    = pBuffer;
                    Retain(pBuffer);

                    m_resourceTable[pBuffer->GetStableId()] = pResource->id;
                    pResult                                 = pResource;
                }
            }

            result = pResult;
        }
    }

    return result;
}

RDGBuffer RDGResourceManager::ImportHostWrittenBuffer(RHIBuffer* pBuffer)
{
    const Allocation* resource = ImportBufferAllocation(pBuffer);

    if (resource != nullptr)
    {
        m_resources[resource->id]->hostWritten     = true;
        m_resources[resource->id]->initialContents = RDGImportContents::eDefined;
    }

    return RDGBuffer(MakeResource(resource));
}

const RDGResourceManager::Allocation* RDGResourceManager::FindResourceByIdx(int32_t idx) const
{
    const Allocation* pResource = nullptr;

    if (idx >= 0 && idx < m_resources.size())
    {
        pResource = m_resources[idx];
    }

    return pResource;
}

const RDGResourceManager::Allocation* RDGResourceManager::FindResourceByStableId(
    uint64_t stableId) const
{
    const Allocation* pResource = nullptr;

    FlatHashMap<uint64_t, int>::const_iterator iter = m_resourceTable.find(stableId);

    if (iter != m_resourceTable.end())
    {
        pResource = FindResourceByIdx(iter->second);
    }

    return pResource;
}

namespace
{
uint64_t PoolAdd(uint64_t a, uint64_t b)
{
    return b > UINT64_MAX - a ? UINT64_MAX : a + b;
}

uint64_t PoolMultiply(uint64_t a, uint64_t b)
{
    return a && b > UINT64_MAX / a ? UINT64_MAX : a * b;
}
} // namespace

uint64_t RDGResourceManager::EstimateBytes(const Allocation& resource)
{
    uint64_t estimatedBytes = resource.bufferSize;

    if (resource.type != RDGResourceType::eBuffer)
    {
        const TextureFormat& format = resource.texFormat;
        uint64_t pixel              = GetTextureFormatPixelSize(format.format);

        switch (format.format)
        {
            case DataFormat::eR8UNORM:
            case DataFormat::eR8UInt:
            case DataFormat::eS8UInt: pixel = 1; break;
            case DataFormat::eR8G8B8SRGB:
            case DataFormat::eR8G8B8UNORM: pixel = 3; break;
            case DataFormat::eR8G8B8A8UInt:
            case DataFormat::eR8G8B8A8SRGB:
            case DataFormat::eR8G8B8A8UNORM:
            case DataFormat::eD32SFloat:
            case DataFormat::eD24UNORMS8UInt: pixel = 4; break;
            case DataFormat::eD16UNORM: pixel = 2; break;
            case DataFormat::eD16UNORMS8UInt: pixel = 4; break;
            case DataFormat::eD32SFloatS8UInt: pixel = 8; break;
            default: break;
        }

        if (pixel == 0x7fffffff || uint32_t(format.sampleCount) >= uint32_t(SampleCount::eMax))
        {
            estimatedBytes = UINT64_MAX; // Unknown formats cannot silently bypass the cache budget.
        }
        else
        {
            uint64_t bytes = 0;
            uint64_t width = format.width, height = format.height, depth = format.depth;

            for (uint32_t mip = 0; mip < format.mipmaps; ++mip)
            {
                bytes  = PoolAdd(bytes, PoolMultiply(PoolMultiply(width, height), depth));
                width  = std::max(uint64_t(1), width / 2);
                height = std::max(uint64_t(1), height / 2);
                depth  = std::max(uint64_t(1), depth / 2);
            }

            estimatedBytes =
                PoolMultiply(PoolMultiply(PoolMultiply(bytes, pixel), format.arrayLayers),
                             uint64_t(1) << uint32_t(format.sampleCount));
        }
    }

    return estimatedBytes;
}

bool RDGResourceManager::InFlight(uint64_t graphics, uint64_t transfer)
{
    return GDynamicRHI &&
        (graphics > GDynamicRHI->GetLastCompletedSerial(RHICommandContextType::eGraphics) ||
         transfer > GDynamicRHI->GetLastCompletedSerial(RHICommandContextType::eTransfer));
}

template <typename Pool>
void RDGResourceManager::CountPoolStats(const Pool& pool, RDGPoolStats& stats)
{
    for (const typename Pool::value_type& bucket : pool)
    {
        stats.descriptorCount += !bucket.second.empty();

        for (const PoolEntry& entry : bucket.second)
        {
            stats.availableBytes = PoolAdd(stats.availableBytes, entry.bytes);
            ++stats.availableCount;

            if (InFlight(entry.graphicsSerial, entry.transferSerial))
            {
                stats.inFlightBytes = PoolAdd(stats.inFlightBytes, entry.bytes);
            }
        }
    }
}

RDGPoolStats RDGResourceManager::GetPoolStats() const
{
    RDGPoolStats stats;
    stats.hits           = m_poolHits;
    stats.misses         = m_poolMisses;
    stats.evictions      = m_poolEvictions;
    const bool submitted = GDynamicRHI &&
        InFlight(GDynamicRHI->GetLastSubmittedSerial(RHICommandContextType::eGraphics),
                 GDynamicRHI->GetLastSubmittedSerial(RHICommandContextType::eTransfer));

    for (const Allocation* resource : m_resources)
    {
        if (resource->ownsPhysical && !resource->imported)
        {
            const uint64_t bytes = EstimateBytes(*resource);
            stats.assignedBytes  = PoolAdd(stats.assignedBytes, bytes);
            ++stats.assignedCount;

            if (submitted)
            {
                stats.inFlightBytes = PoolAdd(stats.inFlightBytes, bytes);
            }
        }
    }

    CountPoolStats(m_texturePool, stats);
    CountPoolStats(m_bufferPool, stats);
    stats.allocatedBytes = PoolAdd(stats.availableBytes, stats.assignedBytes);

    for (RetiredPoolBytes const& entry : m_retiredPoolBytes)
    {
        if (InFlight(entry.graphicsSerial, entry.transferSerial))
        {
            stats.retiringBytes = PoolAdd(stats.retiringBytes, entry.bytes);
        }
    }

    stats.inFlightBytes = PoolAdd(stats.inFlightBytes, stats.retiringBytes);

    return stats;
}

HeapVector<RDGPoolBucketStats> RDGResourceManager::GetPoolBuckets() const
{
    HeapVector<RDGPoolBucketStats> stats;

    for (const TexturePool::value_type& textureBucket : m_texturePool)
    {
        if (textureBucket.second.empty())
        {
            continue;
        }

        RDGPoolBucketStats& bucket = stats.emplace_back();
        bucket.type                = RDGResourceType::eTexture;
        bucket.textureFormat       = textureBucket.first.texFormat;
        bucket.usageFlags          = textureBucket.first.usageFlags;
        bucket.availableCount      = uint32_t(textureBucket.second.size());

        for (PoolEntry const& entry : textureBucket.second)
        {
            bucket.availableBytes = PoolAdd(bucket.availableBytes, entry.bytes);
        }
    }

    for (const BufferPool::value_type& bufferBucket : m_bufferPool)
    {
        if (bufferBucket.second.empty())
        {
            continue;
        }

        RDGPoolBucketStats& bucket = stats.emplace_back();
        bucket.type                = RDGResourceType::eBuffer;
        bucket.bufferSize          = bufferBucket.first.size;
        bucket.usageFlags          = bufferBucket.first.usageFlags;
        bucket.availableCount      = uint32_t(bufferBucket.second.size());

        for (PoolEntry const& entry : bufferBucket.second)
        {
            bucket.availableBytes = PoolAdd(bucket.availableBytes, entry.bytes);
        }
    }

    return stats;
}

bool RDGResourceManager::SetPoolConfig(const RDGPoolConfig& config)
{
    bool result{};

    if (m_owner && m_owner->m_inExecution)
    {
        result = m_owner->Fail(RDGErrorCode::eLifecycle,
                               "Cannot configure a pool while recording commands");
    }
    else
    {
        m_poolConfig = config;
        result       = TrimPool();
    }

    return result;
}

void RDGResourceManager::RetirePoolEntry(const PoolEntry& entry)
{
    ++m_poolEvictions;
    const uint64_t graphics =
        GDynamicRHI ? GDynamicRHI->GetLastSubmittedSerial(RHICommandContextType::eGraphics) : 0;
    const uint64_t transfer =
        GDynamicRHI ? GDynamicRHI->GetLastSubmittedSerial(RHICommandContextType::eTransfer) : 0;

    if (InFlight(graphics, transfer))
    {
        m_retiredPoolBytes.push_back({entry.bytes, graphics, transfer});
    }

    // Keep physical hazard/layout history until the final native reference is retired.
    if (m_owner && m_owner->m_pRenderDevice)
    {
        m_owner->m_pRenderDevice->DeferReleaseResource(entry.resource);
    }
    else
    {
        entry.resource->ReleaseReference();
    }
}

template <typename Pool>
void RDGResourceManager::CollectPoolEntries(Pool& pool, HeapVector<PoolEntry*>& candidates)
{
    for (typename Pool::value_type& bucket : pool)
    {
        for (PoolEntry& entry : bucket.second)
        {
            candidates.push_back(&entry);
        }
    }
}

template <typename Pool> void RDGResourceManager::CompactPool(Pool& pool)
{
    for (typename Pool::iterator it = pool.begin(); it != pool.end();)
    {
        HeapVector<PoolEntry>& entries = it->second;
        entries.erase(
            std::remove_if(entries.begin(), entries.end(),
                           [](const PoolEntry& entry) { return entry.resource == nullptr; }),
            entries.end());

        if (entries.empty())
        {
            it = pool.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

bool RDGResourceManager::TrimPool(bool allAvailable)
{
    bool result{};

    if (m_owner && m_owner->m_inExecution)
    {
        result =
            m_owner->Fail(RDGErrorCode::eLifecycle, "Cannot trim a pool while recording commands");
    }
    else
    {
        m_retiredPoolBytes.erase(
            std::remove_if(m_retiredPoolBytes.begin(), m_retiredPoolBytes.end(),
                           [](const RetiredPoolBytes& entry) {
                               return !InFlight(entry.graphicsSerial, entry.transferSerial);
                           }),
            m_retiredPoolBytes.end());
        HeapVector<PoolEntry*> candidates;

        CollectPoolEntries(m_texturePool, candidates);
        CollectPoolEntries(m_bufferPool, candidates);
        // Keep the newest available allocations first. This also handles saturated byte estimates
        // without subtracting an overflowed total. No active or exported allocation is evicted.
        std::stable_sort(candidates.begin(), candidates.end(),
                         [](const PoolEntry* lhs, const PoolEntry* rhs) {
                             return lhs->lastUsedBuild > rhs->lastUsedBuild;
                         });
        uint64_t retained = 0;

        for (PoolEntry* candidate : candidates)
        {
            PoolEntry& entry   = *candidate;
            const bool expired = m_generation - entry.lastUsedBuild > m_poolConfig.maxIdleBuilds;

            if (allAvailable || expired || entry.bytes > m_poolConfig.budgetBytes - retained)
            {
                RetirePoolEntry(entry);
                entry.resource = nullptr;
            }
            else
            {
                retained += entry.bytes;
            }
        }

        CompactPool(m_texturePool);
        CompactPool(m_bufferPool);
        result = true;
    }

    return result;
}

RDGResult RDGResourceManager::MaterializeTransientResources(RenderDevice* pDevice)
{
    RDGResult result;
    HeapVector<Allocation*> ordered;

    for (Allocation* resource : m_resources)
    {
        if (!resource->imported && resource->liveAccessCount != 0)
        {
            ordered.push_back(resource);
        }
    }

    std::stable_sort(
        ordered.begin(), ordered.end(),
        [](const Allocation* lhs, const Allocation* rhs) { return lhs->firstUse < rhs->firstUse; });

    // Each slot is one native object, not backend memory aliasing. All versions and views of
    // one logical allocation retain its whole first/last-use interval, including replay.
    struct Slot
    {
        Allocation* allocation;
        uint32_t lastUse;

        static bool Later(const Slot& a, const Slot& b)
        {
            return a.lastUse > b.lastUse;
        }
    };

    FlatHashMap<RDGTexturePoolKey, HeapVector<Slot>, RDGTexturePoolKeyHasher> textures;
    FlatHashMap<RDGBufferPoolKey, HeapVector<Slot>, RDGBufferPoolKeyHasher> buffers;

    for (Allocation* resource : ordered)
    {
        if (resource->pTexture || resource->pBuffer)
        {
            continue;
        }

        if (!result.Check(GDynamicRHI != nullptr, RDGErrorCode::eAllocation,
                          "Missing RHI during materialization"))
        {
            break;
        }

        HeapVector<Slot>& slots = resource->type == RDGResourceType::eTexture ?
            textures[MakeTexturePoolKey(*resource)] :
            buffers[MakeBufferPoolKey(*resource)];

        bool reused = false;

        if (!resource->exported && m_owner->m_reuseAllocations && !slots.empty() &&
            slots[0].lastUse < resource->firstUse)
        {
            std::pop_heap(slots.begin(), slots.end(), Slot::Later);
            Slot& slot         = slots.back();
            resource->pTexture = slot.allocation->pTexture;
            resource->pBuffer  = slot.allocation->pBuffer;
            slot.lastUse       = resource->lastUse;
            std::push_heap(slots.begin(), slots.end(), Slot::Later);
            reused = true;
            ++m_owner->m_compileStats.reusedAllocationCount;
        }

        if (!reused)
        {
            CreatePhysicalResource(resource);

            if (!result.Check(resource->pBuffer != nullptr || resource->pTexture != nullptr,
                              RDGErrorCode::eAllocation,
                              "Failed to allocate resource '" + resource->name.ToString() + "'"))
            {
                break;
            }

            resource->ownsPhysical = true;

            if (!resource->exported)
            {
                slots.push_back({resource, resource->lastUse});
                std::push_heap(slots.begin(), slots.end(), Slot::Later);
            }
        }
    }

    return result;
}

void RDGResourceManager::ReleaseTransientResources()
{
    for (Allocation* resource : m_resources)
    {
        if (resource->imported)
        {
            continue;
        }

        RHIResource* physical =
            resource->pTexture ? static_cast<RHIResource*>(resource->pTexture) : resource->pBuffer;

        if (physical && resource->ownsPhysical)
        {
            m_resourceTable.erase(physical->GetStableId());

            if (resource->exported)
            {
                if (m_owner && m_owner->m_pRenderDevice)
                {
                    m_owner->m_pRenderDevice->DeferReleaseResource(physical);
                }
                else
                {
                    physical->ReleaseReference();
                }
            }
            else
            {
                PoolEntry entry{
                    physical, EstimateBytes(*resource), m_generation,
                    GDynamicRHI ?
                        GDynamicRHI->GetLastSubmittedSerial(RHICommandContextType::eGraphics) :
                        0,
                    GDynamicRHI ?
                        GDynamicRHI->GetLastSubmittedSerial(RHICommandContextType::eTransfer) :
                        0};

                if (resource->type == RDGResourceType::eTexture)
                {
                    m_texturePool[MakeTexturePoolKey(*resource)].push_back(entry);
                }
                else
                {
                    m_bufferPool[MakeBufferPoolKey(*resource)].push_back(entry);
                }
            }
        }

        resource->pTexture     = nullptr;
        resource->pBuffer      = nullptr;
        resource->ownsPhysical = false;
    }

    TrimPool();
}

void RDGResourceManager::Destroy(RenderDevice* pDevice)
{
    ReleaseTransientResources();
    TrimPool(true);
    m_viewCache.clear();
    Reset();
}

RDGResourceManager::Allocation* RDGResourceManager::AllocAllocation()
{
    VERIFY_EXPR_MSG(m_pFrameArena != nullptr, "RDGResourceManager: SetFrameArena was not called!");

    void* pMem            = m_pFrameArena->Alloc(sizeof(Allocation), alignof(Allocation));
    Allocation* pResource = new (pMem) Allocation();
    pResource->id         = static_cast<int32_t>(m_resources.size());
    m_resources.push_back(pResource);

    return pResource;
}

void RDGResourceManager::CreatePhysicalResource(Allocation* pResource)
{
    if (pResource != nullptr && !pResource->imported)
    {
        if (pResource->type == RDGResourceType::eTexture)
        {
            if (!TryAcquirePooledTexture(pResource))
            {
                ++m_poolMisses;

                RHITextureCreateInfo createInfo{};
                createInfo.format      = pResource->texFormat.format;
                createInfo.samples     = pResource->texFormat.sampleCount;
                createInfo.usageFlags  = pResource->usageFlags;
                createInfo.width       = pResource->texFormat.width;
                createInfo.height      = pResource->texFormat.height;
                createInfo.depth       = pResource->texFormat.depth;
                createInfo.arrayLayers = pResource->texFormat.arrayLayers;
                createInfo.mipmaps     = pResource->texFormat.mipmaps;
                createInfo.tag         = pResource->name;

                createInfo.type = static_cast<RHITextureType>(pResource->texFormat.dimension);
                createInfo.mutableFormat = pResource->texFormat.mutableFormat;

                pResource->pTexture = GDynamicRHI->CreateTexture(createInfo);
            }

            if (pResource->pTexture != nullptr)
            {
                m_resourceTable[pResource->pTexture->GetStableId()] = pResource->id;
            }
        }
        else if (pResource->type == RDGResourceType::eBuffer)
        {
            if (!TryAcquirePooledBuffer(pResource))
            {
                ++m_poolMisses;

                if (pResource->bufferSize > std::numeric_limits<uint32_t>::max())
                {
                    LOGE("RDG buffer '{}' exceeds the maximum supported size: {}",
                         pResource->name.CStr(), pResource->bufferSize);
                    return;
                }

                RHIBufferCreateInfo createInfo{};
                createInfo.size         = static_cast<uint32_t>(pResource->bufferSize);
                createInfo.usageFlags   = pResource->usageFlags;
                createInfo.allocateType = RHIBufferAllocateType::eGPU;
                createInfo.tag          = pResource->name;

                pResource->pBuffer = GDynamicRHI->CreateBuffer(createInfo);
            }

            if (pResource->pBuffer != nullptr)
            {
                m_resourceTable[pResource->pBuffer->GetStableId()] = pResource->id;
            }
        }
        else
        {
            LOGE("RDG resource '{}' has no valid resource type", pResource->name.CStr());
        }
    }
}

bool RDGResourceManager::TryAcquirePooledTexture(Allocation* pResource)
{
    bool acquired = false;

    if (pResource->type == RDGResourceType::eTexture)
    {
        FlatHashMap<RDGTexturePoolKey, HeapVector<PoolEntry>, RDGTexturePoolKeyHasher>::iterator
            iter = m_texturePool.find(MakeTexturePoolKey(*pResource));

        if (iter != m_texturePool.end() && !iter->second.empty())
        {
            RHITexture* pTexture = static_cast<RHITexture*>(iter->second.back().resource);
            iter->second.pop_back();

            // RDG tracks logical versions; pool reuse preserves the native allocation
            // and its stable ID for commands still translating on RHI.
            pResource->pTexture = pTexture;

            ++m_poolHits;
            acquired = true;
        }
    }

    return acquired;
}

bool RDGResourceManager::TryAcquirePooledBuffer(Allocation* pResource)
{
    bool acquired = false;

    if (pResource->type == RDGResourceType::eBuffer)
    {
        FlatHashMap<RDGBufferPoolKey, HeapVector<PoolEntry>, RDGBufferPoolKeyHasher>::iterator
            iter = m_bufferPool.find(MakeBufferPoolKey(*pResource));

        if (iter != m_bufferPool.end() && !iter->second.empty())
        {
            RHIBuffer* pBuffer = static_cast<RHIBuffer*>(iter->second.back().resource);
            iter->second.pop_back();

            // Pool reuse preserves native descriptor identity, including while an older
            // frame still references this allocation on the RHI thread.
            pResource->pBuffer = pBuffer;

            ++m_poolHits;
            acquired = true;
        }
    }

    return acquired;
}

bool RDGResourceManager::ValidateRDGTextureDesc(const RDGTextureDesc& desc)
{
    bool valid = true;

    if (desc.texFormat.format == DataFormat::eUndefined)
    {
        LOGE("RDG texture '{}' has undefined format", desc.name.CStr());

        valid = false;
    }

    if (desc.texFormat.width == 0 || desc.texFormat.height == 0 || desc.texFormat.depth == 0 ||
        desc.texFormat.arrayLayers == 0 || desc.texFormat.mipmaps == 0)
    {
        LOGE("RDG texture '{}' has invalid extent/layer/mips: {}x{}x{} layers={} mips={}",
             desc.name.CStr(), desc.texFormat.width, desc.texFormat.height, desc.texFormat.depth,
             desc.texFormat.arrayLayers, desc.texFormat.mipmaps);

        valid = false;
    }

    if (desc.usageFlags.IsEmpty())
    {
        LOGE("RDG texture '{}' has no usage flags", desc.name.CStr());

        valid = false;
    }

    return valid;
}

bool RDGResourceManager::ValidateRDGBufferDesc(const RDGBufferDesc& desc)
{
    bool valid = true;

    if (desc.size == 0)
    {
        LOGE("RDG buffer '{}' has zero size", desc.name.CStr());

        valid = false;
    }

    if (desc.size > std::numeric_limits<uint32_t>::max())
    {
        LOGE("RDG buffer '{}' exceeds the maximum supported size: {}", desc.name.CStr(), desc.size);

        valid = false;
    }

    if (desc.usageFlags.IsEmpty())
    {
        LOGE("RDG buffer '{}' has no usage flags", desc.name.CStr());

        valid = false;
    }

    return valid;
}

RDGResourceManager::RDGTexturePoolKey RDGResourceManager::MakeTexturePoolKey(
    const Allocation& resource)
{
    RDGTexturePoolKey key{};
    key.texFormat  = resource.texFormat;
    key.usageFlags = static_cast<int64_t>(resource.usageFlags);

    return key;
}

RDGResourceManager::RDGBufferPoolKey RDGResourceManager::MakeBufferPoolKey(
    const Allocation& resource)
{
    RDGBufferPoolKey key{};
    key.size       = resource.bufferSize;
    key.usageFlags = static_cast<int64_t>(resource.usageFlags);

    return key;
}

size_t RDGResourceManager::RDGTexturePoolKeyHasher::operator()(const RDGTexturePoolKey& key) const
{
    size_t hash = 0;

    util::HashCombine(hash, key.texFormat.format);
    util::HashCombine(hash, key.texFormat.sampleCount);
    util::HashCombine(hash, key.texFormat.dimension);
    util::HashCombine(hash, key.texFormat.mutableFormat);
    util::HashCombine(hash, key.texFormat.width);
    util::HashCombine(hash, key.texFormat.height);
    util::HashCombine(hash, key.texFormat.depth);
    util::HashCombine(hash, key.texFormat.arrayLayers);
    util::HashCombine(hash, key.texFormat.mipmaps);
    util::HashCombine(hash, key.usageFlags);

    return hash;
}

size_t RDGResourceManager::RDGBufferPoolKeyHasher::operator()(const RDGBufferPoolKey& key) const
{
    size_t hash = 0;

    util::HashCombine(hash, key.size);
    util::HashCombine(hash, key.usageFlags);

    return hash;
}

void RDGResourceManager::Reset()
{
    ++m_generation;
    m_extractions.clear();
    m_versions.clear();
    ReleaseRetainedResources(m_owner != nullptr ? m_owner->m_pRenderDevice : nullptr);

    for (Allocation* pResource : m_resources)
    {
        if (pResource != nullptr)
        {
            pResource->~Allocation();
        }
    }

    m_resources.clear();
    m_resourceTable.clear();
}
} // namespace zen::rc
