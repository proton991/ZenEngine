#pragma once
#include <vulkan/vulkan.h>
#include <unordered_map>
#include <string>
#include <vector>

namespace zen::val
{
class Device;

std::vector<uint32_t> LoadSpvFile(const std::string& name);

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
    VkShaderStageFlagBits stages;

    ShaderResourceType type;

    ShaderResourceMode mode{ShaderResourceMode::Static};

    uint32_t set;

    uint32_t binding;

    uint32_t location;

    uint32_t inputAttachmentIndex;

    uint32_t vecSize;

    uint32_t columns;

    uint32_t arraySize;

    uint32_t offset;

    uint32_t size;

    uint32_t constantId;

    uint32_t qualifiers;

    std::string name;
};

using RuntimeArraySizes = std::unordered_map<std::string, uint32_t>;

class ShaderModule
{
public:
    ShaderModule(Device& device, VkShaderStageFlagBits stage, const std::string& name, RuntimeArraySizes runtimeArraySizes);

    ~ShaderModule();

    VkShaderModule GetHandle() const { return m_handle; }

    const std::vector<uint32_t>& GetSpvCode() const { return m_spirvCode; };

    auto& GetEntryPoint() const { return m_entryPoint; }

    std::vector<ShaderResource>& GetResources();

    void SetResourceMode(const std::string& name, ShaderResourceMode mode);

    VkShaderStageFlagBits GetStage() const { return m_stage; }

private:
    Device&                     m_device;
    VkShaderStageFlagBits       m_stage{};
    std::vector<uint32_t>       m_spirvCode;
    VkShaderModule              m_handle{VK_NULL_HANDLE};
    std::vector<ShaderResource> m_resources;
    RuntimeArraySizes           m_runtimeArraySizes;
    std::string                 m_entryPoint;
};
} // namespace zen::val