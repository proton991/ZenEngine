#include "Graphics/VulkanRHI/VulkanRHI.h"
#include "Graphics/RHI/ShaderUtil.h"
#include <fstream>

using namespace zen;
using namespace zen::rhi;

std::vector<uint8_t> loadSpirvCode(const std::string& name)
{
    const auto path = std::string(SPV_SHADER_PATH) + name;
    std::ifstream file(path, std::ios::ate | std::ios::binary);

    if (!file.is_open()) { LOG_FATAL_ERROR("Failed to load shader source"); }

    //find what the size of the file is by looking up the location of the cursor
    //because the cursor is at the end, it gives the size directly in bytes
    size_t fileSize = (size_t)file.tellg();

    //spir-v expects the buffer to be on uint32, so make sure to reserve an int vector big enough for the entire file
    std::vector<uint8_t> buffer(fileSize / sizeof(uint8_t));

    //put file cursor at beginning
    file.seekg(0);

    //load the entire file into the buffer
    file.read((char*)buffer.data(), fileSize);

    //now that the file is loaded into the buffer, we can close it
    file.close();

    return buffer;
}

int main(int argc, char** argv)
{
    // std::vector<uint8_t> vsCode = loadSpirvCode("gbuffer.vert.spv");
    // std::vector<uint8_t> fsCode = loadSpirvCode("gbuffer.frag.spv");
    std::vector<uint8_t> vsCode = loadSpirvCode("main.vert.spv");
    std::vector<uint8_t> fsCode = loadSpirvCode("pbr.frag.spv");
    ShaderGroupInfo shaderGroupInfo{};
    auto shaderGroupSpirv = MakeRefCountPtr<ShaderGroupSPIRV>();
    shaderGroupSpirv->SetStageSPIRV(ShaderStage::eVertex, vsCode);
    shaderGroupSpirv->SetStageSPIRV(ShaderStage::eFragment, fsCode);
    ShaderUtil::ReflectShaderGroupInfo(shaderGroupSpirv, shaderGroupInfo);
    LOGI("Parse finished");
    ShaderUtil::PrintShaderGroupInfo(shaderGroupInfo);

    VulkanRHI vkRHI;
    vkRHI.Init();
    ShaderHandle shaderHdl = vkRHI.CreateShader(shaderGroupInfo);
    vkRHI.DestroyShader(shaderHdl);
}