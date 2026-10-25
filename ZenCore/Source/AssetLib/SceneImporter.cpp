#include "AssetLib/SceneImporter.h"
#include "AssetLib/SceneLoader.h"
#include "AssetLib/AssetPaths.h"
#include <algorithm>
#include <cctype>
#include <filesystem>

namespace zen::asset
{
const SceneFormatDescriptor* FindSceneFormat(const std::string& path, VectorView<const SceneFormatDescriptor> descriptors)
{
    std::string extension = PathToUtf8(std::filesystem::u8path(path).extension());
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    const SceneFormatDescriptor* result = nullptr;
    for (const SceneFormatDescriptor& format : descriptors)
    {
        for (std::string_view supported : format.extensions)
        {
            if (supported == extension)
            {
                result = &format;
            }
        }
    }
    return result;
}

static void RecordSceneImportFailure(SceneImportReport& result,
                                     SceneImportStatus  status,
                                     const std::string& source,
                                     std::string        message,
                                     std::string        object)
{
    result.status = status;
    result.error  = std::move(message);
    result.diagnostics.push_back({SceneDiagnosticSeverity::Error, status, source, std::move(object), result.error});
}

void SetSceneImportFailure(SceneLoadResult&   result,
                           SceneImportStatus  status,
                           const std::string& source,
                           std::string        message,
                           std::string        object)
{
    result.scene.Reset();
    result.asset.Reset();
    RecordSceneImportFailure(result, status, source, std::move(message), std::move(object));
}

void SetSceneImportFailure(SceneImportResult& result,
                           SceneImportStatus  status,
                           const std::string& source,
                           std::string        message,
                           std::string        object)
{
    result.asset.Reset();
    RecordSceneImportFailure(result, status, source, std::move(message), std::move(object));
}

} // namespace zen::asset
