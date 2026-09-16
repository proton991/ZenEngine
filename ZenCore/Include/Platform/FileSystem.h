#pragma once
#include <fstream>
#include <string>
#include <type_traits>
#include "Utils/Errors.h"
#include "Templates/HeapVector.h"

namespace zen::platform
{
enum class FileLoadError : uint8_t
{
    eNone,
    eOpenFailed,
    eSizeQueryFailed,
    eInvalidSize,
    eReadFailed
};

class FileSystem
{
public:
    // Failures are logged and return empty data. The optional error distinguishes
    // a failed read from a successfully loaded empty text file.
    static std::string LoadTextFile(const std::string& path, FileLoadError* pError = nullptr);

    template <typename T = uint8_t>
    static HeapVector<T> LoadSpvFile(const std::string& name, FileLoadError* pError = nullptr)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        const std::string path = std::string(SPV_SHADER_PATH) + name;
        std::ifstream file(path, std::ios::ate | std::ios::binary);
        HeapVector<T> buffer;
        FileLoadError error = FileLoadError::eNone;
        if (!file.is_open())
        {
            LOGE("Failed to open shader file: {}", path);
            error = FileLoadError::eOpenFailed;
        }
        else
        {
            const std::streampos end = file.tellg();
            if (end < 0)
            {
                LOGE("Failed to determine shader file size: {}", path);
                error = FileLoadError::eSizeQueryFailed;
            }
            else
            {
                const size_t fileSize = static_cast<size_t>(end);
                if (fileSize == 0 || fileSize % sizeof(uint32_t) != 0 || fileSize % sizeof(T) != 0)
                {
                    LOGE("Invalid SPIR-V file size ({} bytes): {}", fileSize, path);
                    error = FileLoadError::eInvalidSize;
                }
                else
                {
                    buffer.resize(fileSize / sizeof(T));
                    ASSERT(fileSize == buffer.size() * sizeof(T));
                    ASSERT(reinterpret_cast<uintptr_t>(buffer.data()) % alignof(uint32_t) == 0);
                    file.seekg(0);
                    if (!file.read(reinterpret_cast<char*>(buffer.data()),
                                   static_cast<std::streamsize>(fileSize)))
                    {
                        LOGE("Failed to read shader file: {}", path);
                        error = FileLoadError::eReadFailed;
                        buffer.clear();
                    }
                }
            }
        }

        if (pError != nullptr)
        {
            *pError = error;
        }
        return buffer;
    }
};
} // namespace zen::platform
