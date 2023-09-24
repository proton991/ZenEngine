#include "Graphics/Val/SpirvReflection.h"
#include "Common/Errors.h"
#include "Graphics/Val/Shader.h"
#include "Graphics/Val/VulkanDebug.h"

#include <fstream>

namespace zen::val
{
std::vector<uint32_t> LoadSpvFile(const std::string& name)
{
    const auto    path = std::string(SPV_SHADER_PATH) + name;
    std::ifstream file(path, std::ios::ate | std::ios::binary);

    if (!file.is_open())
    {
        LOG_FATAL_ERROR("Failed to load shader source");
    }

    //find what the size of the file is by looking up the location of the cursor
    //because the cursor is at the end, it gives the size directly in bytes
    size_t fileSize = (size_t)file.tellg();

    //spir-v expects the buffer to be on uint32, so make sure to reserve an int vector big enough for the entire file
    std::vector<uint32_t> buffer(fileSize / sizeof(uint32_t));

    //put file cursor at beginning
    file.seekg(0);

    //load the entire file into the buffer
    file.read((char*)buffer.data(), fileSize);

    //now that the file is loaded into the buffer, we can close it
    file.close();

    return buffer;
}

ShaderModule::ShaderModule(Device& device, VkShaderStageFlagBits stage, const std::string& name, RuntimeArraySizes runtimeArraySizes) :
    DeviceObject(device), m_stage(stage), m_runtimeArraySizes(std::move(runtimeArraySizes))
{
    m_spirvCode = LoadSpvFile(name);

    // create a new shader module, using the buffer we loaded
    VkShaderModuleCreateInfo createInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.pNext = nullptr;

    // codeSize has to be in bytes, so multiply the ints in the buffer by size of int to know the real size of the buffer
    createInfo.codeSize = m_spirvCode.size() * sizeof(uint32_t);
    createInfo.pCode    = m_spirvCode.data();

    CHECK_VK_ERROR_AND_THROW(vkCreateShaderModule(m_device.GetHandle(), &createInfo, nullptr, &m_handle), "Failed to create shader module");

    SetShaderModuleName(m_device.GetHandle(), m_handle, name.c_str());

    SpirvReflection::ReflectEntryPoint(m_spirvCode, m_entryPoint);

    SpirvReflection::ReflectShaderResources(m_stage, m_spirvCode, m_resources, m_runtimeArraySizes);

    std::hash<std::string> hasher{};
    m_id = hasher(std::string(reinterpret_cast<const char*>(m_spirvCode.data()), reinterpret_cast<const char*>(m_spirvCode.data() + m_spirvCode.size())));
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
    auto it = std::find_if(m_resources.begin(), m_resources.end(), [&name](const ShaderResource& resource) { return resource.name == name; });

    if (it != m_resources.end())
    {
        if (mode == ShaderResourceMode::Dynamic)
        {
            if (it->type == ShaderResourceType::BufferUniform || it->type == ShaderResourceType::BufferStorage)
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
