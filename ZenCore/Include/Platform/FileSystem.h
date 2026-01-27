#pragma once
#include <fstream>
#include <string>
#include "Utils/Errors.h"
#include "Templates/HeapVector.h"

namespace zen::platform
{
class FileSystem
{
public:
    static std::string LoadTextFile(const std::string& path);

    template <typename T = uint8_t> static HeapVector<T> LoadSpvFile(const std::string& name)
    {
        const auto path = std::string(SPV_SHADER_PATH) + name;
        std::ifstream file(path, std::ios::ate | std::ios::binary);
        VERIFY_EXPR_MSG_F(file.is_open(), "Failed to load shader file {}", path);
        //find what the size of the file is by looking up the location of the cursor
        //because the cursor is at the end, it gives the size directly in bytes
        size_t fileSize = file.tellg();

        //spir-v expects the buffer to be on uint32, so make sure to reserve an int vector big enough for the entire file
        HeapVector<T> buffer(fileSize / sizeof(T));

        ASSERT(fileSize % sizeof(uint32_t) == 0);
        ASSERT(fileSize == buffer.size());
        ASSERT(reinterpret_cast<uintptr_t>(buffer.data()) % alignof(uint32_t) == 0);

        //put file cursor at beginning
        file.seekg(0);

        //load the entire file into the buffer
        file.read((char*)buffer.data(), fileSize);

        //now that the file is loaded into the buffer, we can close it
        file.close();

        return buffer;
    }
};
} // namespace zen::platform
