#include <gtest/gtest.h>
#include <limits>
#include "SceneGraph/Scene.h"
#include "SceneGraph/SceneAnimationPointer.h"
#include "SceneGraph/SceneAnimation.h"

using namespace zen;

namespace
{
sg::Material* AddPointerMaterial(sg::Scene& scene)
{
    scene.LoadDefaultTextures(0);

    UniquePtr<sg::Material> material = MakeUnique<sg::Material>("PointerMaterial");

    const sg::Scene::DefaultTextures textures = scene.GetDefaultTextures();

    material->m_pBaseColorTexture = textures.pBaseColor;

    material->m_pMetallicRoughnessTexture = textures.pMetallicRoughness;

    material->m_pNormalTexture = textures.pNormal;

    material->m_pOcclusionTexture = textures.pOcclusion;

    material->m_pEmissiveTexture = textures.pEmissive;

    sg::Material* result = material.Get();

    scene.AddComponent(std::move(material));

    return result;
}
} // namespace

TEST(SceneAnimationPointer, ValidatesWithoutMutationAndRejectsInvalidShapesAndPaths)
{
    sg::Scene scene;

    sg::Material* material = AddPointerMaterial(scene);

    const float color[] = {0.2f, 0.4f, 0.6f, 0.8f};

    const std::string path = "/materials/0/pbrMetallicRoughness/baseColorFactor";

    EXPECT_TRUE(sg::ValidateAnimationPointer(scene, path, color));

    EXPECT_EQ(material->baseColorFactor, Vec4(1));

    EXPECT_TRUE(sg::ApplyAnimationPointer(scene, path, color));

    EXPECT_EQ(material->baseColorFactor, Vec4(0.2f, 0.4f, 0.6f, 0.8f));

    EXPECT_EQ(material->data.baseColorFactor, material->baseColorFactor);

    const float component[] = {0.9f};

    EXPECT_FALSE(sg::ApplyAnimationPointer(scene, path + "/2", component));

    EXPECT_EQ(material->baseColorFactor, Vec4(0.2f, 0.4f, 0.6f, 0.8f));

    EXPECT_FALSE(sg::ApplyAnimationPointer(scene, path + "/4", component));

    EXPECT_FALSE(sg::ApplyAnimationPointer(scene, path, component));

    EXPECT_FALSE(sg::ApplyAnimationPointer(scene, "/materials/01/emissiveFactor", color));

    EXPECT_FALSE(sg::ApplyAnimationPointer(scene, "/materials/0/unknown", component));

    EXPECT_FALSE(sg::ApplyAnimationPointer(
        scene, "/materials/0/pbrMetallicRoughness~1baseColorFactor", color));

    const float invalid[] = {std::numeric_limits<float>::quiet_NaN()};

    EXPECT_FALSE(sg::ApplyAnimationPointer(scene, path + "/0", invalid));

    EXPECT_EQ(material->baseColorFactor, Vec4(0.2f, 0.4f, 0.6f, 0.8f));
}

TEST(SceneAnimationPointer, AnimatedTextureTransformsRetainOtherAuthoredTermsAndSampler)
{
    sg::Scene scene;

    sg::Material* material = AddPointerMaterial(scene);

    sg::TextureTransform& transform = material->features.specularColorTexture.transform;

    transform.uvScale = Vec2(2, 3);

    transform.uvRotation = glm::half_pi<float>();

    transform.row0.w = 7;

    const float offset[] = {0.2f, 0.3f};

    const std::string prefix =
        "/materials/0/extensions/KHR_materials_specular/specularColorTexture/extensions/KHR_texture_transform/";

    EXPECT_TRUE(sg::ValidateAnimationPointer(scene, prefix + "offset", offset));

    EXPECT_EQ(transform.uvOffset, Vec2(0));

    EXPECT_TRUE(sg::ApplyAnimationPointer(scene, prefix + "offset", offset));

    EXPECT_NEAR(transform.row0.x, 0.0f, 1e-6f);

    EXPECT_NEAR(transform.row0.y, -3.0f, 1e-6f);

    EXPECT_NEAR(transform.row1.x, 2.0f, 1e-6f);

    EXPECT_NEAR(transform.row1.y, 0.0f, 1e-6f);

    EXPECT_FLOAT_EQ(transform.row0.z, 0.2f);

    EXPECT_FLOAT_EQ(transform.row1.z, 0.3f);

    EXPECT_FLOAT_EQ(transform.row0.w, 7.0f);

    const float rotation[] = {0};

    EXPECT_TRUE(sg::ApplyAnimationPointer(scene, prefix + "rotation", rotation));

    EXPECT_EQ(transform.row0, Vec4(2, 0, 0.2f, 7));

    EXPECT_EQ(transform.row1, Vec4(0, 3, 0.3f, 0));

    const sg::MaterialTextureData& gpu =
        material->data
            .featureTextures[static_cast<uint32_t>(sg::MaterialFeatureTexture::SpecularColor)];

    EXPECT_EQ(gpu.transform, sg::PublishTextureTransform(transform));
}

