#include "Graphics/Val/SpirvReflection.h"
#include "Common/Errors.h"
#include "Graphics/Val/Shader.h"
#include "Graphics/Val/VulkanDebug.h"
#include "Platform/FileSystem.h"

namespace zen::val
{
ShaderModule::ShaderModule(const Device& device,
                           VkShaderStageFlagBits stage,
                           const std::string& name,
                           RuntimeArraySizes runtimeArraySizes) :
    DeviceObject(device), m_stage(stage), m_runtimeArraySizes(std::move(runtimeArraySizes))
{
    m_spirvCode = platform::FileSystem::LoadSpvFile(name);

    // create a new shader module, using the buffer we loaded
    VkShaderModuleCreateInfo createInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.pNext = nullptr;

    // codeSize has to be in bytes, so multiply the ints in the buffer by size of int to know the real size of the buffer
    createInfo.codeSize = m_spirvCode.size() * sizeof(uint32_t);
    createInfo.pCode    = m_spirvCode.data();

    CHECK_VK_ERROR_AND_THROW(
        vkCreateShaderModule(m_device.GetHandle(), &createInfo, nullptr, &m_handle),
        "Failed to create shader module");

    SetShaderModuleName(m_device.GetHandle(), m_handle, name.c_str());

    SpirvReflection::ReflectEntryPoint(m_spirvCode, m_entryPoint);

    SpirvReflection::ReflectShaderResources(m_stage, m_spirvCode, m_resources, m_runtimeArraySizes);

    std::hash<std::string> hasher{};
    m_id =
        hasher(std::string(reinterpret_cast<const char*>(m_spirvCode.data()),
                           reinterpret_cast<const char*>(m_spirvCode.data() + m_spirvCode.size())));
}

ShaderModule::ShaderModule(ShaderModule&& other) : DeviceObject(std::move(other))
{
    m_stage             = other.m_stage;
    m_spirvCode         = std::move(other.m_spirvCode);
    m_resources         = std::move(other.m_resources);
    m_runtimeArraySizes = std::move(other.m_runtimeArraySizes);
    m_entryPoint        = std::move(other.m_entryPoint);
    m_id                = other.m_id;
}

ShaderModule::~ShaderModule()
{
    if (m_handle != VK_NULL_HANDLE)
    {
        vkDestroyShaderModule(m_device.GetHandle(), m_handle, nullptr);
    }
}

void ShaderModule::SetResourceMode(const std::string& name, ShaderResourceMode mode)
{
    auto it =
        std::find_if(m_resources.begin(), m_resources.end(),
                     [&name](const ShaderResource& resource) { return resource.name == name; });

    if (it != m_resources.end())
    {
        if (mode == ShaderResourceMode::Dynamic)
        {
            if (it->type == ShaderResourceType::BufferUniform ||
                it->type == ShaderResourceType::BufferStorage)
            {
                it->mode = mode;
            }
            else
            {
                LOGW("Resource `{}` does not support dynamic.", name);
            }
        }
        else
        {
            it->mode = mode;
        }
    }
    else
    {
        LOGW("Resource `{}` not found for shader.", name);
    }
}

std::vector<ShaderResource>& ShaderModule::GetResources()
{
    return m_resources;
}
} // namespace zen::val
