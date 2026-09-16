#include "Platform/FileSystem.h"
#include "Utils/Errors.h"
#include <fstream>

namespace zen::platform
{
std::string FileSystem::LoadTextFile(const std::string& path, FileLoadError* pError)
{
    std::ifstream file(path);
    std::string text;
    FileLoadError error = FileLoadError::eNone;
    if (!file.is_open())
    {
        LOGE("Failed to open text file: {}", path);
        error = FileLoadError::eOpenFailed;
    }
    else
    {
        char chunk[4096];
        while (file.read(chunk, sizeof(chunk)) || file.gcount() > 0)
        {
            text.append(chunk, static_cast<size_t>(file.gcount()));
        }
        if (file.bad() || !file.eof())
        {
            LOGE("Failed to read text file: {}", path);
            error = FileLoadError::eReadFailed;
            text.clear();
        }
    }

    if (pError != nullptr)
    {
        *pError = error;
    }
    return text;
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
