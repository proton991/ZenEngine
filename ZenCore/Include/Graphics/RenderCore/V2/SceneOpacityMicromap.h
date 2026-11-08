#pragma once
#include "Graphics/RHI/RHIAccelerationStructure.h"
#include "Graphics/RenderCore/V2/RenderCoreDefs.h"
#include "Graphics/RenderCore/V2/ResourceRetirement.h"
#include "Utils/SharedPtr.h"
#include <string>

namespace zen::rc
{
class RenderDevice;
class RenderScene;

// Immutable coverage for one indexed primitive. Generations can share it while
// positions and instance transforms change; alpha inputs are fingerprinted separately.
class SceneOpacityMicromap
{
public:
    explicit SceneOpacityMicromap(RenderDevice* device) : m_device(device) {}
    ~SceneOpacityMicromap();

    static SharedPtr<SceneOpacityMicromap> Prepare(RenderDevice*                           device,
                                                   RenderScene&                            scene,
                                                   const RHIAccelerationStructureGeometry& geometry,
                                                   uint32_t                                material,
                                                   const SharedPtr<SceneOpacityMicromap>&  previous = {});
    void                                   Attach(RHIAccelerationStructureGeometry& geometry) const;
    void                                   Record(RenderScene& scene, RHIBuffer* scratch, uint64_t scratchOffset);
    void                                   OnExecuted(RenderFrameResult result);
    void                                   PollCache();
    uint64_t                               GetBufferBytes() const;
    uint64_t                               GetScratchBytes() const
    {
        return m_built ? 0 : m_sizes.buildScratchSize;
    }
    RHIAccelerationStructure* GetStructure() const
    {
        return m_structure;
    }

private:
    bool                              Initialize(RenderScene&                            scene,
                                                 const RHIAccelerationStructureGeometry& geometry,
                                                 uint32_t                                material,
                                                 const SharedPtr<SceneOpacityMicromap>&  previous,
                                                 bool&                                   reused);
    RHIBuffer*                        Upload(uint32_t bytes, const void* data, NameID name);
    RHIAccelerationStructureBuildDesc Description() const;

    RenderDevice*                      m_device;
    RHIAccelerationStructure*          m_structure{nullptr};
    RHIAccelerationStructureBuildSizes m_sizes;
    RHIBuffer*                         m_mapping{nullptr};
    RHIBuffer*                         m_states{nullptr};
    RHIBuffer*                         m_triangles{nullptr};
    RHIBuffer*                         m_corners{nullptr};
    RHIBuffer*                         m_readback{nullptr};
    RHIOpacityMicromapUsage            m_usage;
    uint32_t                           m_stateBytes{0};
    uint32_t                           m_material{0};
    uint32_t                           m_samplerFlags{0};
    uint64_t                           m_key{0};
    uint64_t                           m_mappingHash{0};
    std::string                        m_cachePath;
    ResourceRetirement                 m_retirement;
    bool                               m_cached{false};
    bool                               m_built{false};
    bool                               m_recorded{false};
    bool                               m_cachePending{false};
};
} // namespace zen::rc
