#pragma once

#include <string>
#include <iostream>
#include <cstdint>
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <utility>
#include <atomic>

#include <spdlog/spdlog.h>

#define LOGT(...) spdlog::trace(__VA_ARGS__);
#define LOGI(...) spdlog::info(__VA_ARGS__);
#define LOGW(...) spdlog::warn(__VA_ARGS__);
#define LOGE(...) spdlog::error("[{}:{}] {}", __FILE__, __LINE__, fmt::format(__VA_ARGS__));
#define LOGD(...) spdlog::debug(__VA_ARGS__);

namespace zen
{
using VerificationReporter = void (*)(const char*) noexcept;

inline std::atomic<VerificationReporter> g_verificationReporter{nullptr};

// Applications can install a CPU/platform diagnostic, independent of the renderer.
inline void SetVerificationReporter(VerificationReporter reporter) noexcept
{
    g_verificationReporter.store(reporter, std::memory_order_release);
}

// Verification is a release-active invariant check. Keep its fatal path independent
// of logger configuration and diagnostic formatting, including during teardown.
[[noreturn]] inline void VerificationFailure(const char* expression,
                                             const char* file,
                                             int         line,
                                             const char* message = nullptr) noexcept
{
    std::fprintf(stderr, "ZenEngine: verification failed: %s (%s:%d)\n", expression, file, line);

    if (message != nullptr)
    {
        std::fprintf(stderr, "%s\n", message);
    }

    std::fflush(stderr);

    const VerificationReporter reporter = g_verificationReporter.load(std::memory_order_acquire);

    if (reporter != nullptr)
    {
        reporter(message != nullptr ? message : expression);
    }

    std::abort();
}

template <typename... Args> [[noreturn]] inline void VerificationFailureFormatted(const char*                 expression,
                                                                                  const char*                 file,
                                                                                  int                         line,
                                                                                  fmt::format_string<Args...> format,
                                                                                  Args&&... args) noexcept
{
    char message[1024]{};

    // Emit a fallback before formatting: even a foreign formatter terminating cannot
    // hide the original invariant. This fatal boundary never unwinds engine code.
    std::fprintf(stderr, "ZenEngine: verification failed: %s (%s:%d)\n", expression, file, line);

    std::fflush(stderr);

    const fmt::format_to_n_result<char*> formatted =
        fmt::format_to_n(message, sizeof(message) - 1, format, std::forward<Args>(args)...);

    message[std::min(formatted.size, sizeof(message) - 1)] = '\0';

    VerificationFailure(expression, file, line, message);
}

template <typename... ArgsType>
void LogError(bool isCritical, const char* pFunction, const char* pFullFilePath, int line, const ArgsType&... args)
{
    std::string fileName(pFullFilePath);

    const std::string::size_type LastSlashPos = fileName.find_last_of("/\\");
    if (LastSlashPos != std::string::npos)
    {
        fileName.erase(0, LastSlashPos + 1);
    }
    std::string message;
    if (isCritical)
    {
        message = fmt::format("ZenEngine: fatal error in {} ({}, {}): {}", pFunction, fileName, line, args...);
        spdlog::critical(message);
    }
    else
    {
        message = fmt::format("ZenEngine: error in {} ({}, {}): {}", pFunction, fileName, line, args...);
        spdlog::error(message);
    }
}

} // namespace zen

#define ASSERT(x) assert(x)

#define VERIFY_EXPR(x)                                        \
    do                                                        \
    {                                                         \
        if (!static_cast<bool>(x))                            \
        {                                                     \
            zen::VerificationFailure(#x, __FILE__, __LINE__); \
        }                                                     \
    } while (0)

#define VERIFY_EXPR_MSG(x, msg)                                    \
    do                                                             \
    {                                                              \
        if (!(x))                                                  \
        {                                                          \
            zen::VerificationFailure(#x, __FILE__, __LINE__, msg); \
        }                                                          \
    } while (0)

#define VERIFY_EXPR_MSG_F(x, ...)                                                   \
    do                                                                              \
    {                                                                               \
        if (!(x))                                                                   \
        {                                                                           \
            zen::VerificationFailureFormatted(#x, __FILE__, __LINE__, __VA_ARGS__); \
        }                                                                           \
    } while (0)

#define LOG_ERROR(...)                                                                     \
    do                                                                                     \
    {                                                                                      \
        zen::LogError(/*IsFatal=*/false, __FUNCTION__, __FILE__, __LINE__, ##__VA_ARGS__); \
    } while (false)

#define LOG_FATAL_ERROR(...)                                                              \
    do                                                                                    \
    {                                                                                     \
        zen::LogError(/*IsFatal=*/true, __FUNCTION__, __FILE__, __LINE__, ##__VA_ARGS__); \
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

#define CHECK_VK_ERROR(err, ...)                                                             \
    {                                                                                        \
        if (err != VK_SUCCESS)                                                               \
            zen::LogError(/*IsFatal=*/false, __FUNCTION__, __FILE__, __LINE__, __VA_ARGS__); \
    }

#define ASSERT_SIZEOF(Struct, Size, ...) \
    static_assert(sizeof(Struct) == Size, "sizeof(" #Struct ") is expected to be " #Size ". " __VA_ARGS__)

#if UINTPTR_MAX == UINT64_MAX
#    define ASSERT_SIZEOF64 ASSERT_SIZEOF
#else
#    define ASSERT_SIZEOF64(...)
#endif
