#include "Systems/SceneEditor.h"
#include "SceneGraph/Scene.h"

namespace zen::sys
{
void SceneEditor::CenterAndNormalizeScene(sg::Scene* scene)
{
    // Edit renderable nodes, center and scale model
    Vec3 center(0.0f);
    Vec3 extents = scene->GetAABB().GetMax() - scene->GetAABB().GetMin();
    Vec3 scaleFactors(1.0f);
    scaleFactors.x = glm::abs(extents.x) < glm::epsilon<float>() ? 1.0f : 1.0f / extents.x;
    scaleFactors.y = glm::abs(extents.y) < glm::epsilon<float>() ? 1.0f : 1.0f / extents.y;
    scaleFactors.z = glm::abs(extents.z) < glm::epsilon<float>() ? 1.0f : 1.0f / extents.z;

    auto scaleFactorMax = std::max(scaleFactors.x, std::max(scaleFactors.y, scaleFactors.z));
    Mat4 scaleMat       = glm::scale(Mat4(1.0f), Vec3(scaleFactorMax));
    Mat4 translateMat   = glm::translate(Mat4(1.0f), center - scene->GetAABB().GetCenter());
    Mat4 transformMat   = scaleMat * translateMat;

    for (auto* sgNode : scene->GetRenderableNodes())
    {
        sg::NodeData nodeData = sgNode->GetData();
        nodeData.modelMatrix  = transformMat * nodeData.modelMatrix;
        nodeData.normalMatrix = glm::transpose(glm::inverse(nodeData.modelMatrix));
        sgNode->SetData(nodeData);
        for (auto* subMesh : sgNode->GetComponent<sg::Mesh>()->GetSubMeshes()) {
            subMesh->GetAABB().Transform(transformMat);
        }
    }

    scene->GetAABB().Transform(transformMat);
}
} // namespace zen::sys