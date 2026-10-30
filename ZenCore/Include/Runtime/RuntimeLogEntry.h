#pragma once
#include <cstdint>
#include <string>

namespace zen::runtime
{
struct RuntimeLogEntry
{
    int         level{0};
    std::string text;
    uint64_t    sequence{0};
};
} // namespace zen::runtime
