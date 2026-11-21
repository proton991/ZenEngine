#include "RuntimeApplication.h"
#include "Platform/ConfigLoader.h"
#include "Platform/CommandLine.h"
#include <charconv>
#include <iostream>
#include <optional>
#include <limits>
#include <fstream>
#include <sstream>
#include <algorithm>

namespace zen::runtime
{
namespace
{
struct LaunchOverrides
{
    std::string                                     giConfiguration;
    std::string                                     giConfigPath;
    std::optional<RHIExecutionMode>                 rhi;
    std::optional<bool>                             fullscreen;
    std::optional<bool>                             vsync;
    std::optional<bool>                             validation;
    std::optional<bool>                             asyncCompute;
    std::optional<bool>                             gpuCulling;
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

// Parses "--capture-frames=a,b,c-d" into sorted, unique scene frames (each from 1, at most 4096 frames).
void ParseFrameList(std::string_view argument, HeapVector<uint64_t>& frames, std::string& error)
{
    const size_t     separator = argument.find('=');
    std::string_view text      = argument.substr(separator + 1);
    while (error.empty() && !text.empty())
    {
        const size_t           comma = text.find(',');
        const std::string_view item  = text.substr(0, comma);
        const size_t           dash  = item.find('-');
        uint64_t               first = 0;
        uint64_t               last  = 0;
        const std::string_view low   = item.substr(0, dash);
        const std::string_view high  = dash == std::string_view::npos ? low : item.substr(dash + 1);
        const bool parsedLow  = std::from_chars(low.data(), low.data() + low.size(), first).ptr == low.data() + low.size();
        const bool parsedHigh = std::from_chars(high.data(), high.data() + high.size(), last).ptr == high.data() + high.size();
        if (!parsedLow || !parsedHigh || low.empty() || high.empty() || first == 0 || last < first || last - first >= 4096
            || frames.size() + (last - first + 1) > 4096)
        {
            error = "--capture-frames requires comma-separated frames or ranges a-b from 1, at most 4096 frames";
        }
        for (uint64_t frame = first; error.empty() && frame <= last; ++frame)
        {
            frames.push_back(frame);
        }
        text = comma == std::string_view::npos ? std::string_view() : text.substr(comma + 1);
    }
    if (error.empty() && frames.empty())
    {
        error = "--capture-frames requires at least one frame";
    }
    std::sort(frames.begin(), frames.end());
    frames.erase(std::unique(frames.begin(), frames.end()), frames.end());
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

void ParseGIOverride(std::string_view argument, LaunchOverrides& overrides, std::string& error)
{
    // Names come from the GI option table shared with engine.cfg and the editor's GI settings panel.
    // Values are parsed by the same ConfigLoader and GI validator as Data/engine.cfg.
    const size_t              equals = argument.find('=');
    const std::string_view    name   = argument.substr(5, equals == std::string_view::npos ? equals : equals - 5);
    const std::string_view    value  = equals == std::string_view::npos ? std::string_view{} : argument.substr(equals + 1);
    const rc::GIConfigOption* option = rc::FindGIConfigArgument(name);
    if (option == nullptr || value.empty()
        || value.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789.+-E") != std::string_view::npos)
    {
        error = "Unknown or invalid GI override: " + std::string(argument);
    }
    else
    {
        overrides.giConfiguration += std::string(option->key) + "=" + std::string(value) + "\n";
    }
}

std::string GetGIOptionHelp()
{
    std::string help;
    for (const rc::GIConfigOption& option : rc::GetGIConfigOptions())
    {
        help += "       [--gi-" + rc::GetGIConfigArgumentName(option) + "=" + option.values + "]\n";
    }
    return help;
}

bool ApplyOverrides(const LaunchOverrides& overrides, RuntimeLaunchSettings& setup, std::string& error)
{
    bool giEnabled = true;
    if (!overrides.giConfigPath.empty())
    {
        std::ifstream input(std::filesystem::u8path(overrides.giConfigPath));
        if (!input)
        {
            error = "Cannot read GI configuration: " + overrides.giConfigPath;
        }
        else
        {
            platform::ConfigLoader config(input);
            if (config.ReadBool("voxel_gi_enabled", giEnabled) && rc::LoadVoxelGIRuntimeSettings(config, setup.rendering.gi))
            {
                setup.rendering.algorithm = giEnabled ? rc::RenderAlgorithm::eVoxelGI : rc::RenderAlgorithm::ePBR;
            }
            else
            {
                error = "Invalid GI configuration: " + overrides.giConfigPath;
            }
        }
    }
    if (error.empty() && !overrides.giConfiguration.empty())
    {
        std::istringstream     input(overrides.giConfiguration);
        platform::ConfigLoader config(input);
        if (config.ReadBool("voxel_gi_enabled", giEnabled)
            && rc::LoadVoxelGIRuntimeSettings(config, setup.rendering.gi, &setup.rendering.gi))
        {
            setup.rendering.algorithm = giEnabled ? rc::RenderAlgorithm::eVoxelGI : rc::RenderAlgorithm::ePBR;
        }
        else
        {
            error = "Invalid GI overrides:\n" + overrides.giConfiguration;
        }
    }
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
        setup.rendering.algorithm           = giEnabled ? rc::RenderAlgorithm::eVoxelGI : rc::RenderAlgorithm::ePBR;
        setup.rendering.gi.cone.rayProvider = *overrides.backend;
    }
    return error.empty();
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
        else if (argument.starts_with("--ground-truth="))
        {
            options.groundTruth = argument.substr(15);
        }
        else if (argument.starts_with("--output="))
        {
            options.output = argument.substr(9);
        }
        else if (argument.starts_with("--camera-path="))
        {
            options.cameraPath = argument.substr(14);
        }
        else if (argument.starts_with("--capture-start="))
        {
            ParseUnsigned(argument, options.captureStart, std::numeric_limits<uint64_t>::max(), error);
        }
        else if (argument.starts_with("--capture-frames="))
        {
            ParseFrameList(argument, options.captureFrames, error);
        }
        else if (argument.starts_with("--light-path="))
        {
            options.lightPath = argument.substr(13);
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
        else if (argument.starts_with("--gpu-culling="))
        {
            ParseToggle(argument, overrides.gpuCulling, error);
        }
        else if (argument.starts_with("--gi-config=") && argument.size() > 12)
        {
            overrides.giConfigPath = argument.substr(12);
        }
        else if (argument.starts_with("--gi-"))
        {
            ParseGIOverride(argument, overrides, error);
        }
        else if (argument == "--backend=auto" || argument == "--backend=compute" || argument == "--backend=hardware")
        {
            overrides.backend = argument == "--backend=auto"    ? rc::VoxelGISettings::RayProvider::Auto
                              : argument == "--backend=compute" ? rc::VoxelGISettings::RayProvider::Voxel
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
        else if (argument == "--pipeline-statistics")
        {
            options.pipelineStatistics = true;
        }
        else if (argument == "--hybrid-hit-stream")
        {
            options.hybridHitStream = true;
        }
        else if (argument == "--hybrid-ray-counts")
        {
            options.hybridRayCounts = true;
        }
        else if (argument == "--disable-rt")
        {
            options.disableRT = true;
        }
        else if (argument.starts_with("--device-memory-limit-mb="))
        {
            ParseUnsigned(argument, options.deviceMemoryLimitMB, 1024ull * 1024, error);
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
    if (error.empty() && !options.groundTruth.empty())
    {
        if (!path.empty() || options.output.empty() || options.startPaused || options.disableRT
            || options.deviceMemoryLimitMB != 0 || !options.capture.empty() || !options.profile.empty()
            || options.captureStart != 0 || overrides.rhi || overrides.fullscreen || overrides.vsync || overrides.validation
            || overrides.asyncCompute || overrides.gpuCulling || overrides.width || overrides.height || overrides.backend
            || !overrides.giConfiguration.empty() || !overrides.giConfigPath.empty())
        {
            error = "--ground-truth requires --output and uses the dataset's setup values without run overrides";
        }
        else if (ReadGroundTruthBatch(options.groundTruth, options.batch, error))
        {
            path           = options.groundTruth + "/frames/" + options.batch.front().id + "/scene.zenrender";
            options.frames = std::max(uint64_t(128), options.frames);
            for (const GroundTruthFrame& frame : options.batch)
            {
                options.frames = std::max(options.frames, uint64_t(frame.setup.rendering.gi.cone.historyFrames) + 32);
            }
            options.fixedStep = true;
            options.noUI      = true;
            if (options.frames > 4096)
            {
                error = "Ground-truth --frames must not exceed 4096";
            }
        }
    }
    if (error.empty() && path.empty())
    {
        error = "Specify --setup=<file.zenrender>";
    }
    if (error.empty() && !options.cameraPath.empty())
    {
        // Path keys name scene frames, so the timeline must not depend on wall-clock time.
        if (!options.fixedStep || !options.groundTruth.empty() || options.startPaused)
        {
            error = "--camera-path requires --fixed-step and cannot be combined with --ground-truth or --start-paused";
        }
        else
        {
            ReadCameraPath(options.cameraPath, options.cameraKeys, error);
        }
    }
    if (error.empty() && !options.capture.empty() && options.frames == 0 && !options.startPaused)
    {
        error = "--capture requires --frames or --start-paused";
    }
    if (error.empty() && options.hybridRayCounts && (options.frames < 2 || !options.groundTruth.empty()))
    {
        error = "--hybrid-ray-counts requires --frames=2 or more and cannot be combined with --ground-truth";
    }
    if (error.empty() && !options.lightPath.empty())
    {
        if (!options.fixedStep || !options.groundTruth.empty() || options.startPaused)
        {
            error = "--light-path requires --fixed-step and cannot be combined with --ground-truth or --start-paused";
        }
        else
        {
            ReadLightPath(options.lightPath, options.lightKeys, error);
        }
    }
    if (error.empty() && options.captureStart != 0
        && (options.capture.empty() || options.captureStart > options.frames || options.startPaused))
    {
        error = "--capture-start requires --capture and --frames >= start, without --start-paused";
    }
    if (error.empty() && !options.captureFrames.empty()
        && (options.capture.empty() || options.captureStart != 0 || options.startPaused
            || options.captureFrames.back() > options.frames))
    {
        error = "--capture-frames requires --capture and --frames >= its last frame, without --capture-start or --start-paused";
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
        std::cout
            << "ZenApp --setup=<file.zenrender> [--frames=N] [--capture=scene.ppm] [--profile=prefix]\n"
               "       [--fullscreen[=0|1] | --windowed] [--width=N] [--height=N]\n"
               "       [--rhi=inline|threaded] [--async-compute=0|1] [--vsync=0|1] [--validation=0|1]\n"
               "       [--backend=auto|compute|hardware] [--gpu-culling=0|1] [--disable-rt] [--gpu-markers] [--no-ui]\n"
               "       [--gi-config=engine.cfg (read like Data/engine.cfg; --gi-* options below override it)]\n"
               "       [--device-memory-limit-mb=N (caps device-local heaps; 8192 emulates an 8 GB GPU)]\n"
               "       [--pipeline-statistics (compiler diagnostics; exclude from timing runs)]\n"
               "       [--hybrid-hit-stream (optional compact secondary-hit buffers)]\n"
               "       [--hybrid-ray-counts (logs HybridTrace ray and candidate counts of the last two frames)]\n"
               "       [--fixed-step] [--start-paused] [--camera-path=<keys.json> (with --fixed-step)]\n"
               "       [--light-path=<keys.json> (with --fixed-step)]\n"
               "       [--capture-start=N (writes <capture>.frame-N.ppm each frame from N)]\n"
               "       [--capture-frames=a,b,c-d (writes <capture>.frame-N.ppm at only these frames)]\n"
               "GI options, the engine.cfg keys voxel_gi_<name> and the editor's GI settings (Copy as ZenApp arguments):\n"
            << GetGIOptionHelp()
            << "ZenApp --ground-truth=<dataset-directory> --output=<new-run-directory> [--frames=128]\n"
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
            valid               = ApplyOverrides(overrides, setup, error);
            options.rhiOverride = overrides.rhi.has_value();
            options.gpuCulling  = overrides.gpuCulling.value_or(true);
            valid               = valid && ValidateRuntimeLaunchSettings(setup, error);
        }
        if (valid)
        {
            const std::u8string log = std::filesystem::u8path(path).replace_extension(".log").u8string();
            options.logPath.assign(reinterpret_cast<const char*>(log.data()), log.size());
            if (!options.batch.empty())
            {
                std::error_code code;
                valid           = std::filesystem::create_directory(std::filesystem::u8path(options.output), code) && !code;
                options.logPath = options.output + "/ZenApp.log";
                if (!valid)
                {
                    error = "Ground-truth output must be a new directory with an existing parent";
                }
            }
            valid = valid
                 && zen::platform::ConfigLoader::SetExplicitSource(setup.engineConfiguration, setup.configurationDirectory);
            if (valid)
            {
                result = RunApplication(setup, options);
            }
            else
            {
                error = error.empty() ? "Runtime configuration was initialized before its launch setup" : error;
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
