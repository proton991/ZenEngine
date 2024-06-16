#pragma once
#include <utility>

#include "Graphics/Val/Shader.h"
#include "Platform/FileSystem.h"

namespace zen
{
class ShaderManager
{
    using ShaderCache = std::unordered_map<std::string, val::ShaderModule>;

public:
    explicit ShaderManager(const val::Device& valDevice) : m_valDevice(valDevice) {}

    val::ShaderModule* RequestShader(const std::string& fileName,
                                     VkShaderStageFlagBits stage,
                                     val::RuntimeArraySizes runtimeArraySizes)
    {
        if (!m_shaderCache.count(fileName))
        {
            auto shader =
                val::ShaderModule(m_valDevice, stage, fileName, std::move(runtimeArraySizes));
            m_shaderCache.emplace(fileName, std::move(shader));
        }
        return &m_shaderCache.at(fileName);
    }

private:
    const val::Device& m_valDevice;
    ShaderCache m_shaderCache;
};
} // namespace zen