TEST(SceneAnimationPointer, ZeroAuthoredIorRemainsPermanentAndAnimatedIorMustBeAtLeastOne)
{
    sg::Scene scene;

    sg::Material* material = AddPointerMaterial(scene);

    material->features.ior = 0;

    const std::string path = "/materials/0/extensions/KHR_materials_ior/ior";

    const float allowed[] = {1.7f};

    EXPECT_TRUE(sg::ApplyAnimationPointer(scene, path, allowed));

    EXPECT_FLOAT_EQ(material->features.ior, 0.0f);

    const float disallowed[] = {0};

    EXPECT_FALSE(sg::ApplyAnimationPointer(scene, path, disallowed));

    EXPECT_FLOAT_EQ(material->features.ior, 0.0f);

    material->features.ior = 1.5f;

    EXPECT_TRUE(sg::ApplyAnimationPointer(scene, path, allowed));

    EXPECT_FLOAT_EQ(material->features.ior, 1.7f);
}

TEST(SceneAnimationPointer, SharedLightAndCameraInstancesKeepTheirNormalizedUnits)
{
    sg::Scene scene;

    for (uint32_t instance = 0; instance < 2; ++instance)
    {
        UniquePtr<sg::Light> light = MakeUnique<sg::Light>("SharedLight");

        light->sourceIndex = 3;

        light->unitScale = instance == 0 ? 0.5f : 0.25f;

        scene.AddComponent(std::move(light));

        UniquePtr<sg::SceneCamera> camera = MakeUnique<sg::SceneCamera>("SharedCamera");

        camera->sourceIndex = 4;

        camera->unitScale = instance == 0 ? 0.5f : 0.25f;

        camera->infiniteFar = true;

        scene.AddComponent(std::move(camera));
    }

    const float intensity[] = {80};

    EXPECT_TRUE(sg::ValidateAnimationPointer(
        scene, "/extensions/KHR_lights_punctual/lights/3/intensity", intensity));

    EXPECT_FLOAT_EQ(scene.GetComponents<sg::Light>()[0]->GetProperties().intensity, 1.0f);

    EXPECT_TRUE(sg::ApplyAnimationPointer(
        scene, "/extensions/KHR_lights_punctual/lights/3/intensity", intensity));

    EXPECT_FLOAT_EQ(scene.GetComponents<sg::Light>()[0]->GetProperties().intensity, 20.0f);

    EXPECT_FLOAT_EQ(scene.GetComponents<sg::Light>()[1]->GetProperties().intensity, 5.0f);

    const float farPlane[] = {100};

    EXPECT_TRUE(sg::ApplyAnimationPointer(scene, "/cameras/4/perspective/zfar", farPlane));

    EXPECT_FLOAT_EQ(scene.GetComponents<sg::SceneCamera>()[0]->farPlane, 50.0f);

    EXPECT_FLOAT_EQ(scene.GetComponents<sg::SceneCamera>()[1]->farPlane, 25.0f);

    EXPECT_FALSE(scene.GetComponents<sg::SceneCamera>()[0]->infiniteFar);

    EXPECT_FALSE(sg::ApplyAnimationPointer(
        scene, "/extensions/KHR_lights_punctual/lights/not-an-index/intensity", intensity));

    EXPECT_FLOAT_EQ(scene.GetComponents<sg::Light>()[0]->GetProperties().intensity, 20.0f);
}

