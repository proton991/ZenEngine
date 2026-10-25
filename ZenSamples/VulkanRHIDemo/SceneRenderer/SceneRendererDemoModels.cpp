#include "AssetTools/SceneTools.h"
#include "SceneRendererDemo.h"
#include "AssetLib/SceneModelCatalog.h"
#include "Platform/ConfigLoader.h"
#include <filesystem>
#include <chrono>

namespace zen
{
const ui::RuntimeModelState& SceneRendererDemo::GetRuntimeModelState() const
{
    return m_modelState;
}

void SceneRendererDemo::RefreshRuntimeModels()
{
    if (m_modelState.pendingPath.empty())
    {
        m_modelState.basePath = platform::ConfigLoader::GetInstance().GetModelBasePath();

        m_modelState.models   = asset::DiscoverSourceSceneModels(m_modelState.basePath);

        m_modelState.error.clear();

        std::error_code error;

        const bool directory = !m_modelState.basePath.empty()
                            && std::filesystem::is_directory(std::filesystem::u8path(m_modelState.basePath), error);

        if (!directory)
        {
            m_modelState.error = m_modelState.basePath.empty() ? "No model directory is configured."
                                                               : "The configured model directory is unavailable.";
        }
    }
}

bool SceneRendererDemo::RequestRuntimeModel(const std::string& path)
{
    bool accepted = false;

    if (m_modelState.pendingPath.empty())
    {
        for (const asset::SceneModelCatalogEntry& model : m_modelState.models)
        {
            if (model.path == path)
            {
                accepted = true;
            }
        }

        if (accepted)
        {
            m_modelState.error.clear();

            if (path != m_modelState.currentPath)
            {
                m_modelState.cancellationRequested = false;
                m_modelLoadControl.Reset(new asset::SceneImportControl);
                m_modelState.pendingPath = path;
            }
        }
        else
        {
            m_modelState.error = "The selected model is no longer in the catalog. Refresh the list.";
        }
    }

    return accepted;
}

bool SceneRendererDemo::CancelRuntimeModel()
{
    const bool accepted = !m_modelState.pendingPath.empty();
    if (accepted)
    {
        m_modelState.cancellationRequested = true;
        if (m_modelLoadControl)
        {
            m_modelLoadControl->Cancel();
        }
    }
    return accepted;
}

void SceneRendererDemo::ProcessPendingModel()
{
    if (!m_modelState.pendingPath.empty())
    {
        if (!m_modelLoadResult.valid())
        {
            if (m_modelState.cancellationRequested)
            {
                m_modelState.pendingPath.clear();
                m_modelLoadControl.Reset();
            }
            else
            {
                if (!m_sceneLoader)
                {
                    m_sceneLoader = MakeUnique<asset::SourceSceneLoader>(1, 0);
                }
                asset::SceneImportRequest request{m_modelState.pendingPath};
                request.useCache  = m_useAssetCache;
                request.control   = m_modelLoadControl;
                m_modelLoadResult = m_sceneLoader->LoadAsync(std::move(request));
            }
        }
        else if (m_modelLoadResult.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        {
            asset::SceneLoadResult result = m_modelLoadResult.get();
            const bool cancelled = m_modelState.cancellationRequested || result.status == asset::SceneImportStatus::Cancelled;
            m_modelState.error   = cancelled ? std::string() : std::move(result.error);
            const bool loaded    = !cancelled && result.scene && PublishModel(std::move(result.scene), false);
            if (loaded)
            {
                m_modelState.diagnostics = std::move(result.diagnostics);
                m_modelState.currentPath = m_modelState.pendingPath;
                m_modelState.error.clear();
                ++m_modelState.revision;
            }
            else if (!cancelled && m_modelState.error.empty())
            {
                m_modelState.error = "The model could not be loaded. The previous model is still active.";
            }
            m_modelState.pendingPath.clear();
            m_modelLoadControl.Reset();
            // GPU publication time does not advance animation or the camera.
            m_timer->Tick();
        }
    }
}
} // namespace zen
