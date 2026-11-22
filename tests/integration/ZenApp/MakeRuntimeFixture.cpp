#include "AssetTools/SceneTools.h"
#include "AssetLib/SceneAssetIO.h"
#include "Runtime/RuntimeLaunchSettings.h"
#include <filesystem>
#include <iostream>

namespace
{
std::string Utf8(const std::filesystem::path& path)
{
    const std::u8string text = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}
} // namespace

// Small prepared scene and launch snapshot for native runtime acceptance checks.
// Source tooling is confined to this fixture executable, never linked into ZenApp.
int main(int argc, char** argv)
{
    bool        valid = argc == 2;
    std::string error = valid ? std::string{} : "Usage: MakeRuntimeFixture <output-directory>";
    if (valid)
    {
        std::error_code             filesystemError;
        const std::filesystem::path directory = std::filesystem::absolute(std::filesystem::u8path(argv[1]), filesystemError);
        valid                                 = !filesystemError;
        if (valid)
        {
            std::filesystem::create_directories(directory, filesystemError);
            valid = !filesystemError;
        }
        if (valid)
        {
            zen::asset::SceneImportRequest request{ZEN_RUNTIME_FIXTURE};
            request.useCache                   = false;
            zen::asset::SceneLoadResult loaded = zen::asset::LoadSourceScene(request);
            valid                              = loaded.scene && loaded.asset;
            error                              = loaded.error;
            if (valid)
            {
                const zen::asset::SceneImportReport written =
                    zen::asset::WriteSceneAssetFile(Utf8(directory / "scene.zenasset"), *loaded.asset, {});
                valid = written.status == zen::asset::SceneImportStatus::Success;
                error = written.error;
                if (valid)
                {
                    zen::runtime::RuntimeLaunchSettings setup;
                    setup.packagePath                      = Utf8(directory / "scene.zenasset");
                    setup.sourcePath                       = request.path;
                    setup.processingProfile                = "reference";
                    setup.configurationDirectory           = Utf8(directory);
                    setup.engineConfiguration              = "voxel_resolution=64\nshadow_map_resolution=256\n";
                    setup.defaultEnvironment               = "papermill.ktx";
                    setup.rendering.scenePath              = request.path;
                    setup.rendering.normalizationCenter    = loaded.scene->normalizationCenter;
                    setup.rendering.normalizationScale     = loaded.scene->normalizationScale;
                    setup.rendering.gi.resolution          = 64;
                    setup.rendering.gi.shadowMapResolution = 256;
                    setup.rendering.cameraLight.enabled    = true;
                    setup.run.windowWidth                  = 640;
                    setup.run.windowHeight                 = 480;
                    setup.run.renderWidth                  = 256;
                    setup.run.renderHeight                 = 256;
                    setup.run.followWindow                 = false;
                    setup.run.validation                   = true;
                    setup.run.vsync                        = false;
                    setup.camera.aspect                    = 1.0f;
                    valid = valid && zen::runtime::WriteRuntimeLaunchSettings(directory / "scene.zenrender", setup, error);
                }
            }
        }
        if (filesystemError)
        {
            error = filesystemError.message();
        }
    }
    if (!valid)
    {
        std::cerr << error << '\n';
    }
    return valid ? 0 : 1;
}
