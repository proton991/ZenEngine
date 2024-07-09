#pragma once

#include <stdexcept>
#include <string>
#include <iostream>
#include <cstdint>

#include <spdlog/spdlog.h>

#define LOGT(...) spdlog::trace(__VA_ARGS__);
#define LOGI(...) spdlog::info(__VA_ARGS__);
#define LOGW(...) spdlog::warn(__VA_ARGS__);
#define LOGE(...) spdlog::error("[{}:{}] {}", __FILE__, __LINE__, fmt::format(__VA_ARGS__));
#define LOGD(...) spdlog::debug(__VA_ARGS__);

namespace zen
{

template <bool> void ThrowIf(std::string&&) {}

template <> inline void ThrowIf<true>(std::string&& msg)
{
    throw std::runtime_error(msg);
}

template <bool bThrowException, typename... ArgsType> void LogError(bool isCritical,
                                                                    const char* function,
                                                                    const char* fullFilePath,
                                                                    int line,
                                                                    const ArgsType&... args)
{
    std::string fileName(fullFilePath);

    auto LastSlashPos = fileName.find_last_of("/\\");
    if (LastSlashPos != std::string::npos)
        fileName.erase(0, LastSlashPos + 1);
    std::string message;
    if (isCritical)
    {
        message = fmt::format("ZenEngine: fatal error in {} ({}, {}): {}", function, fileName, line,
                              args...);
        spdlog::critical(message);
    }
    else
    {
        message =
            fmt::format("ZenEngine: error in {} ({}, {}): {}", function, fileName, line, args...);
        spdlog::error(message);
    }
    ThrowIf<bThrowException>(std::move(message));
}

} // namespace zen

#define ASSERT(x) assert(x)

#define VERIFY_EXPR(x)                                            \
    do                                                            \
    {                                                             \
        if (!bool(x))                                             \
        {                                                         \
            spdlog::error("Error at {}:{}.", __FILE__, __LINE__); \
        }                                                         \
    } while (0)

#define VERIFY_EXPR_MSG(x, msg)                                         \
    do                                                                  \
    {                                                                   \
        if (!(x))                                                       \
        {                                                               \
            spdlog::error("Error at {}:{} - " msg, __FILE__, __LINE__); \
        }                                                               \
    } while (0)

#define VERIFY_EXPR_MSG_F(x, fmt, ...)                                               \
    do                                                                               \
    {                                                                                \
        if (!(x))                                                                    \
        {                                                                            \
            spdlog::error("Error at {}:{} - " fmt, __FILE__, __LINE__, __VA_ARGS__); \
        }                                                                            \
    } while (0)

#define LOG_ERROR(...)                                                                       \
    do                                                                                       \
    {                                                                                        \
        LogError<false>(/*IsFatal=*/false, __FUNCTION__, __FILE__, __LINE__, ##__VA_ARGS__); \
    } while (false)


#define LOG_FATAL_ERROR(...)                                                                \
    do                                                                                      \
    {                                                                                       \
        LogError<false>(/*IsFatal=*/true, __FUNCTION__, __FILE__, __LINE__, ##__VA_ARGS__); \
    } while (false)

#define LOG_ERROR_ONCE(...)             \
    do                                  \
    {                                   \
        static bool IsFirstTime = true; \
        if (IsFirstTime)                \
        {                               \
            LOG_ERROR(##__VA_ARGS__);   \
            IsFirstTime = false;        \
        }                               \
    } while (false)


#define LOG_ERROR_AND_THROW(...)                                                            \
    do                                                                                      \
    {                                                                                       \
        LogError<true>(/*IsFatal=*/false, __FUNCTION__, __FILE__, __LINE__, ##__VA_ARGS__); \
    } while (false)

#define LOG_FATAL_ERROR_AND_THROW(...)                                                     \
    do                                                                                     \
    {                                                                                      \
        LogError<true>(/*IsFatal=*/true, __FUNCTION__, __FILE__, __LINE__, ##__VA_ARGS__); \
    } while (false)

#define CHECK_VK_ERROR(err, ...)                                                               \
    {                                                                                          \
        if (err != VK_SUCCESS)                                                                 \
            LogError<false>(/*IsFatal=*/false, __FUNCTION__, __FILE__, __LINE__, __VA_ARGS__); \
    }

#define CHECK_VK_ERROR_AND_THROW(err, ...)                                                    \
    {                                                                                         \
        if (err != VK_SUCCESS)                                                                \
            LogError<true>(/*IsFatal=*/false, __FUNCTION__, __FILE__, __LINE__, __VA_ARGS__); \
    }

#define ASSERT_SIZEOF(Struct, Size, ...)  \
    static_assert(sizeof(Struct) == Size, \
                  "sizeof(" #Struct ") is expected to be " #Size ". " __VA_ARGS__)

#if UINTPTR_MAX == UINT64_MAX
#    define ASSERT_SIZEOF64 ASSERT_SIZEOF
#else
#    define ASSERT_SIZEOF64(...)
#endif