TEST(SceneAnimationPointer, NodeComponentsAndVisibilityUpdateWithoutResettingOtherTransformTerms)
{
    sg::Scene scene;

    UniquePtr<sg::Node> node = MakeUnique<sg::Node>(7, "PointerNode");

    UniquePtr<sg::Transform> transform = MakeUnique<sg::Transform>(*node);

    transform->SetTranslation(Vec3(1, 2, 3));

    transform->SetScale(Vec3(4, 5, 6));

    node->AddComponent(transform.Get());

    scene.AddComponent(std::move(transform));

    std::vector<UniquePtr<sg::Node>> nodes;

    nodes.push_back(std::move(node));

    scene.SetNodes(std::move(nodes));

    const float position[] = {1, 8, 3};

    EXPECT_TRUE(sg::ValidateAnimationPointer(scene, "/nodes/7/translation", position));

    EXPECT_EQ(scene.GetNodes()[0]->GetComponent<sg::Transform>()->GetTranslation(), Vec3(1, 2, 3));

    EXPECT_TRUE(sg::ApplyAnimationPointer(scene, "/nodes/7/translation", position));

    EXPECT_EQ(scene.GetNodes()[0]->GetComponent<sg::Transform>()->GetTranslation(), Vec3(1, 8, 3));

    EXPECT_EQ(scene.GetNodes()[0]->GetComponent<sg::Transform>()->GetScale(), Vec3(4, 5, 6));

    const float visible[] = {0};

    EXPECT_TRUE(sg::ApplyAnimationPointer(scene, "/nodes/7/extensions/KHR_node_visibility/visible",
                                          visible));

    EXPECT_FALSE(scene.GetNodes()[0]->IsVisible());

    scene.GetAssetData().sourceDocument = R"({"nodes":[{}],"cameras":[{}]})";

    EXPECT_TRUE(sg::ApplyAnimationPointer(scene, "/nodes/0/translation", position));

    EXPECT_FALSE(sg::ApplyAnimationPointer(scene, "/nodes/1/translation", position));
}

TEST(SceneAnimationPointer, MatrixTranslationReplacesAuthoredValuesAndMorphElementsKeepDefaults)
{
    sg::Scene scene;

    UniquePtr<sg::Node> node = MakeUnique<sg::Node>(0, "MatrixNode");

    UniquePtr<sg::Transform> transform = MakeUnique<sg::Transform>(*node);

    Mat4 authored = glm::scale(Mat4(1), Vec3(2));

    authored[3] = Vec4(5, 6, 7, 1);

    transform->SetLocalMatrix(authored);

    node->AddComponent(transform.Get());

    scene.AddComponent(std::move(transform));

    std::vector<UniquePtr<sg::Node>> nodes;

    nodes.push_back(std::move(node));

    scene.SetNodes(std::move(nodes));

    scene.GetAssetData().sourceDocument =
        R"({"nodes":[{"matrix":[2,0,0,0,0,2,0,0,0,0,2,0,5,6,7,1]}]})";

    const float position[] = {1, 2, 3};

    EXPECT_TRUE(sg::ApplyAnimationPointer(scene, "/nodes/0/translation", position));

    EXPECT_EQ(Vec3(scene.GetNodes()[0]->GetComponent<sg::Transform>()->GetWorldMatrix()[3]),
              Vec3(1, 2, 3));

    const float scale[] = {3, 4, 5};

    EXPECT_FALSE(sg::ApplyAnimationPointer(scene, "/nodes/0/scale", scale));

    sg::MorphPrimitiveAsset morph;

    morph.targets.resize(2);

    morph.weights = {0.25f, 0.75f};

    scene.GetAssetData().morphPrimitives.push_back(std::move(morph));

    sg::DeformationPrimitiveAsset deformation;

    deformation.node = 0;

    deformation.morphPrimitive = 0;

    scene.GetAssetData().deformations.push_back(std::move(deformation));

    const float weight[] = {0.9f};

    EXPECT_TRUE(sg::ApplyAnimationPointer(scene, "/nodes/0/weights/1", weight));

    ASSERT_EQ(scene.GetNodes()[0]->morphWeights.size(), 2u);

    EXPECT_FLOAT_EQ(scene.GetNodes()[0]->morphWeights[0], 0.25f);

    EXPECT_FLOAT_EQ(scene.GetNodes()[0]->morphWeights[1], 0.9f);

    EXPECT_FALSE(sg::ApplyAnimationPointer(scene, "/nodes/0/weights/2", weight));
}

