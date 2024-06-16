#pragma once
#include <string>
#include <vector>

namespace zen::platform
{
class FileSystem
{
public:
    static std::string LoadTextFile(const std::string& path);
    static std::vector<uint32_t> LoadSpvFile(const std::string& name);
    ;
};
std::string ReadShaderSource(const std::string& fileName);

} // namespace zen::platform
