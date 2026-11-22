#include "AssetLib/ProcessedSceneAdapter.h"
#include <ZenAsset/Reader/Scene.h>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <string_view>

namespace
{
bool Measure(const char* path, uint32_t iteration, bool warmup)
{
    const std::chrono::steady_clock::time_point readStart = std::chrono::steady_clock::now();
    const zenasset::SceneReadResult             decoded   = zenasset::ReadScene(path);
    const std::chrono::steady_clock::time_point readEnd   = std::chrono::steady_clock::now();
    bool                                        valid     = decoded.status.Succeeded() && decoded.scene;
    if (valid)
    {
        const zen::asset::SceneImportResult         adapted  = zen::asset::AdaptProcessedScene(*decoded.scene);
        const std::chrono::steady_clock::time_point adaptEnd = std::chrono::steady_clock::now();
        valid = adapted.status == zen::asset::SceneImportStatus::Success && adapted.asset;
        if (valid)
        {
            std::printf("ZEN_PACKAGE_BENCH {\"iteration\":%u,\"warmup\":%s,\"read_seconds\":%.9f,"
                        "\"adapt_seconds\":%.9f,\"vertices\":%zu,\"indices\":%zu,\"nodes\":%zu}\n",
                        iteration, warmup ? "true" : "false", std::chrono::duration<double>(readEnd - readStart).count(),
                        std::chrono::duration<double>(adaptEnd - readEnd).count(), adapted.asset->vertices.size(),
                        adapted.asset->indices.size(), adapted.asset->nodes.size());
        }
        else
        {
            std::fprintf(stderr, "Adaptation failed: %s\n", adapted.error.c_str());
        }
    }
    else
    {
        std::fprintf(stderr, "Package read failed: %s\n", decoded.status.message.c_str());
    }
    return valid;
}
} // namespace

int main(int argc, char** argv)
{
    bool     valid       = argc == 3;
    uint32_t repetitions = 0;
    if (valid)
    {
        const std::string_view       text(argv[2]);
        const std::from_chars_result parsed = std::from_chars(text.data(), text.data() + text.size(), repetitions);
        valid = parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && repetitions > 0 && repetitions <= 1000;
    }
    if (valid)
    {
        valid = Measure(argv[1], 0, true);
        for (uint32_t index = 0; valid && index < repetitions; ++index)
        {
            valid = Measure(argv[1], index, false);
        }
    }
    else
    {
        std::fprintf(stderr, "Usage: ScenePackageBenchmark <package.zenasset> <repetitions:1..1000>\n");
    }
    return valid ? 0 : 1;
}
