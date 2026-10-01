#include <gtest/gtest.h>
#include "SceneGraph/Scene.h"
#include "SceneGraph/SceneAnimation.h"
#include "Systems/SceneEditor.h"

using namespace zen;

static void ExpectFloatValues(const HeapVector<float>& values,
                              std::initializer_list<float> expected)
{
    ASSERT_EQ(values.size(), expected.size());

    size_t index = 0;

    for (float value : expected)
    {
        EXPECT_FLOAT_EQ(values[index], value);

        ++index;
    }
}

TEST(SceneAnimation, SamplersClampAndUseStepAndLinearInterpolation)
{
    sg::AnimationSampler sampler;

    sampler.times = {10.0f, 12.0f};

    sampler.values = {1, 2, 3, 5, 6, 7};

    HeapVector<float> sampled;

    ASSERT_TRUE(sg::SampleAnimationSampler(sampler, 0, 3, false, sampled));

    ExpectFloatValues(sampled, {1, 2, 3});

    ASSERT_TRUE(sg::SampleAnimationSampler(sampler, 11, 3, false, sampled));

    ExpectFloatValues(sampled, {3, 4, 5});

    ASSERT_TRUE(sg::SampleAnimationSampler(sampler, 100, 3, false, sampled));

    ExpectFloatValues(sampled, {5, 6, 7});

    sampler.interpolation = sg::AnimationInterpolation::Step;

    ASSERT_TRUE(sg::SampleAnimationSampler(sampler, 11.9f, 3, false, sampled));

    EXPECT_FLOAT_EQ(sampled[0], 1);

    ASSERT_TRUE(sg::SampleAnimationSampler(sampler, 12, 3, false, sampled));

    EXPECT_FLOAT_EQ(sampled[0], 5);
}

TEST(SceneAnimation, CubicTangentsUseKeyframeDurationAndScalarWeightGroups)
{
    sg::AnimationSampler sampler;

    sampler.interpolation = sg::AnimationInterpolation::CubicSpline;

    sampler.times = {0, 2};

    // Two scalar weights: in tangents, values, out tangents for each key.
    sampler.values = {0, 0, 0, 1, 4, 0, 0, 0, 2, 3, 0, 0};

    HeapVector<float> sampled;

    ASSERT_TRUE(sg::SampleAnimationSampler(sampler, 1, 2, false, sampled));

    EXPECT_FLOAT_EQ(sampled[0], 2);

    EXPECT_FLOAT_EQ(sampled[1], 2);

    sampler.times[1] = 0;

    sampled = {42};

    EXPECT_FALSE(sg::SampleAnimationSampler(sampler, 1, 2, false, sampled));

    ExpectFloatValues(sampled, {42});
}

TEST(SceneAnimation, RotationUsesShortestSphericalPathAndNormalizedCubicValues)
{
    sg::AnimationSampler sampler;

    sampler.times = {0, 1};

    sampler.values = {0, 0, 0, 1, 0, 0, 0, -1};

    HeapVector<float> sampled;

    ASSERT_TRUE(sg::SampleAnimationSampler(sampler, 0.5f, 4, true, sampled));

    EXPECT_NEAR(std::abs(sampled[3]), 1.0f, 1e-6f);

    sampler.interpolation = sg::AnimationInterpolation::CubicSpline;

    sampler.values = {0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0};

    ASSERT_TRUE(sg::SampleAnimationSampler(sampler, 0.5f, 4, true, sampled));

    EXPECT_NEAR(sampled[2], std::sqrt(0.5f), 1e-6f);

    EXPECT_NEAR(sampled[3], std::sqrt(0.5f), 1e-6f);
}

static sg::Node* AddAnimationNode(sg::Scene& scene,
                                  std::vector<UniquePtr<sg::Node>>& nodes,
                                  uint32_t index,
                                  const Vec3& translation)
{
    UniquePtr<sg::Node> node = MakeUnique<sg::Node>(index, "animation node");

    UniquePtr<sg::Transform> transform = MakeUnique<sg::Transform>(*node);

    transform->SetTranslation(translation);

    node->AddComponent(transform.Get());

    node->SetData(index, transform->GetWorldMatrix());

    scene.AddComponent(std::move(transform));

    sg::Node* result = node.Get();

    nodes.push_back(std::move(node));

    return result;
}