TEST(SceneAnimationPointer, InstancedSourceWeightsUseTargetCountWithoutAuthoredDefaults)
{
    sg::Scene scene;

    UniquePtr<sg::Node> source = MakeUnique<sg::Node>(0, "InstancedSource");

    UniquePtr<sg::Node> instance = MakeUnique<sg::Node>(1, "VirtualInstance");

    instance->morphWeightsSourceNode = 0;

    UniquePtr<sg::Mesh> mesh = MakeUnique<sg::Mesh>("SourceMesh");

    UniquePtr<sg::SubMesh> primitive = MakeUnique<sg::SubMesh>("SourcePrimitive");

    primitive->assetMesh = 3;

    mesh->AddSubMesh(primitive.Get());

    source->AddComponent(mesh.Get());

    scene.AddComponent(std::move(primitive));

    scene.AddComponent(std::move(mesh));

    std::vector<UniquePtr<sg::Node>> nodes;

    nodes.push_back(std::move(source));

    nodes.push_back(std::move(instance));

    scene.SetNodes(std::move(nodes));

    sg::MorphPrimitiveAsset morph;

    morph.mesh = 3;

    morph.targets.resize(2);

    scene.GetAssetData().morphPrimitives.push_back(std::move(morph));

    sg::DeformationPrimitiveAsset deformation;

    deformation.node = 1;

    deformation.morphPrimitive = 0;

    scene.GetAssetData().deformations.push_back(std::move(deformation));

    const float weight[] = {0.75f};

    EXPECT_TRUE(sg::ValidateAnimationPointer(scene, "/nodes/0/weights/1", weight));

    EXPECT_TRUE(scene.GetNodes()[0]->morphWeights.empty());

    EXPECT_TRUE(sg::ApplyAnimationPointer(scene, "/nodes/0/weights/1", weight));

    ASSERT_EQ(scene.GetNodes()[0]->morphWeights.size(), 2u);

    EXPECT_FLOAT_EQ(scene.GetNodes()[0]->morphWeights[0], 0.0f);

    EXPECT_FLOAT_EQ(scene.GetNodes()[0]->morphWeights[1], 0.75f);

    EXPECT_FALSE(sg::ApplyAnimationPointer(scene, "/nodes/0/weights/2", weight));
}

