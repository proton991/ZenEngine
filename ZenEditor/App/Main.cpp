#include "EditorApplication.h"
#include <charconv>
#include <cstdio>
#include <string_view>

namespace
{
bool ParseOptions(int argc, char** argv, zen::editor::EditorOptions& options)
{
    bool valid = true;

    for (int index = 1; index < argc; ++index)
    {
        const std::string_view argument(argv[index]);

        if (argument.starts_with("--scene="))
        {
            options.scene = argument.substr(8);
        }
        else if (argument.starts_with("--environment="))
        {
            options.environment = argument.substr(14);
        }
        else if (argument.starts_with("--settings-dir="))
        {
            options.settingsDirectory = argument.substr(15);
        }
        else if (argument.starts_with("--capture="))
        {
            options.capture = argument.substr(10);
        }
        else if (argument.starts_with("--frames="))
        {
            const std::string_view value        = argument.substr(9);

            const std::from_chars_result parsed = std::from_chars(value.data(), value.data() + value.size(), options.frames);

            valid = valid && parsed.ec == std::errc() && parsed.ptr == value.data() + value.size() && options.frames > 0;
        }
        else if (argument == "--rhi=inline" || argument == "--rhi=threaded")
        {
            options.threaded = argument == "--rhi=threaded";
        }
        else if (argument == "--scale=1" || argument == "--scale=1.5" || argument == "--scale=2")
        {
            options.scale = argument == "--scale=1" ? 1.0f : argument == "--scale=1.5" ? 1.5f : 2.0f;
        }
        else if (argument == "--mode=pbr" || argument == "--mode=gi" || argument == "--mode=voxels")
        {
            options.mode = argument == "--mode=pbr" ? zen::rc::RenderOption::ePBR
                         : argument == "--mode=gi"  ? zen::rc::RenderOption::eVoxelGI
                                                    : zen::rc::RenderOption::eVoxelize;
        }
        else if (argument == "--windowed")
        {
            options.windowed = true;
        }
        else if (argument == "--hidden")
        {
            options.hidden = true;
        }
        else if (argument == "--smoke-test")
        {
            options.smokeTest = true;
        }
        else
        {
            valid = false;
        }
    }

    if (options.smokeTest && options.frames == 0)
    {
        options.frames = 40;
    }

    valid =
        valid && (options.capture.empty() || options.frames != 0) && (options.environment.empty() || !options.scene.empty());

    return valid;
}
} // namespace

int main(int argc, char** argv)
{
    zen::editor::EditorOptions options;

    int result = 1;

    if (ParseOptions(argc, argv, options))
    {
        zen::editor::EditorApplication application(std::move(options));

        result = application.Run();
    }
    else
    {
        std::fprintf(
            stderr,
            "Usage: zen_editor [--scene=path.gltf|path.glb] [--rhi=inline|threaded] [--frames=N] "
            "[--environment=path.hdr|path.ktx|path.dds] [--mode=pbr|gi|voxels] [--scale=1|1.5|2] [--settings-dir=path] [--capture=path.ppm] [--smoke-test] [--hidden] [--windowed]\n");
    }

    return result;
}