TEST(SceneAnimation, AuthoredRootAnimationPreservesSceneNormalization)
{
    sg::Scene scene;

    std::vector<UniquePtr<sg::Node>> nodes;

    sg::Node* root = AddAnimationNode(scene, nodes, 0, Vec3(10, 0, 0));

    root->GetComponent<sg::Transform>()->SetPrefixMatrix(glm::scale(Mat4(1), Vec3(0.25f)));

    scene.SetNodes(std::move(nodes));

    sg::AnimationAsset animation;

    sg::AnimationSampler sampler;

    sampler.times = {0, 2};

    sampler.values = {10, 0, 0, 14, 0, 0};

    animation.samplers.push_back(std::move(sampler));

    animation.channels.push_back({0, 0, sg::AnimationPath::Translation});

    scene.GetAssetData().animations.push_back(std::move(animation));

    ASSERT_TRUE(sg::EvaluateSceneAnimation(scene, 0, 1, false));

    EXPECT_NEAR(root->GetData().modelMatrix[3].x, 3.0f, 1e-6f);

    ASSERT_TRUE(sg::EvaluateSceneAnimation(scene, 0, 3, true));

    EXPECT_NEAR(root->GetData().modelMatrix[3].x, 3.0f, 1e-6f);
}

TEST(SceneAnimation, MorphPrecedesSkinAndInstancesUseIndependentWeights)
{
    sg::Scene scene;

    std::vector<UniquePtr<sg::Node>> nodes;

    sg::Node* skinned = AddAnimationNode(scene, nodes, 0, Vec3(100, 0, 0));

    AddAnimationNode(scene, nodes, 1, Vec3(3, 0, 0));

    sg::Node* other = AddAnimationNode(scene, nodes, 2, Vec3(0));

    scene.SetNodes(std::move(nodes));

    skinned->skinIndex = 0;

    skinned->morphWeights = {0.5f};

    other->morphWeights = {1.0f};

    sg::SkinAsset skin;

    skin.joints = {1};

    skin.inverseBindMatrices = {Mat4(1)};

    scene.GetAssetData().skins.push_back(std::move(skin));

    sg::MorphPrimitiveAsset morph;

    sg::MorphTargetAsset target;

    target.positions = {Vec3(0, 2, 0)};

    morph.targets.push_back(std::move(target));

    scene.GetAssetData().morphPrimitives.push_back(std::move(morph));

    sg::DeformationPrimitiveAsset first;

    first.node = 0;

    first.firstVertex = 0;

    first.vertexCount = 1;

    first.morphPrimitive = 0;

    first.influences.push_back(HeapVector<sg::VertexJointInfluence>({{0, 1.0f}}));

    scene.GetAssetData().deformations.push_back(std::move(first));

    sg::DeformationPrimitiveAsset second;

    second.node = 2;

    second.firstVertex = 1;

    second.vertexCount = 1;

    second.morphPrimitive = 0;

    scene.GetAssetData().deformations.push_back(std::move(second));

    asset::Vertex rest{};

    rest.pos = Vec4(1, 0, 0, 1);

    rest.normal = Vec4(0, 1, 0, 0);

    HeapVector<asset::Vertex> bind = {rest, rest};

    HeapVector<asset::Vertex> posed;

    ASSERT_TRUE(sg::ApplySceneDeformations(scene, MakeVecView(bind), posed));

    EXPECT_EQ(posed[0].pos, Vec4(4, 1, 0, 1));

    EXPECT_EQ(posed[1].pos, Vec4(1, 2, 0, 1));

    EXPECT_EQ(skinned->GetData().modelMatrix, Mat4(1));

    EXPECT_EQ(bind[0].pos, Vec4(1, 0, 0, 1));
}
