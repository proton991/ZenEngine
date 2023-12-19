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
    unsigned int numThreads{1};
    // number of secondary command buffers
    unsigned int numSecondaryCmdBuffers{1};
};
} // namespace zen