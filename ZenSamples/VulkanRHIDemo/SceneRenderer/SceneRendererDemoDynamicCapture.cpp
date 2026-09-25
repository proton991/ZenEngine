#include "SceneRendererDemo.h"
#include "Graphics/RenderCore/V2/Renderer/RendererServer.h"
#include "Graphics/RenderCore/V2/Renderer/DynamicVoxelGIRenderer.h"
#include "SceneGraph/Mesh.h"
#include <limits>

namespace zen
{
bool SceneRendererDemo::GetGIMotionFixture(uint32_t& moving, Mat4& original) const
{
    // Fixed diagnostic fixture produced by validate_dynamic_voxel_m4.py: four
    // independent six-vertex meshes, with the moving receiver last. Reject other assets.
    const HeapVector<asset::Vertex>& vertices = m_renderScene->GetVertices();
    bool valid = m_scene->GetRenderableNodes().size() == 4 && vertices.size() == 24;
    if (valid)
    {
        const sg::Node* node                   = m_scene->GetRenderableNodes()[3];
        const HeapVector<sg::SubMesh*>& meshes = node->GetComponent<sg::Mesh>()->GetSubMeshes();
        valid = meshes.size() == 1 && meshes[0]->GetFirstIndex() == 18 &&
            meshes[0]->GetIndexCount() == 6;
        moving   = node->GetRenderableIndex();
        original = node->GetData().modelMatrix;
    }
    return valid;
}

bool SceneRendererDemo::CaptureDynamicGILifecycle(const std::string& path, bool switchMethods)
{
    uint32_t moving = 0;
    Mat4 original(1);
    bool valid = GetGIMotionFixture(moving, original) &&
        m_renderDevice->GetRendererServer()->RequestDynamicVoxelGI() != nullptr;
    HeapVector<asset::Vertex> originalVertices;
    if (valid)
    {
        const HeapVector<asset::Vertex>& vertices = m_renderScene->GetVertices();
        for (uint32_t i = 18; i < 24; ++i)
        {
            originalVertices.push_back(vertices[i]);
        }
    }
    const Vec3 camera    = m_camera->GetPos();
    const uint32_t width = m_pViewport->GetWidth(), height = m_pViewport->GetHeight();
    valid                = valid && m_renderScene->SetInstanceClass(moving, GI_DYNAMIC);
    const char* stages[] = {"initial",  "moved",  "deformed", "teleported", "removed",
                            "restored", "camera", "resized",  "returned"};
    for (uint32_t step = 0; valid && step < 9; ++step)
    {
        if (step == 1 || step == 3)
        {
            if (step == 3)
            {
                m_renderScene->InvalidateGIHistory();
            }
            const Vec3 translation = step == 1 ? Vec3(-.04f, .01f, .02f) : Vec3(-.3f, .08f, .04f);
            valid                  = m_renderScene->SetInstanceTransform(
                moving, glm::translate(Mat4(1), translation) * original);
        }
        else if (step == 2)
        {
            HeapVector<asset::Vertex> deformed = originalVertices;
            for (asset::Vertex& vertex : deformed)
            {
                vertex.pos.z += .4f * vertex.pos.x * vertex.pos.y;
            }
            valid = m_renderScene->UpdateVertices(18, {deformed.data(), deformed.size()});
        }
        else if (step == 4)
        {
            valid = m_renderScene->SetInstanceEnabled(moving, false);
        }
        else if (step == 5)
        {
            valid = m_renderScene->SetInstanceEnabled(moving, true) &&
                m_renderScene->SetInstanceTransform(moving, original) &&
                m_renderScene->UpdateVertices(18,
                                              {originalVertices.data(), originalVertices.size()});
        }
        else if (step == 6)
        {
            m_camera->SetPosition(Vec3(.25f, .1f, 1.4f));
        }
        else if (step == 7)
        {
            glfwSetWindowSize(m_pWindow->GetHandle(), 515, 321);
        }
        else if (step == 8)
        {
            m_camera->SetPosition(camera);
            glfwSetWindowSize(m_pWindow->GetHandle(), width, height);
        }
        if (switchMethods)
        {
            const rc::VoxelGIMethod method = step == 6 ?
                rc::VoxelGIMethod::eAuto :
                (step == 1 || step == 2 || step == 4 ? rc::VoxelGIMethod::eCone :
                                                       rc::VoxelGIMethod::eDynamicVoxel);
            valid = valid && m_renderDevice->GetRendererServer()->SetVoxelGIMethod(method);
        }
        valid = valid && Run(step == 0 ? 12 : 2, false, 3) &&
            CaptureLighting(path + "." + stages[step]);
    }
    if (!valid)
    {
        LOGE("Dynamic GI lifecycle fixture failed: {}", path);
    }
    return valid;
}

bool SceneRendererDemo::CaptureGIContracts(const std::string& path)
{
    rc::RendererServer* server = m_renderDevice->GetRendererServer();
    bool valid                 = m_scene->GetRenderableNodes().size() == 4 &&
        m_renderScene->GetVertices().size() == 24 && server->RequestDynamicVoxelGI() != nullptr;
    uint32_t moving = 0, senderMaterial = 0, movingMaterial = 0;
    Mat4 original(1);
    sg::MaterialData sender, receiver;
    const sg::AABB bounds = m_renderScene->GetVoxelSceneBounds();
    if (valid)
    {
        const sg::Node* senderNode = m_scene->GetRenderableNodes()[2];
        const sg::Node* movingNode = m_scene->GetRenderableNodes()[3];
        moving                     = movingNode->GetRenderableIndex();
        original                   = movingNode->GetData().modelMatrix;
        senderMaterial =
            senderNode->GetComponent<sg::Mesh>()->GetSubMeshes()[0]->GetMaterial()->index;
        movingMaterial =
            movingNode->GetComponent<sg::Mesh>()->GetSubMeshes()[0]->GetMaterial()->index;
        sender                   = m_renderScene->GetMaterialsData()[senderMaterial];
        receiver                 = m_renderScene->GetMaterialsData()[movingMaterial];
        sg::MaterialData invalid = sender;
        invalid.emissiveFactor.x = std::numeric_limits<float>::quiet_NaN();
        valid                    = !m_renderScene->UpdateMaterial(UINT32_MAX, sender) &&
            !m_renderScene->UpdateMaterial(senderMaterial, invalid) &&
            !m_renderScene->SetVoxelBounds(sg::AABB(Vec3(1), Vec3(-1)));
        invalid            = sender;
        invalid.bcTexIndex = INT32_MAX;
        valid              = valid && !m_renderScene->UpdateMaterial(senderMaterial, invalid) &&
            m_renderScene->SetInstanceClass(moving, GI_DYNAMIC);
    }
    const char* stages[]    = {"initial",          "tint",          "metal",
                               "emission",         "alpha",         "restored",
                               "dynamic-emission", "dynamic-alpha", "materials-restored",
                               "outside",          "returned",      "outside-again",
                               "expanded",         "grid-restored"};
    uint64_t initialBatches = 0, initialVisibility = 0, returnedBatches = 0;
    for (uint32_t step = 0; valid && step < 14; ++step)
    {
        if (step >= 1 && step <= 5)
        {
            sg::MaterialData changed = sender;
            if (step == 1)
            {
                changed.baseColorFactor = Vec4(0.1f, 0.8f, 0.2f, 1);
            }
            else if (step == 2)
            {
                changed.metallicFactor = 1;
            }
            else if (step == 3)
            {
                changed.emissiveFactor = Vec4(4, 1, 0.25f, 0);
            }
            else if (step == 4)
            {
                changed.baseColorFactor.a   = 0;
                changed.surfaceProperties.y = 1;
                changed.surfaceProperties.x = 0.5f;
            }
            valid = m_renderScene->UpdateMaterial(senderMaterial, changed);
        }
        else if (step >= 6 && step <= 8)
        {
            sg::MaterialData changed = receiver;
            if (step == 6)
            {
                changed.emissiveFactor = Vec4(0, 3, 0, 0);
            }
            else if (step == 7)
            {
                changed.baseColorFactor.a   = 0;
                changed.surfaceProperties.y = 1;
                changed.surfaceProperties.x = 0.5f;
            }
            valid = m_renderScene->UpdateMaterial(movingMaterial, changed);
        }
        else if (step == 9 || step == 11)
        {
            valid = m_renderScene->SetInstanceTransform(
                moving, glm::translate(Mat4(1), Vec3(2, 0, 0)) * original);
        }
        else if (step == 10 || step == 13)
        {
            valid = m_renderScene->SetInstanceTransform(moving, original);
            if (step == 13)
            {
                valid = valid && m_renderScene->SetVoxelBounds(bounds);
            }
        }
        else if (step == 12)
        {
            valid = m_renderScene->SetVoxelBounds(m_renderScene->GetAABB());
        }
        valid =
            valid && Run(1, false, static_cast<uint32_t>(server->GetRequestedRenderOption()) + 1);
        if (valid && step >= 1 && step <= 8)
        {
            valid = server->RequestDynamicVoxelGI()->GetFilterUniform().control.z == 1;
        }
        valid =
            valid && Run(11, false, static_cast<uint32_t>(server->GetRequestedRenderOption()) + 1);
        const bool outside = step == 9 || step == 11;
        if (valid)
        {
            valid = server->GetRenderOption() ==
                    (outside ? rc::RenderOption::ePBR : rc::RenderOption::eVoxelGI) &&
                (step == 12 || m_renderScene->GetVoxelSceneBounds() == bounds);
            if (outside)
            {
                valid =
                    valid && server->GetClassVisibility().GetInfo().grid.dimensions.y == GI_STATIC;
            }
            const uint64_t batches = server->RequestDynamicVoxelGI()->GetCacheBuildBatches();
            const uint64_t visibility =
                server->RequestVoxelizer(GI_STATIC)->GetRecordedVisibilityRevision();
            if (step == 0)
            {
                initialBatches    = batches;
                initialVisibility = visibility;
            }
            if (step >= 1 && step <= 3)
            {
                valid = valid && batches == initialBatches && visibility == initialVisibility;
            }
            if (step == 8)
            {
                returnedBatches = batches;
            }
            if (step == 10)
            {
                valid = valid && batches == returnedBatches;
            }
        }
        valid = valid && CaptureLighting(path + "." + stages[step]);
        if (valid && step <= 8)
        {
            valid = server->SetVoxelGIMethod(rc::VoxelGIMethod::eCone) && Run(1, false, 3) &&
                CaptureLighting(path + "." + stages[step] + ".cone") &&
                server->SetVoxelGIMethod(rc::VoxelGIMethod::eDynamicVoxel) && Run(1, false, 3);
        }
        if (!valid)
        {
            LOGE("GI contract lifecycle failed at {}", stages[step]);
        }
    }
    return valid;
}
} // namespace zen
