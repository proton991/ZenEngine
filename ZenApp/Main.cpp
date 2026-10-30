#include "RuntimeApplication.h"
#include "Platform/ConfigLoader.h"
#include "Platform/CommandLine.h"
#include <charconv>
#include <iostream>
#include <optional>

namespace zen::runtime
{
namespace
{
bool ParseOptions(int                              argc,
                  char**                           argv,
                  std::string&                     path,
                  RuntimeOptions&                  options,
                  std::optional<RHIExecutionMode>& rhi,
                  std::string&                     error)
{
    for (int index = 1; index < argc && error.empty(); ++index)
    {
        const std::string_view argument(argv[index]);
        if (argument == "--setup" && index + 1 < argc)
        {
            path = argv[++index];
        }
        else if (argument.starts_with("--setup="))
        {
            path = argument.substr(8);
        }
        else if (argument.starts_with("--frames="))
        {
            const std::string_view       value  = argument.substr(9);
            const std::from_chars_result parsed = std::from_chars(value.data(), value.data() + value.size(), options.frames);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || options.frames == 0)
            {
                error = "--frames requires a positive integer";
            }
        }
        else if (argument.starts_with("--capture="))
        {
            options.capture = argument.substr(10);
        }
        else if (argument.starts_with("--profile="))
        {
            options.profile = argument.substr(10);
        }
        else if (argument == "--rhi=inline" || argument == "--rhi=threaded")
        {
            rhi = argument == "--rhi=inline" ? RHIExecutionMode::eInline : RHIExecutionMode::eThreaded;
        }
        else if (argument == "--fixed-step")
        {
            options.fixedStep = true;
        }
        else if (argument == "--start-paused")
        {
            options.startPaused = true;
        }
        else
        {
            error = "Unknown or incomplete argument: " + std::string(argument);
        }
    }
    if (error.empty() && path.empty())
    {
        error = "Specify --setup=<file.zenrender>";
    }
    if (error.empty() && !options.capture.empty() && options.frames == 0 && !options.startPaused)
    {
        error = "--capture requires --frames or --start-paused";
    }
    return error.empty();
}
} // namespace
} // namespace zen::runtime

int RuntimeMain(int argc, char** argv)
{
    using namespace zen::runtime;
    RuntimeExit result = RuntimeExit::Setup;
    if (argc == 2 && std::string_view(argv[1]) == "--help")
    {
        std::cout << "ZenApp --setup=<file.zenrender> [--frames=N] [--capture=scene.ppm] [--profile=prefix]\n"
                     "       [--rhi=inline|threaded] [--fixed-step] [--start-paused]\n"
                     "Space: Pause/Resume. Escape: Quit. Camera controls match the editor.\n";
        result = RuntimeExit::Success;
    }
    else
    {
        std::string                          path;
        std::string                          error;
        RuntimeOptions                       options;
        RuntimeLaunchSettings                setup;
        std::optional<zen::RHIExecutionMode> rhi;
        bool                                 valid = ParseOptions(argc, argv, path, options, rhi, error)
                                                  && ReadRuntimeLaunchSettings(std::filesystem::u8path(path), setup, error);
        if (valid)
        {
            const std::u8string log = std::filesystem::u8path(path).replace_extension(".log").u8string();
            options.logPath.assign(reinterpret_cast<const char*>(log.data()), log.size());
            if (rhi.has_value())
            {
                options.rhiOverride = true;
                setup.run.rhiMode   = *rhi;
            }
            valid = zen::platform::ConfigLoader::SetExplicitSource(setup.engineConfiguration, setup.configurationDirectory);
            if (valid)
            {
                result = RunApplication(setup, options);
            }
            else
            {
                error = "Runtime configuration was initialized before its launch setup";
            }
        }
        if (!valid)
        {
            std::cerr << "ZenApp: " << error << '\n';
        }
    }
    return static_cast<int>(result);
}

#if defined(_WIN32)
int wmain(int argc, wchar_t** argv)
{
    return zen::platform::RunUTF8Main(argc, argv, &RuntimeMain);
}
#else
int main(int argc, char** argv)
{
    return RuntimeMain(argc, argv);
}
#endif
