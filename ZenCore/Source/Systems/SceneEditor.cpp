#include "Systems/SceneEditor.h"
#include "SceneGraph/Scene.h"

namespace zen::sys
{
void SceneEditor::CenterAndNormalizeScene(sg::Scene* pScene)
{
    // Edit renderable nodes, center and scale model
    Vec3 center(0.0f);
    Vec3 extents = pScene->GetAABB().GetMax() - pScene->GetAABB().GetMin();
    Vec3 scaleFactors(1.0f);
    scaleFactors.x = glm::abs(extents.x) < glm::epsilon<float>() ? 1.0f : 1.0f / extents.x;
    scaleFactors.y = glm::abs(extents.y) < glm::epsilon<float>() ? 1.0f : 1.0f / extents.y;
    scaleFactors.z = glm::abs(extents.z) < glm::epsilon<float>() ? 1.0f : 1.0f / extents.z;

    auto scaleFactorMax = std::max(scaleFactors.x, std::max(scaleFactors.y, scaleFactors.z));
    Mat4 scaleMat       = glm::scale(Mat4(1.0f), Vec3(scaleFactorMax));
    Mat4 translateMat   = glm::translate(Mat4(1.0f), center - pScene->GetAABB().GetCenter());
    Mat4 transformMat   = scaleMat * translateMat;

    for (auto* pSgNode : pScene->GetRenderableNodes())
    {
        sg::NodeData nodeData = pSgNode->GetData();
        nodeData.modelMatrix  = transformMat * nodeData.modelMatrix;
        nodeData.normalMatrix = glm::transpose(glm::inverse(nodeData.modelMatrix));
        pSgNode->SetData(nodeData);
        for (auto* pSubMesh : pSgNode->GetComponent<sg::Mesh>()->GetSubMeshes()) {
            pSubMesh->GetAABB().Transform(transformMat);
        }
    }

    pScene->GetAABB().Transform(transformMat);
}
} // namespace zen::sys