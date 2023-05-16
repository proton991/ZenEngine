#pragma once
#include "Common/Base.h"

ZEN_DISABLE_WARNINGS()
#include <glslang/Public/ShaderLang.h>
ZEN_ENABLE_WARNINGS()
#include "Graphics/Vulkan/ShaderModuleVK.h"

using namespace zen::vulkan;
namespace zen {  /// Helper class to generate SPIRV code from GLSL source
/// A very simple version of the glslValidator application
class GLSLCompiler {
private:
  static glslang::EShTargetLanguage envTargetLanguage;
  static glslang::EShTargetLanguageVersion envTargetLanguageVersion;

public:
  /**
	 * @brief Set the glslang target environment to translate to when generating code
	 * @param target_language The language to translate to
	 * @param target_language_version The version of the language to translate to
	 */
  static void SetTargetEnvironment(glslang::EShTargetLanguage targetLanguage,
                                   glslang::EShTargetLanguageVersion targetLanguageVersion);

  /**
	 * @brief Reset the glslang target environment to the default values
	 */
  static void ResetTargetEnvironment();

  /**
	 * @brief Compiles GLSL to SPIRV code
	 * @param stage The Vulkan shader stage flag
	 * @param glsl_source The GLSL source code to be compiled
	 * @param entry_point The entrypoint function name of the shader stage
	 * @param shader_variant The shader variant
	 * @param[out] spirv The generated SPIRV code
	 * @param[out] info_log Stores any log messages during the compilation process
	 */
  bool CompileToSpirv(vk::ShaderStageFlagBits stage, const std::vector<uint8_t>& glslSource,
                      const std::string& entryPoint, const ShaderVariant& shaderVariant,
                      std::vector<std::uint32_t>& spirv, std::string& infoLog);
};
}  // namespace zen