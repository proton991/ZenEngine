#include "Graphics/Utils/GLSLCompiler.h"

ZEN_DISABLE_WARNINGS()
#include <glslang/SPIRV/GLSL.std.450.h>
#include <glslang/SPIRV/GlslangToSpv.h>
#include <glslang/StandAlone/ResourceLimits.h>
#include <glslang/glslang/Include/ShHandle.h>
#include <glslang/glslang/OSDependent/osinclude.h>
ZEN_ENABLE_WARNINGS()

namespace zen {
namespace {
inline EShLanguage FindShaderLanguage(vk::ShaderStageFlagBits stage) {
  switch (stage) {
    case vk::ShaderStageFlagBits::eVertex:
      return EShLangVertex;

    case vk::ShaderStageFlagBits::eTessellationControl:
      return EShLangTessControl;

    case vk::ShaderStageFlagBits::eTessellationEvaluation:
      return EShLangTessEvaluation;

    case vk::ShaderStageFlagBits::eGeometry:
      return EShLangGeometry;

    case vk::ShaderStageFlagBits::eFragment:
      return EShLangFragment;

    case vk::ShaderStageFlagBits::eCompute:
      return EShLangCompute;

    case vk::ShaderStageFlagBits::eRaygenKHR:
      return EShLangRayGen;

    case vk::ShaderStageFlagBits::eAnyHitKHR:
      return EShLangAnyHit;

    case vk::ShaderStageFlagBits::eClosestHitKHR:
      return EShLangClosestHit;

    case vk::ShaderStageFlagBits::eMissKHR:
      return EShLangMiss;

    case vk::ShaderStageFlagBits::eIntersectionKHR:
      return EShLangIntersect;

    case vk::ShaderStageFlagBits::eCallableKHR:
      return EShLangCallable;

    case vk::ShaderStageFlagBits::eMeshEXT:
      return EShLangMesh;

    case vk::ShaderStageFlagBits::eTaskEXT:
      return EShLangTask;

    default:
      return EShLangVertex;
  }
}
}  // namespace

glslang::EShTargetLanguage GLSLCompiler::envTargetLanguage =
    glslang::EShTargetLanguage::EShTargetNone;
glslang::EShTargetLanguageVersion GLSLCompiler::envTargetLanguageVersion =
    static_cast<glslang::EShTargetLanguageVersion>(0);

void GLSLCompiler::SetTargetEnvironment(glslang::EShTargetLanguage targetLanguage,
                                        glslang::EShTargetLanguageVersion targetLanguageVersion) {
  GLSLCompiler::envTargetLanguage        = targetLanguage;
  GLSLCompiler::envTargetLanguageVersion = targetLanguageVersion;
}

void GLSLCompiler::ResetTargetEnvironment() {
  GLSLCompiler::envTargetLanguage        = glslang::EShTargetLanguage::EShTargetNone;
  GLSLCompiler::envTargetLanguageVersion = static_cast<glslang::EShTargetLanguageVersion>(0);
}

bool GLSLCompiler::CompileToSpirv(vk::ShaderStageFlagBits stage,
                                  const std::vector<uint8_t>& glslSource,
                                  const std::string& entryPoint, const ShaderVariant& shaderVariant,
                                  std::vector<std::uint32_t>& spirv, std::string& infoLog) {
  // Initialize glslang library.
  glslang::InitializeProcess();

  EShMessages messages =
      static_cast<EShMessages>(EShMsgDefault | EShMsgVulkanRules | EShMsgSpvRules);

  EShLanguage language = FindShaderLanguage(stage);
  std::string source   = std::string(glslSource.begin(), glslSource.end());

  const char* fileNameList[1] = {""};
  const char* shaderSource    = reinterpret_cast<const char*>(source.data());

  glslang::TShader shader(language);
  shader.setStringsWithLengthsAndNames(&shaderSource, nullptr, fileNameList, 1);
  shader.setEntryPoint(entryPoint.c_str());
  shader.setSourceEntryPoint(entryPoint.c_str());
  shader.setPreamble(shaderVariant.GetPreamble().c_str());
  shader.addProcesses(shaderVariant.GetProcesses());
  if (GLSLCompiler::envTargetLanguage != glslang::EShTargetLanguage::EShTargetNone) {
    shader.setEnvTarget(GLSLCompiler::envTargetLanguage, GLSLCompiler::envTargetLanguageVersion);
  }

  if (!shader.parse(&glslang::DefaultTBuiltInResource, 100, false, messages)) {
    infoLog = std::string(shader.getInfoLog()) + "\n" + std::string(shader.getInfoDebugLog());
    return false;
  }

  // Add shader to new program object.
  glslang::TProgram program;
  program.addShader(&shader);

  // Link program.
  if (!program.link(messages)) {
    infoLog = std::string(program.getInfoLog()) + "\n" + std::string(program.getInfoDebugLog());
    return false;
  }

  // Save any info log that was generated.
  if (shader.getInfoLog()) {
    infoLog +=
        std::string(shader.getInfoLog()) + "\n" + std::string(shader.getInfoDebugLog()) + "\n";
  }

  if (program.getInfoLog()) {
    infoLog += std::string(program.getInfoLog()) + "\n" + std::string(program.getInfoDebugLog());
  }

  glslang::TIntermediate* intermediate = program.getIntermediate(language);

  // Translate to SPIRV.
  if (!intermediate) {
    infoLog += "Failed to get shared intermediate code.\n";
    return false;
  }

  spv::SpvBuildLogger logger;

  glslang::GlslangToSpv(*intermediate, spirv, &logger);

  infoLog += logger.getAllMessages() + "\n";

  // Shutdown glslang library.
  glslang::FinalizeProcess();

  return true;
}
}  // namespace zen