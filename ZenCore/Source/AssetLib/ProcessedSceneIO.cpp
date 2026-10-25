#include "AssetLib/ProcessedSceneIO.h"
#include "AssetLib/ProcessedSceneAdapter.h"
#include "ZenAsset/Reader/Scene.h"
#include <type_traits>

namespace zen::asset
{
namespace
{
zenasset::StatusCode ProcessedStatus(SceneImportStatus source)
{
    zenasset::StatusCode result = zenasset::StatusCode::InvalidData;
    switch (source)
    {
        case SceneImportStatus::Success: result = zenasset::StatusCode::Success; break;
        case SceneImportStatus::InvalidRequest: result = zenasset::StatusCode::InvalidArgument; break;
        case SceneImportStatus::UnsupportedFormat: result = zenasset::StatusCode::UnsupportedFormat; break;
        case SceneImportStatus::SourceUnavailable: result = zenasset::StatusCode::SourceUnavailable; break;
        case SceneImportStatus::InvalidData: result = zenasset::StatusCode::InvalidData; break;
        case SceneImportStatus::UnsupportedFeature: result = zenasset::StatusCode::UnsupportedFeature; break;
        case SceneImportStatus::Cancelled: result = zenasset::StatusCode::Cancelled; break;
        case SceneImportStatus::ResourceLimit: result = zenasset::StatusCode::ResourceLimit; break;
        case SceneImportStatus::IOError: result = zenasset::StatusCode::IOError; break;
    }
    return result;
}

template <class Target, class Source> Target ConvertSettings(const Source& source)
{
    Target target;
    target.sceneIndex                         = source.sceneIndex;
    target.fallbackMissingNamedUVSets         = source.fallbackMissingNamedUVSets;
    target.animationBake.sampleRate           = source.animationBake.sampleRate;
    target.animationBake.translationTolerance = source.animationBake.translationTolerance;
    target.animationBake.rotationTolerance    = source.animationBake.rotationTolerance;
    target.animationBake.scaleTolerance       = source.animationBake.scaleTolerance;
    target.animationBake.valueTolerance       = source.animationBake.valueTolerance;
    target.animationBake.maxSubdivisions      = source.animationBake.maxSubdivisions;
    target.animationBake.maxValues            = source.animationBake.maxValues;
    return target;
}

zenasset::Diagnostic ConvertDiagnostic(const SceneImportDiagnostic& source)
{
    return {source.severity == SceneDiagnosticSeverity::Warning ? zenasset::DiagnosticSeverity::Warning
                                                                : zenasset::DiagnosticSeverity::Error,
            ProcessedStatus(source.status), source.source, source.object, source.message};
}

SceneImportDiagnostic ConvertDiagnostic(const zenasset::Diagnostic& source)
{
    return AdaptProcessedDiagnostic(source);
}

template <class Target, class Source> Target ConvertMetadata(const Source& source)
{
    Target target;
    target.sourceFormat     = source.sourceFormat;
    target.importerVersion  = source.importerVersion;
    target.processorVersion = source.processorVersion;
    target.buildFingerprint = source.buildFingerprint;
    target.profile          = source.profile;
    target.processing       = source.processing;
    target.settings         = ConvertSettings<decltype(target.settings)>(source.settings);
    target.cacheKey         = source.cacheKey;
    target.inputs.reserve(source.inputs.size());
    for (const typename std::remove_cvref_t<decltype(source.inputs)>::value_type& input : source.inputs)
    {
        target.inputs.push_back({input.path, input.exists, input.size, input.modified, input.contentHash});
    }
    target.diagnostics.reserve(source.diagnostics.size());
    for (const typename std::remove_cvref_t<decltype(source.diagnostics)>::value_type& diagnostic : source.diagnostics)
    {
        target.diagnostics.push_back(ConvertDiagnostic(diagnostic));
    }
    return target;
}

zenasset::SceneReadLimits ProcessedLimits(const SceneAssetReadLimits& source)
{
    zenasset::SceneReadLimits limits;
    limits.package.maxFileBytes = source.maxFileBytes;
    // Directory validation may inspect a large package while only its small
    // metadata resource is decoded; actual allocations use the separate limit.
    limits.package.maxDecodedBytes = source.maxFileBytes;
    limits.package.maxChunkBytes   = source.maxFileBytes;
    limits.package.maxResources    = source.maxArrayElements;
    limits.maxAllocationBytes      = source.maxAllocationBytes;
    limits.maxArrayElements        = source.maxArrayElements;
    limits.maxStringBytes          = source.maxStringBytes;
    limits.maxSourceDocumentBytes  = source.maxStringBytes;
    limits.maxRuntimeGeometryBytes = source.maxRuntimeGeometryBytes;
    return limits;
}
} // namespace

SceneImportStatus AdaptProcessedStatus(zenasset::StatusCode status)
{
    SceneImportStatus result = SceneImportStatus::InvalidData;
    switch (status)
    {
        case zenasset::StatusCode::Success: result = SceneImportStatus::Success; break;
        case zenasset::StatusCode::InvalidArgument: result = SceneImportStatus::InvalidRequest; break;
        case zenasset::StatusCode::SourceUnavailable: result = SceneImportStatus::SourceUnavailable; break;
        case zenasset::StatusCode::InvalidData: result = SceneImportStatus::InvalidData; break;
        case zenasset::StatusCode::UnsupportedVersion:
        case zenasset::StatusCode::UnsupportedCodec:
        case zenasset::StatusCode::UnsupportedFeature: result = SceneImportStatus::UnsupportedFeature; break;
        case zenasset::StatusCode::UnsupportedFormat: result = SceneImportStatus::UnsupportedFormat; break;
        case zenasset::StatusCode::Cancelled: result = SceneImportStatus::Cancelled; break;
        case zenasset::StatusCode::ResourceLimit: result = SceneImportStatus::ResourceLimit; break;
        case zenasset::StatusCode::IOError: result = SceneImportStatus::IOError; break;
        default: break;
    }
    return result;
}

bool ProcessedOperationCancelled(void* control)
{
    return control != nullptr && static_cast<SceneImportControl*>(control)->IsCancelled();
}

SceneImportDiagnostic AdaptProcessedDiagnostic(const zenasset::Diagnostic& source)
{
    return {source.severity == zenasset::DiagnosticSeverity::Warning ? SceneDiagnosticSeverity::Warning
                                                                     : SceneDiagnosticSeverity::Error,
            AdaptProcessedStatus(source.status), source.source, source.object, source.message};
}

zenasset::SourceSettings MakeProcessedSettings(const SceneImportSettings& source)
{
    return ConvertSettings<zenasset::SourceSettings>(source);
}

zenasset::BuildMetadata MakeProcessedMetadata(const SceneAssetFileMetadata& source)
{
    return ConvertMetadata<zenasset::BuildMetadata>(source);
}

SceneAssetFileMetadata AdaptProcessedMetadata(const zenasset::BuildMetadata& source)
{
    return ConvertMetadata<SceneAssetFileMetadata>(source);
}

SceneAssetFileResult ReadProcessedSceneFile(const std::string&          path,
                                            const SceneAssetReadLimits& limits,
                                            SceneImportControl*         control,
                                            bool                        metadataOnly)
{
    SceneAssetFileResult            result;
    const zenasset::Cancellation    cancellation(&ProcessedOperationCancelled, control);
    const zenasset::SceneReadLimits readLimits = ProcessedLimits(limits);
    if (control != nullptr)
    {
        control->SetStage(SceneImportStage::Reading);
    }
    zenasset::Status status;
    if (metadataOnly)
    {
        const zenasset::SceneMetadataResult metadata = zenasset::ReadSceneMetadata(path, readLimits, &cancellation);
        status                                       = metadata.status;
        result.metadata                              = AdaptProcessedMetadata(metadata.metadata);
        result.status                                = AdaptProcessedStatus(status.code);
    }
    else
    {
        const zenasset::SceneReadResult loaded = zenasset::ReadScene(path, readLimits, &cancellation);
        status                                 = loaded.status;
        if (status.Succeeded() && loaded.scene)
        {
            static_cast<SceneImportResult&>(result) =
                AdaptProcessedScene(*loaded.scene, control, limits.maxRuntimeGeometryBytes);
            result.metadata = AdaptProcessedMetadata(loaded.metadata);
        }
        else if (status.Succeeded())
        {
            status = {zenasset::StatusCode::InvalidData, "Package reader returned success without a scene"};
        }
    }
    if (!status.Succeeded())
    {
        SetSceneImportFailure(result, AdaptProcessedStatus(status.code), path, status.message);
    }
    if (result.status == SceneImportStatus::Success)
    {
        result.diagnostics = result.metadata.diagnostics;
    }
    result.format          = "zenasset";
    result.importerVersion = zenasset::PackageSchemaVersion;
    return result;
}
} // namespace zen::asset
