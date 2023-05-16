#include "Platform/FileSystem.h"
#include <fstream>

namespace zen::platform::fs {
// declaration
std::string ReadTextFile(const std::string& path);

std::string ReadTextFile(const std::string& path) {
  std::ifstream file;
  file.open(path, std::ios::in);
  if (!file.is_open()) {
    throw std::runtime_error("Failed to open file: " + path);
  }

  return std::string{(std::istreambuf_iterator<char>(file)), (std::istreambuf_iterator<char>())};
}

std::string ReadShaderSource(const std::string& fileName) {
  return ReadTextFile(SHADER_PATH + fileName);
}

}  // namespace zen::platform::fs