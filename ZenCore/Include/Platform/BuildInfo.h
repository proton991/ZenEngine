#pragma once
#include <string>

namespace zen::platform
{
#if defined(NDEBUG)
inline constexpr const char* BuildConfiguration = "Release";
#else
inline constexpr const char* BuildConfiguration = "Debug";
#endif

inline std::string BuildApplicationTitle(const char* applicationName)
{
    return std::string(applicationName) + " [" + BuildConfiguration + "]";
}
} // namespace zen::platform
