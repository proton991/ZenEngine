#pragma once
#include <string>
#define SHADER_PATH "../Data/Shaders/"

namespace zen::platform::fs {
std::string ReadShaderSource(const std::string& fileName);

}  // namespace zen::platform::fs
