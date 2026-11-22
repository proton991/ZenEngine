#pragma once
#include "AssetTools/SceneTools.h"
#include "AssetLib/SceneLoader.h"

namespace zen::test
{
// Tests keep their familiar scene/geometry fixture access while exercising the
// public dispatcher and runtime builder, with no parser dependency.
class SceneLoader
{
public:
    bool LoadFromFile(const std::string& path, sg::Scene* scene)
    {
        asset::SceneImportRequest request{path};
        request.useCache              = false;
        asset::SceneLoadResult result = asset::LoadSourceScene(request);
        const bool             loaded = bool(result.scene);
        m_error                       = std::move(result.error);
        if (loaded)
        {
            *scene   = std::move(*result.scene->scene);
            m_loaded = std::move(result.scene);
        }
        return loaded;
    }

    const HeapVector<asset::Vertex>& GetVertices() const
    {
        return m_loaded->vertices;
    }

    const HeapVector<uint32_t>& GetIndices() const
    {
        return m_loaded->indices;
    }

    const std::string& GetError() const
    {
        return m_error;
    }

private:
    UniquePtr<asset::LoadedScene> m_loaded{MakeUnique<asset::LoadedScene>()};
    std::string                   m_error;
};
} // namespace zen::test
