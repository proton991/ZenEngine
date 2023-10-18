#pragma once
#include "RenderGraphDefinitions.h"
#include "Common/UniquePtr.h"
#include <vector>
#include <vulkan/vulkan.h>
#include <unordered_set>
#include <unordered_map>
#include "RenderDevice.h"

namespace zen
{

class RenderGraph;

class RDGResource
{
public:
    enum class Type
    {
        Buffer,
        Image
    };

    template <typename T> T* As() { return dynamic_cast<T*>(this); }

    template <typename T> const T* As() const { return dynamic_cast<const T*>(this); }

    RDGResource(Index index, Type type, Tag tag) :
        m_index(index), m_type(type), m_tag(std::move(tag))
    {}

    virtual ~RDGResource() = default;

    void WriteInPass(Index index) { m_writtenInPasses.insert(index); }

    void ReadInPass(Index index) { m_readInPasses.insert(index); }

    const auto& GetReadInPasses() const { return m_readInPasses; }
    const auto& GetWrittenInPasses() const { return m_writtenInPasses; }

    void SetPhysicalIndex(Index index) { m_physicalIndex = index; }
    auto GetPhysicalIndex() const { return m_physicalIndex; }

    void ClearState()
    {
        m_readInPasses.clear();
        m_writtenInPasses.clear();
    }

    auto GetType() const { return m_type; }

    auto GetTag() const { return m_tag; }

private:
    Index m_index;
    Index m_physicalIndex;
    Type  m_type;
    Tag   m_tag;

    std::unordered_set<Index> m_readInPasses;
    std::unordered_set<Index> m_writtenInPasses;
};

class RDGImage : public RDGResource
{
public:
    explicit RDGImage(Index index, Tag tag) :
        RDGResource(index, RDGResource::Type::Image, std::move(tag))
    {}

    struct Info
    {
        VkFormat      format{VK_FORMAT_UNDEFINED};
        uint32_t      samples{1};
        uint32_t      levels{1};
        uint32_t      layers{1};
        ImageSizeType sizeType{ImageSizeType::SwapchainRelative};
        float         sizeX{1.0};
        float         sizeY{1.0};
        float         sizeZ{0.0};
        VkExtent3D    extent3D{};
    };

    void SetInfo(const Info& info) { m_info = info; }

    void AddImageUsage(val::ImageUsage flag) { m_usage |= flag; }

    const auto& GetInfo() { return m_info; }

    auto GetUsage() const { return m_usage; }

private:
    Info            m_info{};
    val::ImageUsage m_usage{val::ImageUsage::Undefined};
};

class RDGBuffer : public RDGResource
{
public:
    explicit RDGBuffer(Index index, Tag tag) :
        RDGResource(index, RDGResource::Type::Buffer, std::move(tag))
    {}

    struct Info
    {
        VkDeviceSize size{0};
    };

    void SetInfo(const Info& info) { m_info = info; }

    const auto& GetInfo() { return m_info; }

    void AddBufferUsage(val::BufferUsage flag) { m_usage |= flag; }

    auto GetUsage() const { return m_usage; }

private:
    Info             m_info{};
    val::BufferUsage m_usage{};
};

/**
 * @brief A render graph pass in a render graph
 * @note Distinguish between vulkan's render pass
 */
class RDGPass
{
public:
    RDGPass(RenderGraph& graph, Index index, Tag tag, RDGQueueFlags queueFlags);

    void WriteToColorImage(const Tag& tag, const RDGImage::Info& info);

    void WriteToStorageBuffer(const Tag& tag, const RDGBuffer::Info& info);

    void WriteToTransferDstBuffer(const Tag& tag, const RDGBuffer::Info& info);

    void WriteToDepthStencilImage(const Tag& tag, const RDGImage::Info& info);

    void ReadFromInternalImage(const Tag& tag, val::ImageUsage usage);

    void ReadFromInternalBuffer(const Tag& tag, val::BufferUsage usage);

    void ReadFromExternalImage(const Tag& tag, val::Image* image);

    void ReadFromExternalBuffer(const Tag& tag, val::Buffer* buffer);

    const auto& GetIndex() const { return m_index; }
    const auto& GetOutImageResources() const { return m_outImageResources; }
    const auto& GetOutBufferResources() const { return m_outBufferResources; }
    const auto& GetInImageResources() const { return m_inImagesResources; }
    const auto& GetInBufferResources() const { return m_inBufferResources; }
    const auto& GetExternImageResources() const { return m_externImageResources; }
    const auto& GetExternBufferResources() const { return m_externBufferResources; }
    const auto& GetTag() const { return m_tag; }

    void SetPhysicalIndex(Index index) { m_physicalIndex = index; }

    void UseShaders(std::vector<val::ShaderModule*>&& shaders) { m_shaders = std::move(shaders); }

    const auto& GetUsedShaders() const { return m_shaders; }

    void BindSRD(const Tag&            rdgResourceTag,
                 VkShaderStageFlagBits shaderStage,
                 const std::string&    shaderResourceName);

    void BindSampler(const Tag& tag, val::Sampler* sampler) { m_samplerBinding[tag] = sampler; }

    auto& GetSRDBinding() const { return m_srdBinding; }

    auto& GetSamplerBinding() const { return m_samplerBinding; }

    void SetOnExecute(std::function<void(val::CommandBuffer*)> func)
    {
        m_onExecute = std::move(func);
    }