TEST(SceneAnimationPointer, UpdatesImageBasedLightIntensityAndWholeQuaternionWithoutChangingMipData)
{
    sg::Scene scene;

    sg::ImageBasedLightAsset light;

    light.intensity = 2.0f;

    light.size = 1;

    light.mipLevels = 1;

    light.specularMipFaces = HeapVector<HeapVector<Vec4>>(6, HeapVector<Vec4>{Vec4(1, 2, 3, 4)});

    scene.GetAssetData().imageBasedLights.push_back(std::move(light));

    scene.GetAssetData().imageBasedLights.emplace_back();

    scene.GetAssetData().imageBasedLight = 0;

    const std::string prefix = "/extensions/EXT_lights_image_based/lights/0/";

    const float intensity[] = {7.0f};

    EXPECT_TRUE(sg::ValidateAnimationPointer(scene, prefix + "intensity", intensity));

    EXPECT_FLOAT_EQ(scene.GetAssetData().imageBasedLights[0].intensity, 2.0f);

    EXPECT_TRUE(sg::ApplyAnimationPointer(scene, prefix + "intensity", intensity));

    EXPECT_FLOAT_EQ(scene.GetAssetData().imageBasedLights[0].intensity, 7.0f);

    const float rotation[] = {0, 2, 0, 2};

    EXPECT_TRUE(sg::ValidateAnimationPointer(scene, prefix + "rotation", rotation));

    EXPECT_EQ(scene.GetAssetData().imageBasedLights[0].rotation, Quat(1, 0, 0, 0));

    EXPECT_TRUE(sg::ApplyAnimationPointer(scene, prefix + "rotation", rotation));

    EXPECT_EQ(scene.GetAssetData().imageBasedLights[0].rotation, glm::normalize(Quat(2, 0, 2, 0)));

    EXPECT_FALSE(sg::ApplyAnimationPointer(scene, prefix + "rotation/1", intensity));

    const float zeroQuaternion[] = {0, 0, 0, 0};

    EXPECT_FALSE(sg::ApplyAnimationPointer(scene, prefix + "rotation", zeroQuaternion));

    const float negative[] = {-1};

    EXPECT_FALSE(sg::ApplyAnimationPointer(scene, prefix + "intensity", negative));

    const float nonFinite[] = {std::numeric_limits<float>::infinity()};

    EXPECT_FALSE(sg::ApplyAnimationPointer(scene, prefix + "intensity", nonFinite));

    EXPECT_FALSE(sg::ApplyAnimationPointer(
        scene, "/extensions/EXT_lights_image_based/lights/2/intensity", intensity));

    EXPECT_TRUE(sg::ApplyAnimationPointer(
        scene, "/extensions/EXT_lights_image_based/lights/1/intensity", intensity));

    EXPECT_EQ(scene.GetAssetData().imageBasedLight, 0);

    EXPECT_EQ(scene.GetAssetData().imageBasedLights[0].specularMipFaces[5][0], Vec4(1, 2, 3, 4));
}

TEST(SceneAnimationPointer,
     ImageBasedLightRotationUsesShortestQuaternionInterpolationAndAtomicValidation)
{
    sg::Scene scene;

    scene.GetAssetData().imageBasedLights.emplace_back();

    sg::AnimationSampler rotation;

    rotation.components = 4;

    rotation.times = {0, 1};

    rotation.values = {0, 0, 0, 1, 0, 0, 0, -1};

    sg::AnimationSampler intensity;

    intensity.components = 1;

    intensity.times = {0, 1};

    intensity.values = {2, 6};

    sg::AnimationAsset animation;

    animation.samplers.push_back(std::move(rotation));

    animation.samplers.push_back(std::move(intensity));

    sg::AnimationChannel rotationChannel;

    rotationChannel.path = sg::AnimationPath::Pointer;

    rotationChannel.pointer = "/extensions/EXT_lights_image_based/lights/0/rotation";

    animation.channels.push_back(std::move(rotationChannel));

    sg::AnimationChannel intensityChannel;

    intensityChannel.sampler = 1;

    intensityChannel.path = sg::AnimationPath::Pointer;

    intensityChannel.pointer = "/extensions/EXT_lights_image_based/lights/0/intensity";

    animation.channels.push_back(std::move(intensityChannel));

    scene.GetAssetData().animations.push_back(std::move(animation));

    ASSERT_TRUE(sg::EvaluateSceneAnimation(scene, 0, 0.5f, false));

    EXPECT_FLOAT_EQ(scene.GetAssetData().imageBasedLights[0].intensity, 4.0f);

    EXPECT_EQ(glm::mat4_cast(scene.GetAssetData().imageBasedLights[0].rotation), Mat4(1));

    const Quat previous = scene.GetAssetData().imageBasedLights[0].rotation;

    scene.GetAssetData().animations[0].samplers[1].values = {-2, -6};

    EXPECT_FALSE(sg::EvaluateSceneAnimation(scene, 0, 1.0f, false));

    EXPECT_EQ(scene.GetAssetData().imageBasedLights[0].rotation, previous);

    EXPECT_FLOAT_EQ(scene.GetAssetData().imageBasedLights[0].intensity, 4.0f);
}
