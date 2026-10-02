#include "SceneRendererDemo.h"
#include "AssetLib/GLTFModelCatalog.h"
#include "Platform/ConfigLoader.h"
#include <filesystem>

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
        m_modelState.basePath = platform::ConfigLoader::GetInstance().GetGLTFModelBasePath();

        m_modelState.models   = asset::DiscoverGLTFModels(m_modelState.basePath);

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
        for (const asset::GLTFModelCatalogEntry& model : m_modelState.models)
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

void SceneRendererDemo::ProcessPendingModel()
{
    if (!m_modelState.pendingPath.empty())
    {
        const std::string path = m_modelState.pendingPath;

        const bool loaded      = LoadModel(path, false);

        m_modelState.pendingPath.clear();

        if (loaded)
        {
            m_modelState.currentPath = path;

            m_modelState.error.clear();

            ++m_modelState.revision;
        }
        else if (m_modelState.error.empty())
        {
            m_modelState.error = "The model could not be loaded. The previous model is still active.";
        }

        // Loading time does not advance the new scene's animation or camera.
        m_timer->Tick();
    }
}
} // namespace zen
