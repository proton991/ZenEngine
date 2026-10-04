#include "Editor/Model/NodeInspection.h"
#include <limits>

namespace zen::editor
{
NodeInspection InspectNode(const sg::Node& node, NodeId id)
{
    NodeInspection result;

    result.valid        = true;

    result.id           = id;

    result.name         = node.GetName();

    result.visible      = node.IsVisible();

    result.hasTransform = node.HasComponent<sg::Transform>();

    if (result.hasTransform)
    {
        sg::Transform& transform = *node.GetComponent<sg::Transform>();

        result.translation       = transform.GetTranslation();

        result.rotationDegrees   = glm::degrees(glm::eulerAngles(transform.GetRotation()));

        result.scale             = transform.GetScale();

        result.baseMatrix        = transform.GetBaseMatrix();

        result.worldMatrix       = transform.GetWorldMatrix();
    }

    result.hasMesh = node.HasComponent<sg::Mesh>();

    result.skinned = node.skinIndex >= 0;

    if (result.hasMesh)
    {
        const sg::Mesh& mesh = *node.GetComponent<sg::Mesh>();

        result.primitives    = uint32_t(mesh.GetSubMeshes().size());

        result.indices       = mesh.GetNumIndices();

        for (const sg::SubMesh* primitive : mesh.GetSubMeshes())
        {
            const sg::Material* material = primitive->GetMaterial();

            if (material != nullptr)
            {
                result.materials.push_back({material->GetName(), material->index, material->baseColorFactor,
                                            material->metallicFactor, material->roughnessFactor, material->alphaMode});
            }
        }
    }

    result.hasLight = node.HasComponent<sg::Light>();

    if (result.hasLight)
    {
        result.light     = node.GetComponent<sg::Light>()->GetProperties();

        result.lightType = node.GetComponent<sg::Light>()->GetType();
    }

    result.hasCamera = node.HasComponent<sg::SceneCamera>();

    if (result.hasCamera)
    {
        const sg::SceneCamera& camera = *node.GetComponent<sg::SceneCamera>();

        result.orthographic           = camera.orthographic;

        result.fov                    = glm::degrees(camera.verticalFov);

        result.nearPlane              = camera.nearPlane;

        result.farPlane               = camera.infiniteFar ? std::numeric_limits<float>::infinity() : camera.farPlane;
    }

    return result;
}
} // namespace zen::editor
