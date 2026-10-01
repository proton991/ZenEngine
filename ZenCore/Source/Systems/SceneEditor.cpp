#include "Systems/SceneEditor.h"
#include "SceneGraph/Scene.h"
#include <cmath>

namespace zen::sys
{
void SceneEditor::CenterAndNormalizeScene(sg::Scene* pScene)
{
    const float extent = pScene->GetAABB().GetMaxExtent();

    const Vec3 center = pScene->GetAABB().GetCenter();

    if (std::isfinite(extent) && extent > 1e-6f && std::isfinite(center.x) &&
        std::isfinite(center.y) && std::isfinite(center.z))
    {
        const float scale = 1.0f / extent;

        pScene->GetAssetData().unitScale *= scale;

        const Mat4 normalization =
            glm::scale(Mat4(1.0f), Vec3(scale)) * glm::translate(Mat4(1.0f), -center);

        // Apply once at every root so hierarchy queries and renderer matrices agree.
        for (const UniquePtr<sg::Node>& node : pScene->GetNodes())
        {
            if (node->GetParent() == nullptr && node->HasComponent<sg::Transform>())
            {
                sg::Transform* transform = node->GetComponent<sg::Transform>();

                transform->SetPrefixMatrix(normalization * transform->GetPrefixMatrix());
            }
        }

        for (const UniquePtr<sg::Node>& node : pScene->GetNodes())
        {
            sg::NodeData data = node->GetData();

            data.modelMatrix = normalization * data.modelMatrix;

            node->SetData(node->GetRenderableIndex(), data.modelMatrix);
        }

        // Manually constructed scenes may only register renderable nodes.
        if (pScene->GetNodes().empty())
        {
            for (sg::Node* node : pScene->GetRenderableNodes())
            {
                if (node->HasComponent<sg::Transform>())
                {
                    sg::Transform* transform = node->GetComponent<sg::Transform>();

                    transform->SetPrefixMatrix(normalization * transform->GetPrefixMatrix());
                }

                sg::NodeData data = node->GetData();

                data.modelMatrix = normalization * data.modelMatrix;

                node->SetData(node->GetRenderableIndex(), data.modelMatrix);
            }
        }

        for (sg::Light* light : pScene->GetComponents<sg::Light>())
        {
            light->unitScale *= scale;
            sg::LightProperties properties = light->GetProperties();

            properties.position = Vec3(normalization * Vec4(properties.position, 1.0f));

            properties.range *= scale;

            // Preserve inverse-square illumination when changing the engine's unit scale.
            if (light->GetType() != sg::Directional)
            {
                properties.intensity *= scale * scale;
            }
            light->SetProperties(properties);
        }

        for (sg::SceneCamera* camera : pScene->GetComponents<sg::SceneCamera>())
        {
            camera->unitScale *= scale;
            camera->worldMatrix = normalization * camera->worldMatrix;

            camera->nearPlane *= scale;

            camera->farPlane *= scale;

            camera->xmag *= scale;

            camera->ymag *= scale;
        }

        pScene->GetAABB().Transform(normalization);
    }
}
} // namespace zen::sys
