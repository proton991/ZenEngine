#include "AssetLib/SceneImporter.h"
#include "AssetLib/SceneLoader.h"
#include "AssetLib/GltfAssetReader.h"
#include "AssetLib/FbxSceneImporter.h"
#include "AssetLib/SceneAssetProcessing.h"
#include "AssetLib/AssetPaths.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>

namespace zen::asset
{
namespace
{
class GltfSceneImporter final : public ISceneImporter
{
public:
    SceneImportResult Import(const SceneImportRequest& request, const SceneImportContext& context) const override
    {
        SceneImportResult result;
        {
            GltfAssetReader       importer(context.processing);
            UniquePtr<SceneAsset> candidate = importer.LoadAsset(request.path, request.settings, request.control.Get());
            if (candidate && ProcessImportedAsset(*candidate, result, request.path))
            {
                result.asset.Reset(candidate.Get());
                candidate.Release();
                result.status = SceneImportStatus::Success;
            }
            else if (candidate)
            {
                candidate.Reset();
            }
            else
            {
                const SceneImportStatus status =
                    request.control && request.control->IsCancelled() ? SceneImportStatus::Cancelled : importer.GetStatus();
                SetSceneImportFailure(result, status, request.path, importer.GetError());
            }
        }
        return result;
    }
};

const std::string_view      gltfExtensions[]{".gltf", ".glb"};
const std::string_view      fbxExtensions[]{".fbx"};
const SceneFormatDescriptor formats[]{{"gltf", "glTF scenes", VectorView<const std::string_view>(gltfExtensions), 1},
                                      {"fbx", "FBX scenes", VectorView<const std::string_view>(fbxExtensions), 1}};
const GltfSceneImporter     gltfImporter;
const FbxSceneImporter      fbxImporter;
const ISceneImporter* const importers[]{&gltfImporter, &fbxImporter};
static_assert(std::size(formats) == std::size(importers));
} // namespace

VectorView<const SceneFormatDescriptor> GetSceneImportFormats()
{
    return VectorView<const SceneFormatDescriptor>(formats);
}

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

const SceneFormatDescriptor* FindSceneImportFormat(const std::string& path)
{
    return FindSceneFormat(path, GetSceneImportFormats());
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

SceneImportResult ImportScene(const SceneImportRequest& request, const SceneImportContext& context)
{
    SceneImportResult            result;
    const SceneFormatDescriptor* format = FindSceneImportFormat(request.path);
    std::error_code              error;
    if (request.control && request.control->IsCancelled())
    {
        SetSceneImportFailure(result, SceneImportStatus::Cancelled, request.path, "Scene import cancelled");
    }
    else if (request.path.empty())
    {
        SetSceneImportFailure(result, SceneImportStatus::InvalidRequest, request.path, "A scene source path is required");
    }
    else if (format == nullptr)
    {
        SetSceneImportFailure(result, SceneImportStatus::UnsupportedFormat, request.path,
                              "Unsupported scene format: " + request.path);
    }
    else if (!std::filesystem::is_regular_file(std::filesystem::u8path(request.path), error))
    {
        SetSceneImportFailure(result, SceneImportStatus::SourceUnavailable, request.path,
                              "Scene source is unavailable: " + request.path);
    }
    else
    {
        if (request.control)
        {
            request.control->SetStage(SceneImportStage::Reading);
        }
        result                 = importers[format - formats]->Import(request, context);
        result.format          = std::string(format->id);
        result.importerVersion = format->importerVersion;
    }
    if (request.control && request.control->IsCancelled() && result.status != SceneImportStatus::Cancelled)
    {
        SetSceneImportFailure(result, SceneImportStatus::Cancelled, request.path, "Scene import cancelled");
    }
    if (request.control)
    {
        request.control->SetStage(result.status == SceneImportStatus::Success     ? SceneImportStage::AssetReady
                                  : result.status == SceneImportStatus::Cancelled ? SceneImportStage::Cancelled
                                                                                  : SceneImportStage::Failed);
    }
    return result;
}
} // namespace zen::asset
