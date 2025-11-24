#include "Platform/FileSystem.h"
#include "Utils/Errors.h"
#include <fstream>

namespace zen::platform
{
std::string FileSystem::LoadTextFile(const std::string& path)
{
    std::ifstream file;
    file.open(path, std::ios::in);
    if (!file.is_open())
    {
        throw std::runtime_error("Failed to open file: " + path);
    }

    return std::string{(std::istreambuf_iterator<char>(file)), (std::istreambuf_iterator<char>())};
}

// template <typename T> std::vector<T> FileSystem::LoadSpvFile(const std::string& name)
// {
//     const auto path = std::string(SPV_SHADER_PATH) + name;
//     std::ifstream file(path, std::ios::ate | std::ios::binary);
//     VERIFY_EXPR_MSG_F(file.is_open(), "Failed to load shader file {}", path);
//     //find what the size of the file is by looking up the location of the cursor
//     //because the cursor is at the end, it gives the size directly in bytes
//     size_t fileSize = file.tellg();
//
//     //spir-v expects the buffer to be on uint32, so make sure to reserve an int vector big enough for the entire file
//     std::vector<T> buffer(fileSize / sizeof(T));
//
//     //put file cursor at beginning
//     file.seekg(0);
//
//     //load the entire file into the buffer
//     file.read((char*)buffer.data(), fileSize);
//
//     //now that the file is loaded into the buffer, we can close it
//     file.close();
//
//     return buffer;
// }

} // namespace zen::platform