    auto GetOnExecute() const { return m_onExecute; }

private:
    RenderGraph&  m_graph;
    Index         m_index;
    Index         m_physicalIndex;
    Tag           m_tag;
    RDGQueueFlags m_queueFlags;
    // output resources
    std::vector<RDGImage*>  m_outImageResources;
    std::vector<RDGBuffer*> m_outBufferResources;
    // input resources
    std::unordered_map<Tag, val::ImageUsage>  m_inImagesResources;
    std::unordered_map<Tag, val::BufferUsage> m_inBufferResources;
    // clear screen
    bool m_clearScreen{false};

    // used shaders
    std::vector<val::ShaderModule*> m_shaders;

    std::unordered_map<Tag, val::ShaderResource> m_srdBinding;
    std::unordered_map<Tag, val::Sampler*>       m_samplerBinding;
    std::unordered_map<Tag, val::Image*>         m_externImageResources;
    std::unordered_map<Tag, val::Buffer*>        m_externBufferResources;

    std::function<void(val::CommandBuffer*)> m_onExecute{[](auto*) {
    }};
};

struct ImageTransition
{
    val::ImageUsage srcUsage;
    val::ImageUsage dstUsage;
};

struct BufferTransition
{
    val::BufferUsage srcUsage;
    val::BufferUsage dstUsage;
};

struct ResourceState
{
    using ImageTransitionMap  = std::unordered_map<Tag, ImageTransition>;
    using BufferTransitionMap = std::unordered_map<Tag, BufferTransition>;
    std::unordered_map<Tag, ImageTransitionMap>  perPassImageState;
    std::unordered_map<Tag, BufferTransitionMap> perPassBufferState;
    std::unordered_map<Tag, val::ImageUsage>     totalImageUsages;
    std::unordered_map<Tag, val::BufferUsage>    totalBufferUsages;
    std::unordered_map<Tag, Tag>                 imageFirstUsePass;
    std::unordered_map<Tag, Tag>                 imageLastUsePass;
    std::unordered_map<Tag, Tag>                 bufferFirstUsePass;
    std::unordered_map<Tag, Tag>                 bufferLastUsePass;
};
class RenderContext;
class RenderGraph
{
public:
    explicit RenderGraph(RenderDevice& device) : m_renderDevice(device) {}

    RDGImage*  GetImageResource(const Tag& tag);
    RDGBuffer* GetBufferResource(const Tag& tag);

    void SetBackBufferSize(uint32_t width, uint32_t height);

    RDGPass* AddPass(const Tag& tag, RDGQueueFlags queueFlags);

    void SetBackBufferTag(const Tag& tag);

    void Compile();

    void Execute(val::CommandBuffer* commandBuffer, RenderContext* renderContext);

    const auto& GetResourceIndexMap() const { return m_resourceToIndex; }

private:
    struct PhysicalPass
    {
        val::PipelineLayout*         pipelineLayout{nullptr};
        val::PipelineState           pipelineState{};
        val::GraphicsPipeline*       graphicPipeline{nullptr};
        val::RenderPass*             renderPass{nullptr};
        std::vector<VkDescriptorSet> descriptorSets;
        UniquePtr<val::Framebuffer>  framebuffer;
        uint32_t                     index{0};
        bool                         descriptorSetsUpdated{false};

        std::function<void(val::CommandBuffer*)> onExecute{[](auto*) {
        }};
    };

    void SortRenderPasses();

    void TraversePassDepsRecursive(Index passIndex, uint32_t level);

    static void RemoveDuplicates(std::vector<Index>& list);

    void ResolveResourceState();

    void BuildPhysicalImage(RDGImage* image);

    void BuildPhysicalBuffer(RDGBuffer* buffer);

    void BuildPhysicalResources();

    void BuildPhysicalPasses();

    void EmitPipelineBarrier(val::CommandBuffer*                              commandBuffer,
                             const std::unordered_map<Tag, ImageTransition>&  imageTransitions,
                             const std::unordered_map<Tag, BufferTransition>& bufferTransitions);

    void BeforeExecuteSetup(val::CommandBuffer* commandBuffer);

    void CopyToPresentImage(val::CommandBuffer* commandBuffer, const val::Image& presentImage);

    void RunPass(PhysicalPass& pass, val::CommandBuffer* commandBuffer);

    void UpdateDescriptorSets(PhysicalPass& pass);

    std::unordered_map<Tag, Index>         m_resourceToIndex;
    std::unordered_map<Tag, Index>         m_passToIndex;
    std::vector<UniquePtr<RDGResource>>    m_resources;
    std::vector<UniquePtr<RDGPass>>        m_passes;
    std::vector<std::unordered_set<Index>> m_passDeps;
    std::vector<Index>                     m_sortedPassIndices;
    // Tracking resource state
    ResourceState m_resourceState;
    // Actual Physical resources
    std::vector<UniquePtr<val::Image>>  m_physicalImages;
    std::vector<UniquePtr<val::Buffer>> m_physicalBuffers;
    // Actual physical passes
    std::vector<PhysicalPass> m_physicalPasses;
    // Swapchain back buffer info
    Tag        m_backBufferTag;
    VkExtent2D m_backBufferExtent{};

    bool m_initialized{false};

    RenderDevice& m_renderDevice;
};
} // namespace zen