#include <gtest/gtest.h>
#include "SceneGraph/Scene.h"
#include "SceneGraph/SceneAnimation.h"
#include "Systems/SceneEditor.h"

using namespace zen;

TEST(ScenePhysicalUnits, NormalizedSkinningRetainsAuthoredSurfaceScaleAndMaterialDistances)
{
    sg::Scene scene;

    UniquePtr<sg::Node> skinned      = MakeUnique<sg::Node>(0, "Scaled volume skin");

    UniquePtr<sg::Transform> surface = MakeUnique<sg::Transform>(*skinned);

    surface->SetScale(Vec3(-2, 3, 4));

    skinned->AddComponent(surface.Get());

    skinned->SetData(0, surface->GetWorldMatrix());

    skinned->skinIndex = 0;

    scene.AddComponent(std::move(surface));

    UniquePtr<sg::Mesh> mesh = MakeUnique<sg::Mesh>("Scaled volume triangle");

    mesh->SetAABB(Vec3(0), Vec3(4, 2, 0));

    skinned->AddComponent(mesh.Get());

    scene.AddComponent(std::move(mesh));

    scene.AddRenderableNode(skinned.Get());

    UniquePtr<sg::Node> joint               = MakeUnique<sg::Node>(1, "Skin joint");

    UniquePtr<sg::Transform> jointTransform = MakeUnique<sg::Transform>(*joint);

    jointTransform->SetTranslation(Vec3(0, 2, 0));

    joint->AddComponent(jointTransform.Get());

    joint->SetData(1, jointTransform->GetWorldMatrix());

    scene.AddComponent(std::move(jointTransform));

    zen::HeapVector<UniquePtr<sg::Node>> nodes;

    nodes.push_back(std::move(skinned));

    nodes.push_back(std::move(joint));

    scene.SetNodes(std::move(nodes));

    sg::SkinAsset skin;

    skin.joints              = {1};

    skin.inverseBindMatrices = {Mat4(1)};

    scene.GetAssetData().skins.push_back(std::move(skin));

    sg::DeformationPrimitiveAsset deformation;

    deformation.node        = 0;

    deformation.vertexCount = 3;

    for (uint32_t vertex = 0; vertex < 3; ++vertex)
    {
        deformation.influences.push_back(HeapVector<sg::VertexJointInfluence>({{0, 1.0f}}));
    }

    scene.GetAssetData().deformations.push_back(std::move(deformation));

    HeapVector<asset::Vertex> bind(3);

    for (asset::Vertex& vertex : bind)
    {
        vertex.pos     = Vec4(0, 0, 0, 1);

        vertex.normal  = Vec4(0, 0, 1, 0);

        vertex.tangent = Vec4(1, 0, 0, 1);

        vertex.color   = Vec4(1);
    }

    bind[1].pos.x = 4;

    bind[2].pos.y = 2;

    scene.LoadDefaultTextures(0);

    UniquePtr<sg::Material> material          = MakeUnique<sg::Material>("Authored volume distances");

    const sg::Scene::DefaultTextures defaults = scene.GetDefaultTextures();

    material->m_pBaseColorTexture             = defaults.pBaseColor;

    material->m_pMetallicRoughnessTexture     = defaults.pMetallicRoughness;

    material->m_pNormalTexture                = defaults.pNormal;

    material->m_pOcclusionTexture             = defaults.pOcclusion;

    material->m_pEmissiveTexture              = defaults.pEmissive;

    material->features.thickness              = 0.9f;

    material->features.attenuationDistance    = 10;

    material->SetData();

    sg::Material* authored = material.Get();

    scene.AddComponent(std::move(material));

    scene.UpdateAABB();

    ASSERT_FLOAT_EQ(scene.GetAABB().GetMaxExtent(), 8.0f);

    sys::SceneEditor::CenterAndNormalizeScene(&scene);

    EXPECT_FLOAT_EQ(scene.GetAssetData().unitScale, 0.125f);

    EXPECT_EQ(scene.GetNodes()[0]->GetData().surfaceScale, Vec4(0.25f, 0.375f, 0.5f, -1));

    HeapVector<asset::Vertex> posed;

    ASSERT_TRUE(sg::ApplySceneDeformations(scene, MakeVecView(bind), posed));

    EXPECT_TRUE(scene.GetNodes()[0]->deformationInWorldSpace);

    EXPECT_EQ(scene.GetNodes()[0]->GetData().modelMatrix, Mat4(1));

    EXPECT_EQ(scene.GetNodes()[0]->GetData().normalMatrix, Mat4(1));

    EXPECT_EQ(scene.GetNodes()[0]->GetData().surfaceScale, Vec4(0.25f, 0.375f, 0.5f, 1));

    EXPECT_FLOAT_EQ(authored->features.attenuationDistance, 10.0f);

    EXPECT_FLOAT_EQ(authored->data.volumeIridescence.y, 10.0f);

    EXPECT_FLOAT_EQ(authored->features.thickness, 0.9f);

    // The mesh transform is ignored by skinning: the posed triangle still faces +Z.
    const Vec3 face = glm::cross(Vec3(posed[1].pos - posed[0].pos), Vec3(posed[2].pos - posed[0].pos));

    EXPECT_GT(face.z, 0.0f);

    EXPECT_GT(glm::dot(face, Vec3(posed[0].normal)), 0.0f);

    const HeapVector<asset::Vertex> firstPose = posed;

    ASSERT_TRUE(sg::ApplySceneDeformations(scene, MakeVecView(bind), posed));

    for (size_t vertex = 0; vertex < posed.size(); ++vertex)
    {
        EXPECT_EQ(posed[vertex].pos, firstPose[vertex].pos);
    }

    EXPECT_FLOAT_EQ(scene.GetAssetData().unitScale, 0.125f);

    EXPECT_EQ(scene.GetNodes()[0]->GetData().surfaceScale, Vec4(0.25f, 0.375f, 0.5f, 1));
}
