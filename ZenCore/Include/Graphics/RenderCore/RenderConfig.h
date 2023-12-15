#pragma once

namespace zen
{
struct RenderConfig
{
    // Singleton instance getter
    static RenderConfig& GetInstance()
    {
        static RenderConfig instance; // Create a single instance
        return instance;
    }

    bool useSecondaryCmd{false};
    // number of threads
    unsigned int threadCount{1};
};
} // namespace zen