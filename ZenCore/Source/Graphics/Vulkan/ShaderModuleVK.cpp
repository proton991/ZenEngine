#include "Graphics/Vulkan/ShaderModuleVK.h"
#include "Common/Error.h"
#include "Common/Exception.h"
#include "Common/Logging.h"
#include "Graphics/Utils/GLSLCompiler.h"
#include "Graphics/Utils/SpirvReflection.h"
#include "Platform/FileSystem.h"

namespace zen::vulkan {
std::vector<std::string> split(const std::string& input, char delim) {
  std::vector<std::string> tokens;

  std::stringstream sstream(input);
  std::string token;
  while (std::getline(sstream, token, delim)) {
    tokens.push_back(token);
  }

  return tokens;
}
/**
 * @brief Pre-compiles project shader files to include header code
 * @param source The shader file
 * @returns A byte array of the final shader
 */
inline std::vector<std::string> PrecompileShader(const std::string& source) {
  std::vector<std::string> final_file;

  auto lines = split(source, '\n');

  for (auto& line : lines) {
    if (line.find("#include \"") == 0) {
      // Include paths are relative to the base shader directory
      std::string includePath = line.substr(10);
      size_t lastQuotePos     = includePath.find("\"");
      if (!includePath.empty() && lastQuotePos != std::string::npos) {
        includePath = includePath.substr(0, lastQuotePos);
      }

      auto include_file = PrecompileShader(platform::fs::ReadShaderSource(includePath));
      for (auto& include_file_line : include_file) {
        final_file.push_back(include_file_line);
      }
    } else {
      final_file.push_back(line);
    }
  }

  return final_file;
}

inline std::vector<uint8_t> ConvertToBytes(std::vector<std::string>& lines) {
  std::vector<uint8_t> bytes;

  for (auto& line : lines) {
    line += "\n";
    std::vector<uint8_t> lineBytes(line.begin(), line.end());
    bytes.insert(bytes.end(), lineBytes.begin(), lineBytes.end());
  }

  return bytes;
}

ShaderVariant::ShaderVariant(std::string&& preamble, std::vector<std::string>&& processes)
    : m_preamble(std::move(preamble)), m_processes(std::move(processes)) {
  UpdateId();
}

ShaderVariant& ShaderVariant::AddDefine(const std::string& def) {
  m_processes.push_back("D" + def);

  std::string tmpDef = def;

  // The "=" needs to turn into a space
  size_t posEqual = tmpDef.find_first_of("=");
  if (posEqual != std::string::npos) {
    tmpDef[posEqual] = ' ';
  }
  m_preamble.append("#define " + tmpDef + "\n");

  UpdateId();

  return *this;
}

ShaderVariant& ShaderVariant::AddUnDefine(const std::string& undef) {
  m_processes.push_back("U" + undef);

  m_preamble.append("#undef " + undef + "\n");

  UpdateId();

  return *this;
}

ShaderVariant& ShaderVariant::AddRuntimeArraySize(const std::string& name, size_t size) {
  if (m_runtimeArraySizes.find(name) == m_runtimeArraySizes.end()) {
    m_runtimeArraySizes.insert({name, size});
  } else {
    m_runtimeArraySizes[name] = size;
  }
  return *this;
}

void ShaderVariant::UpdateId() {
  std::hash<std::string> hasher{};
  m_id = hasher(m_preamble);
}

void ShaderVariant::Clear() {
  m_preamble.clear();
  m_processes.clear();
  m_runtimeArraySizes.clear();
  UpdateId();
}

ShaderSource::ShaderSource(const std::string& name) : m_name(name) {
  m_srcCode = platform::fs::ReadShaderSource(m_name);
}

ShaderModule::ShaderModule(const Device& device, vk::ShaderStageFlagBits stage,
                           const ShaderSource& source, const ShaderVariant& shaderVariant,
                           std::string entryPoint /*= "main"*/)
    : DeviceResource(device, nullptr), m_stage(stage), m_entryPoint(std::move(entryPoint)) {
  const auto debugName = fmt::format("{} [variant {:X}] [entrypoint {}]", source.GetName(),
                                     shaderVariant.GetId(), m_entryPoint);
  SetDebugName(debugName);

  auto& sourceCode = source.GetSourceCode();
  if (sourceCode.empty()) {
    throw VulkanException(vk::Result::eErrorInitializationFailed);
  }

  auto finalSource = PrecompileShader(sourceCode);

  GLSLCompiler glslCompiler;
  if (!glslCompiler.CompileToSpirv(m_stage, ConvertToBytes(finalSource), m_entryPoint,
                                   shaderVariant, m_spirv, m_infoLog)) {
    LOGE("Shader compilation failed for shader \"{}\"", source.GetName());
    LOGE("{}", m_infoLog);
    throw VulkanException(vk::Result::eErrorInitializationFailed);
  }

  SPIRVReflection spirvReflection;

  // Reflect all shader resources
  if (!spirvReflection.ReflectShaderResources(m_stage, m_spirv, m_resources, shaderVariant)) {
    throw VulkanException(vk::Result::eErrorInitializationFailed);
  }

  // Generate a unique id, determined by source and variant
  std::hash<std::string> hasher{};
  m_id = hasher(std::string{reinterpret_cast<const char*>(m_spirv.data()),
                            reinterpret_cast<const char*>(m_spirv.data() + m_spirv.size())});
  // set shader module handle
  auto shaderModuleCI = vk::ShaderModuleCreateInfo().setCode(m_spirv);
  SetHandle(GetDeviceHandle().createShaderModule(shaderModuleCI));
}

ShaderModule::ShaderModule(ShaderModule&& other)
    : DeviceResource(std::move(other)),
      m_id(other.m_id),
      m_stage(other.m_stage),
      m_entryPoint(other.m_entryPoint),
      m_spirv(std::move(other.m_spirv)),
      m_resources(std::move(other.m_resources)),
      m_infoLog(std::move(other.m_infoLog)) {}

}  // namespace zen::vulkan