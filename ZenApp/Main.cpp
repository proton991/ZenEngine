#include "RuntimeApplication.h"
#include "Platform/ConfigLoader.h"
#include "Platform/CommandLine.h"
#include <charconv>
#include <iostream>
#include <optional>
#include <limits>

namespace zen::runtime
{
namespace
{
struct LaunchOverrides
{
    std::optional<RHIExecutionMode>                 rhi;
    std::optional<bool>                             fullscreen;
    std::optional<bool>                             vsync;
    std::optional<bool>                             validation;
    std::optional<bool>                             asyncCompute;
    std::optional<uint32_t>                         width;
    std::optional<uint32_t>                         height;
    std::optional<rc::VoxelGISettings::RayProvider> backend;
};

bool ParseUnsigned(std::string_view argument, uint64_t& value, uint64_t maximum, std::string& error)
{
    const size_t                 separator = argument.find('=');
    const std::string_view       text      = argument.substr(separator + 1);
    const std::from_chars_result parsed    = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value == 0 || value > maximum)
    {
        error = std::string(argument.substr(0, separator)) + " requires an integer from 1 to " + std::to_string(maximum);
    }
    return error.empty();
}

void ParseToggle(std::string_view argument, std::optional<bool>& value, std::string& error)
{
    const size_t           separator = argument.find('=');
    const std::string_view text      = argument.substr(separator + 1);
    if (text == "0" || text == "1")
    {
        value = text == "1";
    }
    else
    {
        error = std::string(argument.substr(0, separator)) + " requires 0 or 1";
    }
}

void ApplyOverrides(const LaunchOverrides& overrides, RuntimeLaunchSettings& setup)
{
    if (overrides.rhi.has_value())
    {
        setup.run.rhiMode = *overrides.rhi;
    }
    if (overrides.fullscreen.has_value())
    {
        setup.run.fullscreen = *overrides.fullscreen;
    }
    if (overrides.vsync.has_value())
    {
        setup.run.vsync       = *overrides.vsync;
        setup.run.presentMode = RHIPresentMode::eDefault;
    }
    if (overrides.validation.has_value())
    {
        setup.run.validation = *overrides.validation;
    }
    if (overrides.asyncCompute.has_value())
    {
        setup.rendering.gi.asyncCompute =
            *overrides.asyncCompute ? platform::AsyncComputeMode::eAuto : platform::AsyncComputeMode::eDisabled;
    }
    if (overrides.width.has_value())
    {
        setup.run.windowWidth  = *overrides.width;
        setup.run.renderWidth  = *overrides.width;
        setup.run.followWindow = false;
    }
    if (overrides.height.has_value())
    {
        setup.run.windowHeight = *overrides.height;
        setup.run.renderHeight = *overrides.height;
        setup.run.followWindow = false;
    }
    if (overrides.backend.has_value())
    {
        setup.rendering.algorithm = rc::RenderAlgorithm::eVoxelGI;
        setup.rendering.gi.cone.SetAllFeatureProviders(*overrides.backend);
    }
}

bool ParseOptions(int              argc,
                  char**           argv,
                  std::string&     path,
                  RuntimeOptions&  options,
                  LaunchOverrides& overrides,
                  std::string&     error)
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
            ParseUnsigned(argument, options.frames, std::numeric_limits<uint64_t>::max(), error);
        }
        else if (argument.starts_with("--width=") || argument.starts_with("--height="))
        {
            uint64_t value = 0;
            if (ParseUnsigned(argument, value, 8192, error))
            {
                std::optional<uint32_t>& dimension = argument.starts_with("--width=") ? overrides.width : overrides.height;
                dimension                          = static_cast<uint32_t>(value);
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
            overrides.rhi = argument == "--rhi=inline" ? RHIExecutionMode::eInline : RHIExecutionMode::eThreaded;
        }
        else if (argument == "--fullscreen" || argument == "--windowed")
        {
            overrides.fullscreen = argument == "--fullscreen";
        }
        else if (argument.starts_with("--fullscreen="))
        {
            ParseToggle(argument, overrides.fullscreen, error);
        }
        else if (argument.starts_with("--vsync="))
        {
            ParseToggle(argument, overrides.vsync, error);
        }
        else if (argument.starts_with("--validation="))
        {
            ParseToggle(argument, overrides.validation, error);
        }
        else if (argument.starts_with("--async-compute="))
        {
            ParseToggle(argument, overrides.asyncCompute, error);
        }
        else if (argument == "--backend=compute" || argument == "--backend=hardware")
        {
            overrides.backend = argument == "--backend=compute" ? rc::VoxelGISettings::RayProvider::Voxel
                                                                : rc::VoxelGISettings::RayProvider::Hardware;
        }
        else if (argument == "--no-ui")
        {
            options.noUI = true;
        }
        else if (argument == "--gpu-markers")
        {
            options.gpuMarkers = true;
        }
        else if (argument == "--disable-rt")
        {
            options.disableRT = true;
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
    if (error.empty() && options.disableRT && overrides.backend == rc::VoxelGISettings::RayProvider::Hardware)
    {
        error = "--backend=hardware cannot be combined with --disable-rt";
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
                     "       [--fullscreen[=0|1] | --windowed] [--width=N] [--height=N]\n"
                     "       [--rhi=inline|threaded] [--async-compute=0|1] [--vsync=0|1] [--validation=0|1]\n"
                     "       [--backend=compute|hardware] [--disable-rt] [--gpu-markers] [--no-ui]\n"
                     "       [--fixed-step] [--start-paused]\n"
                     "Fullscreen defaults on and uses the display's native mode; width/height set the render size.\n"
                     "Space: Pause/Resume. Escape: Quit. Camera controls match the editor.\n";
        result = RuntimeExit::Success;
    }
    else
    {
        std::string           path;
        std::string           error;
        RuntimeOptions        options;
        RuntimeLaunchSettings setup;
        LaunchOverrides       overrides;
        bool                  valid = ParseOptions(argc, argv, path, options, overrides, error)
                                   && ReadRuntimeLaunchSettings(std::filesystem::u8path(path), setup, error);
        if (valid)
        {
            ApplyOverrides(overrides, setup);
            options.rhiOverride = overrides.rhi.has_value();
            valid               = ValidateRuntimeLaunchSettings(setup, error);
        }
        if (valid)
        {
            const std::u8string log = std::filesystem::u8path(path).replace_extension(".log").u8string();
            options.logPath.assign(reinterpret_cast<const char*>(log.data()), log.size());
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
