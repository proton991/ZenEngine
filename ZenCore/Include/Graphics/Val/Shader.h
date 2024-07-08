#pragma once
#include <vulkan/vulkan.h>
#include <string>
#include <vector>
#include "DeviceObject.h"

namespace zen::val
{
enum class ShaderResourceType
{
    Input,
    InputAttachment,
    Output,
    Image,
    ImageSampler,
    ImageStorage,
    Sampler,
    BufferUniform,
    BufferStorage,
    PushConstant,
    SpecializationConstant,
    All
};

/// A bitmask of qualifiers applied to a resource
struct ShaderResourceQualifiers
{
    enum : uint32_t
    {
        None        = 0,
        NonReadable = 1,
        NonWritable = 2,
    };
};

/// This determines the type and method of how descriptor set should be created and bound
enum class ShaderResourceMode
{
    Static,
    Dynamic,
    UpdateAfterBind
};

// Store shader resource data.
// Used by the shader module.
struct ShaderResource
{
    VkFormat format{VK_FORMAT_UNDEFINED};

    VkShaderStageFlags stages;

    ShaderResourceType type;

    ShaderResourceMode mode{ShaderResourceMode::Static};

    uint32_t set;

    uint32_t binding;

    uint32_t location;

    uint32_t inputAttachmentIndex;

    uint32_t vecSize;

    //    uint32_t byteSize;

    uint32_t columns;

    uint32_t arraySize;

    uint32_t offset;

    uint32_t size;

    uint32_t constantId;

    uint32_t qualifiers;

    std::string name;
};

using RuntimeArraySizes = std::unordered_map<std::string, uint32_t>;

class ShaderModule : public DeviceObject<VkShaderModule, VK_OBJECT_TYPE_SHADER_MODULE>
{
public:
    ShaderModule(const Device& device,
                 VkShaderStageFlagBits stage,
                 const std::string& name,
                 RuntimeArraySizes runtimeArraySizes);

    ~ShaderModule();

    ShaderModule(ShaderModule&& other);

    const std::vector<uint32_t>& GetSpvCode() const
    {
        return m_spirvCode;
    };

    auto& GetEntryPoint() const
    {
        return m_entryPoint;
    }

    std::vector<ShaderResource>& GetResources();

    void SetResourceMode(const std::string& name, ShaderResourceMode mode);

    VkShaderStageFlagBits GetStage() const
    {
        return m_stage;
    }

    auto GetId() const
    {
        return m_id;
    }

private:
    VkShaderStageFlagBits m_stage{};
    std::vector<uint32_t> m_spirvCode;
    std::vector<ShaderResource> m_resources;
    RuntimeArraySizes m_runtimeArraySizes;
    std::string m_entryPoint;
    // Unique id
    size_t m_id;
};
} // namespace zen::val