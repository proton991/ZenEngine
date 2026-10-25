#include "AssetTools/SceneTools.h"
#include "AssetLib/ProcessedSceneAdapter.h"
#include "AssetLib/ProcessedSceneIO.h"
#include "ProcessedSourceImport.h"
#include "ZenAsset/Processor/Import.h"
#include "ZenAsset/Processor/Cook.h"
#include <memory>

namespace zen::asset
{
namespace
{
void ImportStageChanged(void* user, zenasset::SceneImportStage stage)
{
    SceneImportStage engineStage = SceneImportStage::Reading;
    switch (stage)
    {
        case zenasset::SceneImportStage::Queued: engineStage = SceneImportStage::Queued; break;
        case zenasset::SceneImportStage::Reading: engineStage = SceneImportStage::Reading; break;
        case zenasset::SceneImportStage::DecodingTextures: engineStage = SceneImportStage::DecodingTextures; break;
        case zenasset::SceneImportStage::ProcessingMeshes: engineStage = SceneImportStage::ProcessingMeshes; break;
        case zenasset::SceneImportStage::BakingAnimations: engineStage = SceneImportStage::BakingAnimations; break;
        case zenasset::SceneImportStage::BuildingScene: engineStage = SceneImportStage::BuildingScene; break;
        case zenasset::SceneImportStage::AssetReady: engineStage = SceneImportStage::AssetReady; break;
        case zenasset::SceneImportStage::Complete: engineStage = SceneImportStage::Complete; break;
        case zenasset::SceneImportStage::Failed: engineStage = SceneImportStage::Failed; break;
        case zenasset::SceneImportStage::Cancelled: engineStage = SceneImportStage::Cancelled; break;
    }
    static_cast<SceneImportControl*>(user)->SetStage(engineStage);
}

void ImportFileProgressChanged(void* user, zenasset::SceneImportFileProgress progress)
{
    static_cast<SceneImportControl*>(user)->SetFileProgress(
        {progress.processedFiles, progress.totalFiles, progress.totalKnown});
}

HeapVector<SceneFormatDescriptor> ImportFormats()
{
    HeapVector<SceneFormatDescriptor> formats;
    for (const zenasset::SceneFormatDescriptor& format : zenasset::GetSceneImportFormats())
    {
        formats.push_back({format.id, format.displayName,
                           VectorView<const std::string_view>(format.extensions.data(), format.extensions.size()),
                           format.importerVersion});
    }
    return formats;
}

} // namespace

VectorView<const SceneFormatDescriptor> GetSceneImportFormats()
{
    static const HeapVector<SceneFormatDescriptor> formats = ImportFormats();
    return VectorView<const SceneFormatDescriptor>(formats.data(), formats.size());
}

const SceneFormatDescriptor* FindSceneImportFormat(const std::string& path)
{
    return FindSceneFormat(path, GetSceneImportFormats());
}

zenasset::Status ValidateSourceProcessing(const SceneImportRequest& request)
{
    zenasset::Status status = zenasset::ValidateProcessingSettings(request.processing);
    if (status.Succeeded() && request.profile != "reference" && request.profile != "desktop")
    {
        status = {zenasset::StatusCode::InvalidArgument, "Unknown scene processing profile: " + request.profile};
    }
    if (status.Succeeded() && request.profile == "reference" && zenasset::HasProcessingOperations(request.processing))
    {
        status = {zenasset::StatusCode::InvalidArgument, "Reference profile does not permit geometry or texture processing"};
    }
    if (status.Succeeded() && request.forceRecook && !request.useCache)
    {
        status = {zenasset::StatusCode::InvalidArgument, "Recooking requires cache publication to be enabled"};
    }
    return status;
}

ProcessedSourceResult ImportProcessedSource(const SceneImportRequest& request, const SceneImportContext& context)
{
    ProcessedSourceResult        output;
    SceneImportResult&           result     = output.imported;
    const zenasset::Status       validation = ValidateSourceProcessing(request);
    zenasset::SceneImportRequest sourceRequest;
    sourceRequest.path     = request.path;
    sourceRequest.settings = MakeProcessedSettings(request.settings);
    if (request.control)
    {
        sourceRequest.control = std::make_shared<zenasset::SceneImportControl>(zenasset::SceneImportCallbacks{
            request.control.Get(), &ProcessedOperationCancelled, &ImportStageChanged, &ImportFileProgressChanged});
    }
    zenasset::SceneImportContext sourceContext;
    if (context.processing != nullptr)
    {
        sourceContext.workerCount = static_cast<uint32_t>(context.processing->GetSize());
    }
    zenasset::SceneImportResult imported;
    zenasset::SceneCookResult   cooked;
    if (validation.Succeeded())
    {
        imported = zenasset::ImportScene(sourceRequest, sourceContext);
        if (imported.status == zenasset::StatusCode::Success && imported.asset)
        {
            if (request.control)
            {
                request.control->SetStage(SceneImportStage::CookingAssets);
            }
            zenasset::SceneCookOptions options;
            options.settings = request.processing;
            const zenasset::Cancellation cancellation(&ProcessedOperationCancelled, request.control.Get());
            cooked = zenasset::CookScene(imported.asset, options, &cancellation);
            if (cooked.status.Succeeded() && cooked.scene)
            {
                result = AdaptProcessedScene(*cooked.scene, request.control.Get());
            }
            else
            {
                SetSceneImportFailure(
                    result,
                    cooked.status.Succeeded() ? SceneImportStatus::InvalidData : AdaptProcessedStatus(cooked.status.code),
                    request.path,
                    cooked.status.Succeeded() ? "Processor returned success without a cooked scene" : cooked.status.message);
            }
        }
        else
        {
            result.status = AdaptProcessedStatus(imported.status);
            result.error  = imported.error;
            if (result.status == SceneImportStatus::Success)
            {
                SetSceneImportFailure(result, SceneImportStatus::InvalidData, request.path,
                                      "Processor returned success without a scene");
            }
        }
    }
    else
    {
        SetSceneImportFailure(result, AdaptProcessedStatus(validation.code), request.path, validation.message);
    }
    result.format          = imported.format;
    result.importerVersion = imported.importerVersion;
    for (const zenasset::SceneImportDiagnostic& diagnostic : imported.diagnostics)
    {
        result.diagnostics.push_back(AdaptProcessedDiagnostic(diagnostic));
    }
    for (const zenasset::Diagnostic& diagnostic : cooked.diagnostics)
    {
        result.diagnostics.push_back(AdaptProcessedDiagnostic(diagnostic));
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
    if (result.asset)
    {
        output.processed = std::move(cooked.scene);
    }
    return output;
}

SceneImportResult ImportScene(const SceneImportRequest& request, const SceneImportContext& context)
{
    return std::move(ImportProcessedSource(request, context).imported);
}
} // namespace zen::asset
