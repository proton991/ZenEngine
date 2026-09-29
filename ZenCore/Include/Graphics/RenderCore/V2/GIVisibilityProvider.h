#pragma once
#include "Graphics/RenderCore/V2/Renderer/VoxelizerBase.h"
#include "Graphics/Shared/GIVisibility.h"

namespace zen::rc
{
class RenderScene;
class SceneShadowRenderer;

enum class GIVisibilityBackend
{
    eVoxelDDA,
    eDeterministic
};

enum class GIQueryStage
{
    eConformance,
    eStaticCache,
    eSenderLighting,
    eSenderEnvironment,
    eGather
};

struct GIQueryVariant
{
    bool environment{false};
    bool meshLightVisibility{false};
};

struct GIStaticVisibilityKey
{
    uint64_t generation{0};
    glm::uvec4 settings{};
};

struct GIVisibilityInfo
{
    GIVisibilityBackend backend{GIVisibilityBackend::eVoxelDDA};
    uint64_t generation{0};
    uint32_t precision{GI_CELL_PRECISION};
    bool ready{false};
    // DDA coverage bounds and the class-completeness mask. Coverage/material
    // acceptance is the voxelized opaque/masked approximation, not exact triangles.
    GIGridUniform grid{};
    bool compactCache{false};
};

// Prepared snapshots borrow resources. Binding declares them to RDG, which retains
// the actual handles through submission. Changing a provider never mutates a recorded pass.
class GIVisibilityProvider
{
public:
    virtual ~GIVisibilityProvider() = default;

    virtual bool BindQueryInputs(RDGComputePassDesc& pass) const = 0;

    virtual bool BindLightingInputs(RDGComputePassDesc& pass,
                                    const RenderScene* scene,
                                    const SceneShadowRenderer* shadows) const = 0;

    virtual NameID GetShader(GIQueryStage stage, GIQueryVariant variant = {}) const = 0;

    virtual GIStaticVisibilityKey GetStaticCacheKey(uint64_t listGeneration) const = 0;

    const GIVisibilityInfo& GetInfo() const
    {
        return m_info;
    }

protected:
    GIVisibilityInfo m_info;
};

class VoxelDDAProvider final : public GIVisibilityProvider
{
public:
    bool Prepare(const VoxelTextures& staticVoxels,
                 const VoxelTextures& dynamicVoxels,
                 const GIGridUniform& grid,
                 uint64_t generation);

    bool BindQueryInputs(RDGComputePassDesc& pass) const override;

    bool BindLightingInputs(RDGComputePassDesc& pass,
                            const RenderScene* scene,
                            const SceneShadowRenderer* shadows) const override;

    NameID GetShader(GIQueryStage stage, GIQueryVariant variant = {}) const override;

    GIStaticVisibilityKey GetStaticCacheKey(uint64_t listGeneration) const override;

private:
    VoxelTextures m_static;
    VoxelTextures m_dynamic;
};

// Diagnostic provider: supplied decoded responses exercise the same consumer ABI
// and RDG binding path without depending on traversal. It is not a shipping backend.
class DeterministicGIProvider final : public GIVisibilityProvider
{
public:
    bool Prepare(RHIBuffer* responses, uint32_t count, uint64_t generation, const RHIGPUInfo& gpu);

    bool BindQueryInputs(RDGComputePassDesc& pass) const override;

    bool BindLightingInputs(RDGComputePassDesc& pass,
                            const RenderScene* scene,
                            const SceneShadowRenderer* shadows) const override;

    NameID GetShader(GIQueryStage stage, GIQueryVariant variant = {}) const override;

    GIStaticVisibilityKey GetStaticCacheKey(uint64_t listGeneration) const override;

private:
    RHIBuffer* m_responses{nullptr};
    uint32_t m_count{0};
};

// M1 consumer harness; later gathers call the same GLSL query functions.
bool BuildGIQueryPass(RenderGraph& graph,
                      const GIVisibilityProvider& provider,
                      RHIBuffer* requests,
                      RHIBuffer* results,
                      uint32_t count,
                      const RHIGPUInfo& gpu,
                      RDGQueuePreference queue = RDGQueuePreference::eDefault);
} // namespace zen::rc
