#include "AssetTools/SceneTools.h"
#include "AssetLib/SceneLoader.h"
#include "AssetLib/SceneAssetIO.h"
#include <chrono>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#if defined(_WIN32)
#    include <Windows.h>
#    include <DbgHelp.h>
#    pragma comment(lib, "Dbghelp.lib")
#endif

using namespace zen;

namespace
{
#if defined(_WIN32)
// Prints the faulting thread's stack, so a failed benchmark run identifies its cause.
void PrintStack()
{
    HANDLE process = GetCurrentProcess();
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
    SymInitialize(process, nullptr, TRUE);
    void*                     frames[64]{};
    const uint16_t            count = CaptureStackBackTrace(0, 64, frames, nullptr);
    alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + 512]{};
    SYMBOL_INFO*              symbol = reinterpret_cast<SYMBOL_INFO*>(storage);
    symbol->SizeOfStruct             = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen               = 511;
    for (uint16_t index = 0; index < count; ++index)
    {
        DWORD64         displacement = 0;
        DWORD           column       = 0;
        IMAGEHLP_LINE64 line{};
        line.SizeOfStruct = sizeof(line);
        const bool named  = SymFromAddr(process, reinterpret_cast<DWORD64>(frames[index]), &displacement, symbol) != 0;
        const bool lined  = SymGetLineFromAddr64(process, reinterpret_cast<DWORD64>(frames[index]), &column, &line) != 0;
        std::fprintf(stderr, "  #%u %s (%s:%lu)\n", index, named ? symbol->Name : "?", lined ? line.FileName : "?",
                     lined ? line.LineNumber : 0);
    }
    std::fflush(stderr);
}

void OnAbort(int)
{
    std::fprintf(stderr, "SceneLoadBenchmark aborted:\n");
    PrintStack();
    TerminateProcess(GetCurrentProcess(), 3);
}

LONG WINAPI OnException(EXCEPTION_POINTERS* exception)
{
    std::fprintf(stderr, "SceneLoadBenchmark exception 0x%08lx:\n", exception->ExceptionRecord->ExceptionCode);
    PrintStack();
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

struct BenchmarkOptions
{
    std::string scene;
    std::string cacheDirectory;
    uint32_t    warmRuns{1};
    bool        keepCache{false};
};

double Seconds(std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

bool ParseOptions(int argc, char** argv, BenchmarkOptions& options)
{
    bool valid = argc >= 2;
    for (int index = 1; valid && index < argc; ++index)
    {
        const std::string_view argument(argv[index]);
        if (argument.starts_with("--cache-dir="))
        {
            options.cacheDirectory = std::string(argument.substr(12));
        }
        else if (argument.starts_with("--warm-runs="))
        {
            options.warmRuns = static_cast<uint32_t>(std::stoul(std::string(argument.substr(12))));
        }
        else if (argument == "--keep-cache")
        {
            options.keepCache = true;
        }
        else if (!argument.starts_with("--") && options.scene.empty())
        {
            options.scene = std::string(argument);
        }
        else
        {
            valid = false;
        }
    }
    return valid && !options.scene.empty();
}

const char* StageName(asset::SceneImportStage stage)
{
    static const char* names[]{
        "queued",   "reading", "decoding-textures", "processing-meshes", "baking-animations", "asset-ready", "building-scene",
        "complete", "failed",  "cancelled"};
    const uint32_t index = static_cast<uint32_t>(stage);
    return index < std::size(names) ? names[index] : "unknown";
}

// Polls the request's stage so the report shows where a load spends its time.
asset::SceneLoadResult TimedLoad(asset::SourceSceneLoader& loader, asset::SceneImportRequest request, const char* label)
{
    request.control.Reset(new asset::SceneImportControl);
    const std::chrono::steady_clock::time_point start  = std::chrono::steady_clock::now();
    std::future<asset::SceneLoadResult>         future = loader.LoadAsync(request);
    asset::SceneImportStage                     stage  = asset::SceneImportStage::Queued;
    while (future.wait_for(std::chrono::milliseconds(5)) != std::future_status::ready)
    {
        const asset::SceneImportStage current = request.control->GetStage();
        if (current != stage)
        {
            stage = current;
            std::printf("ZEN_SCENE_STAGE %s %s at=%.3f\n", label, StageName(stage), Seconds(start));
            std::fflush(stdout);
        }
    }
    return future.get();
}

bool Report(const char* label, const asset::SceneLoadResult& result, double seconds)
{
    const bool loaded = bool(result.scene);
    if (loaded)
    {
        std::printf("ZEN_SCENE_LOAD %s seconds=%.3f cacheHit=%d vertices=%zu indices=%zu textures=%zu diagnostics=%zu\n", label,
                    seconds, result.cacheHit ? 1 : 0, result.scene->vertices.size(), result.scene->indices.size(),
                    result.scene->scene->GetComponents<sg::Texture>().size(), result.diagnostics.size());
    }
    else
    {
        std::printf("ZEN_SCENE_LOAD %s failed after %.3f seconds: %s\n", label, seconds, result.error.c_str());
    }
    std::fflush(stdout);
    return loaded;
}
} // namespace

// Times the application load path: a cold source import that publishes the
// internal cache, then warm loads that must reuse it.
int main(int argc, char** argv)
{
#if defined(_WIN32)
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    std::signal(SIGABRT, OnAbort);
    SetUnhandledExceptionFilter(OnException);
#endif
    int              exitCode = 0;
    BenchmarkOptions options;
    if (!ParseOptions(argc, argv, options))
    {
        std::fprintf(stderr, "Usage: SceneLoadBenchmark <scene> [--cache-dir=path] [--warm-runs=N] [--keep-cache]\n");
        exitCode = 2;
    }
    else
    {
        asset::SceneImportRequest request{options.scene};
        request.cacheDirectory = options.cacheDirectory;
        request.useCache       = true;
        if (!options.keepCache && !options.cacheDirectory.empty())
        {
            std::error_code error;
            std::filesystem::remove_all(std::filesystem::u8path(options.cacheDirectory), error);
        }
        asset::SourceSceneLoader loader(1, 0);

        std::chrono::steady_clock::time_point start             = std::chrono::steady_clock::now();
        asset::SceneLoadResult                cold              = TimedLoad(loader, request, "cold");
        bool                                  valid             = Report("cold", cold, Seconds(start));
        cold                                                    = {};
        start                                                   = std::chrono::steady_clock::now();
        const HeapVector<asset::SceneImportDiagnostic> failures = asset::WaitForSceneAssetCacheWrites();
        std::printf("ZEN_SCENE_LOAD cache-publish seconds=%.3f failures=%zu\n", Seconds(start), failures.size());
        for (const asset::SceneImportDiagnostic& failure : failures)
        {
            std::printf("ZEN_SCENE_LOAD cache-failure %s\n", failure.message.c_str());
        }

        for (uint32_t run = 0; valid && run < options.warmRuns; ++run)
        {
            start                       = std::chrono::steady_clock::now();
            asset::SceneLoadResult warm = TimedLoad(loader, request, "warm");
            valid                       = Report("warm", warm, Seconds(start)) && warm.cacheHit;
        }
        exitCode = valid ? 0 : 1;
    }
    return exitCode;
}
