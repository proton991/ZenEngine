#include <gtest/gtest.h>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include "AssetLib/FastGLTFLoader.h"
#include "SceneGraph/Scene.h"
#include "SceneGraph/SceneAnimation.h"
#include "SceneGraph/SceneAnimationPointer.h"

using namespace zen;

namespace
{
class MorphAttributeFixture
{
public:
    MorphAttributeFixture()
    {
        const int64_t identifier = std::chrono::steady_clock::now().time_since_epoch().count();

        m_directory = std::filesystem::temp_directory_path() /
            ("ZenGLTFMorphAttributeTests_" + std::to_string(identifier));

        if (!std::filesystem::create_directory(m_directory))
        {
            throw std::runtime_error("Could not create glTF morph attribute fixture directory");
        }
    }

    ~MorphAttributeFixture()
    {
        std::error_code error;

        std::filesystem::remove_all(m_directory, error);
    }

    template <class T> uint32_t AddAccessor(std::initializer_list<T> values,
                                            uint32_t componentType,
                                            uint32_t count,
                                            const char* type,
                                            const char* bounds = "")
    {
        const size_t offset = m_bytes.size();

        const size_t byteLength = values.size() * sizeof(T);

        m_bytes.resize(offset + byteLength);

        std::memcpy(m_bytes.data() + offset, values.begin(), byteLength);

        const uint32_t result = static_cast<uint32_t>(m_accessors.size());

        m_views.push_back("{\"buffer\":0,\"byteOffset\":" + std::to_string(offset) +
                          ",\"byteLength\":" + std::to_string(byteLength) + "}");

        m_accessors.push_back("{\"bufferView\":" + std::to_string(result) +
                              ",\"componentType\":" + std::to_string(componentType) +
                              ",\"count\":" + std::to_string(count) + ",\"type\":\"" + type + "\"" +
                              bounds + "}");

        return result;
    }

    std::string Write(bool colorHasAlpha,
                      bool gpuInstancing    = false,
                      bool instancedSkin    = false,
                      bool customAttributes = false)
    {
        AddAccessor<float>({0, 0, 0, 1, 0, 0, 0, 1, 0}, 5126, 3, "VEC3",
                           ",\"min\":[0,0,0],\"max\":[1,1,0]");

        AddAccessor<float>({0, 0, 1, 0, 0, 1}, 5126, 3, "VEC2");

        AddAccessor<float>({0, 0, 0, 0, 0, 0}, 5126, 3, "VEC2");

        AddAccessor<float>({0.2f, 0.3f, 0.4f, 0.3f, 0.2f, 0.5f}, 5126, 3, "VEC2");

        AddAccessor<float>({0.4f, 0.2f, 0.8f, 0.4f, 0.2f, 0.8f, 0.4f, 0.2f, 0.8f}, 5126, 3, "VEC3");

        AddAccessor<uint8_t>({0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, 5121, 3, "VEC4");

        AddAccessor<float>({1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0}, 5126, 3, "VEC4");

        AddAccessor<float>({0, 0, 1, 0, 0, 1, 0, 0, 1}, 5126, 3, "VEC3",
                           ",\"min\":[0,0,1],\"max\":[0,0,1]");

        AddAccessor<float>({0.2f, -0.4f, 0.2f, -0.4f, 0.2f, -0.4f}, 5126, 3, "VEC2");

        AddAccessor<float>({0.8f, 0.4f, 0.8f, 0.4f, 0.8f, 0.4f}, 5126, 3, "VEC2");

        if (colorHasAlpha)
        {
            AddAccessor<float>(
                {0.8f, -0.8f, 0.8f, -0.4f, 0.8f, -0.8f, 0.8f, -0.4f, 0.8f, -0.8f, 0.8f, -0.4f},
                5126, 3, "VEC4");
        }
        else
        {
            AddAccessor<float>({0.8f, -0.8f, 0.8f, 0.8f, -0.8f, 0.8f, 0.8f, -0.8f, 0.8f}, 5126, 3,
                               "VEC3");
        }

        AddAccessor<float>(
            {0.1f, 0.2f, 0.3f, -0.4f, 0.1f, 0.2f, 0.3f, -0.4f, 0.1f, 0.2f, 0.3f, -0.4f}, 5126, 3,
            "VEC4");

        AddAccessor<float>({1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}, 5126, 1, "MAT4");

        AddAccessor<float>({0, 1}, 5126, 2, "SCALAR", ",\"min\":[0],\"max\":[1]");

        AddAccessor<float>({0, 1}, 5126, 2, "SCALAR");

        if (customAttributes)
        {
            AddAccessor<float>({1, 2, 3}, 5126, 3, "SCALAR");

            AddAccessor<float>({0.5f, 0.25f, -1.0f}, 5126, 3, "SCALAR");
        }

        if (gpuInstancing)
        {
            if (instancedSkin)
            {
                AddAccessor<float>({0, 0, 0, 2, 0, 0}, 5126, 2, "VEC3");
            }
            else
            {
                AddAccessor<float>({3, 0, 0, -3, 0, 0}, 5126, 2, "VEC3");
            }
        }

        const std::string extensions = gpuInstancing ?
            "\"extensionsUsed\":[\"EXT_mesh_gpu_instancing\"],"
            "\"extensionsRequired\":[\"EXT_mesh_gpu_instancing\"]," :
            "";

        const std::string instanceSkin =
            instancedSkin ? "\"skin\":0,\"translation\":[10,0,0]," : "";

        const std::string instanceNode = gpuInstancing ? ",{\"mesh\":0," + instanceSkin +
                "\"extensions\":{\"EXT_mesh_gpu_instancing\":{"
                "\"attributes\":{\"TRANSLATION\":15}}}}" :
                                                         "";

        const std::string instanceChannel =
            gpuInstancing ? ",{\"sampler\":0,\"target\":{\"node\":3,\"path\":\"weights\"}}" : "";

        const std::string customBase = customAttributes ? ",\"_TEMPERATURE\":15" : "";

        const std::string customTarget = customAttributes ? ",\"_TEMPERATURE\":16" : "";

        std::string json = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0," + extensions +
            "\"scenes\":[{\"nodes\":[0,1,2" + (gpuInstancing ? ",3" : "") +
            "]}],"
            "\"nodes\":[{\"mesh\":0},{\"mesh\":0,\"skin\":0,\"weights\":[0.25]},"
            "{\"translation\":[2,0,0]}" +
            instanceNode +
            "],"
            "\"meshes\":[{" +
            (gpuInstancing ? "" : "\"weights\":[0.5],") +
            "\"primitives\":[{\"attributes\":{"
            "\"POSITION\":0,\"TEXCOORD_0\":1,\"TEXCOORD_1\":2,\"TEXCOORD_2\":3,"
            "\"COLOR_0\":4,\"COLOR_1\":4,\"JOINTS_0\":5,\"WEIGHTS_0\":6" +
            customBase +
            "},"
            "\"targets\":[{\"POSITION\":7,\"TEXCOORD_0\":8,\"TEXCOORD_2\":9,"
            "\"COLOR_0\":10,\"COLOR_1\":11" +
            customTarget +
            "}]}]}],"
            "\"skins\":[{\"joints\":[2],\"inverseBindMatrices\":12}],"
            "\"animations\":[{\"samplers\":[{\"input\":13,\"output\":14}],"
            "\"channels\":[{\"sampler\":0,\"target\":{\"node\":0,\"path\":\"weights\"}}" +
            instanceChannel +
            "]}],"
            "\"buffers\":[{\"uri\":\"attributes.bin\",\"byteLength\":" +
            std::to_string(m_bytes.size()) + "}],\"bufferViews\":" + JsonArray(m_views) +
            ",\"accessors\":" + JsonArray(m_accessors) + "}";

        const std::filesystem::path file = m_directory / "attributes.gltf";

        std::ofstream binary(m_directory / "attributes.bin", std::ios::binary);

        binary.write(m_bytes.data(), static_cast<std::streamsize>(m_bytes.size()));

        std::ofstream output(file, std::ios::binary);

        output.write(json.data(), static_cast<std::streamsize>(json.size()));

        if (!binary || !output)
        {
            throw std::runtime_error("Could not write glTF morph attribute fixture");
        }

        const std::string result = file.string();

        return result;
    }

private:
    static std::string JsonArray(const HeapVector<std::string>& entries)
    {
        std::string result = "[";

        for (size_t index = 0; index < entries.size(); ++index)
        {
            result += index == 0 ? "" : ",";

            result += entries[index];
        }

        result += "]";

        return result;
    }

    std::filesystem::path m_directory;

    std::string m_bytes;

    HeapVector<std::string> m_views;

    HeapVector<std::string> m_accessors;
};

void ExpectVec2(const Vec2& actual, const Vec2& expected)
{
    EXPECT_FLOAT_EQ(actual.x, expected.x);

    EXPECT_FLOAT_EQ(actual.y, expected.y);
}

void ExpectVec4(const Vec4& actual, const Vec4& expected)
{
    EXPECT_FLOAT_EQ(actual.x, expected.x);

    EXPECT_FLOAT_EQ(actual.y, expected.y);

    EXPECT_FLOAT_EQ(actual.z, expected.z);

    EXPECT_FLOAT_EQ(actual.w, expected.w);
}

void ExpectMorphAttributeImport(bool colorHasAlpha)
{
    MorphAttributeFixture fixture;

    sg::Scene scene;

    asset::FastGLTFLoader loader;

    ASSERT_NO_THROW(loader.LoadFromFile(fixture.Write(colorHasAlpha), &scene));

    const sg::SceneAssetData& data = scene.GetAssetData();

    ASSERT_EQ(data.deformations.size(), 2u);

    ASSERT_EQ(data.morphPrimitives.size(), 1u);

    const sg::MorphTargetAsset& target = data.morphPrimitives[0].targets[0];

    ASSERT_EQ(target.texCoords.size(), 3u);

    ASSERT_EQ(target.colors.size(), 3u);

    ASSERT_EQ(target.extraColors.size(), 1u);

    EXPECT_EQ(target.extraColors[0].set, 1u);

    ExpectVec4(target.extraColors[0].values[0], Vec4(0.1f, 0.2f, 0.3f, -0.4f));

    const uint32_t first = data.deformations[0].firstVertex;

    const uint32_t skinned = data.deformations[1].firstVertex;

    ASSERT_EQ(data.deformations[0].node, 0u);

    ASSERT_EQ(data.deformations[1].node, 1u);

    EXPECT_FLOAT_EQ(loader.GetVertices()[first].pos.z, 0.5f);

    EXPECT_FLOAT_EQ(loader.GetVertices()[skinned].pos.x, 2.0f);

    EXPECT_FLOAT_EQ(loader.GetVertices()[skinned].pos.z, 0.25f);

    ExpectVec2(loader.GetVertices()[first].uv0, Vec2(0.1f, -0.2f));

    ExpectVec2(data.vertexTexCoords[first][2], Vec2(0.6f, 0.5f));

    ExpectVec2(data.vertexTexCoords[skinned][2], Vec2(0.4f, 0.4f));

    ExpectVec2(data.bindVertexTexCoords[first][2], Vec2(0.2f, 0.3f));

    ExpectVec4(loader.GetVertices()[first].color,
               Vec4(0.8f, 0.0f, 1.0f, colorHasAlpha ? 0.8f : 1.0f));

    ASSERT_TRUE(sg::EvaluateSceneAnimation(scene, 0, 0.75f, false));

    HeapVector<asset::Vertex> posed;

    const VectorView<const asset::Vertex> bind(data.bindVertices.data(), data.bindVertices.size());

    ASSERT_TRUE(sg::ApplySceneDeformations(scene, bind, posed));

    EXPECT_FLOAT_EQ(posed[first].pos.z, 0.75f);

    EXPECT_FLOAT_EQ(posed[skinned].pos.x, 2.0f);

    EXPECT_FLOAT_EQ(posed[skinned].pos.z, 0.25f);

    ExpectVec2(posed[first].uv0, Vec2(0.15f, -0.3f));

    ExpectVec2(data.vertexTexCoords[first][2], Vec2(0.8f, 0.6f));

    ExpectVec4(posed[first].color, Vec4(1.0f, 0.0f, 1.0f, colorHasAlpha ? 0.7f : 1.0f));

    ASSERT_TRUE(sg::ApplySceneDeformations(scene, bind, posed));

    ExpectVec2(data.vertexTexCoords[first][2], Vec2(0.8f, 0.6f));

    ASSERT_TRUE(sg::EvaluateSceneAnimation(scene, 0, 0.0f, false));

    ASSERT_TRUE(sg::ApplySceneDeformations(scene, bind, posed));

    EXPECT_FLOAT_EQ(posed[first].pos.z, 0.0f);

    ExpectVec2(posed[first].uv0, Vec2(0));

    ExpectVec2(data.vertexTexCoords[first][2], Vec2(0.2f, 0.3f));

    ExpectVec4(posed[first].color, Vec4(0.4f, 0.2f, 0.8f, 1));
}
} // namespace

TEST(SceneImportMorphAttributes, Vec3ColorAndTextureCoordinatesUseDefaultAnimatedAndSkinnedWeights)
{
    ExpectMorphAttributeImport(false);
}

TEST(SceneImportMorphAttributes, Vec4ColorAlphaIsAdditiveAndFinalColorIsClamped)
{
    ExpectMorphAttributeImport(true);
}

TEST(SceneImportMorphAttributes, InstancedMorphsFollowSourceAnimationWithoutAuthoredDefaultWeights)
{
    MorphAttributeFixture fixture;

    sg::Scene scene;

    asset::FastGLTFLoader loader;

    ASSERT_NO_THROW(loader.LoadFromFile(fixture.Write(true, true), &scene));

    const sg::SceneAssetData& data = scene.GetAssetData();

    ASSERT_EQ(data.deformations.size(), 4u);

    ASSERT_EQ(data.morphPrimitives.size(), 1u);

    EXPECT_TRUE(data.morphPrimitives[0].weights.empty());

    for (size_t instance = 2; instance < data.deformations.size(); ++instance)
    {
        const sg::DeformationPrimitiveAsset& binding = data.deformations[instance];

        EXPECT_EQ(binding.morphPrimitive, 0);

        EXPECT_FLOAT_EQ(loader.GetVertices()[binding.firstVertex].pos.z, 0.0f);

        ExpectVec2(data.vertexTexCoords[binding.firstVertex][2], Vec2(0.2f, 0.3f));
    }

    ASSERT_TRUE(sg::EvaluateSceneAnimation(scene, 0, 0.75f, false));

    HeapVector<asset::Vertex> posed;

    const VectorView<const asset::Vertex> bind(data.bindVertices.data(), data.bindVertices.size());

    ASSERT_TRUE(sg::ApplySceneDeformations(scene, bind, posed));

    for (size_t instance = 2; instance < data.deformations.size(); ++instance)
    {
        const uint32_t first = data.deformations[instance].firstVertex;

        EXPECT_FLOAT_EQ(posed[first].pos.z, 0.75f);

        ExpectVec2(posed[first].uv0, Vec2(0.15f, -0.3f));

        ExpectVec2(data.vertexTexCoords[first][2], Vec2(0.8f, 0.6f));

        ExpectVec4(posed[first].color, Vec4(1.0f, 0.0f, 1.0f, 0.7f));
    }

    ASSERT_TRUE(sg::EvaluateSceneAnimation(scene, 0, 0.0f, false));

    ASSERT_TRUE(sg::ApplySceneDeformations(scene, bind, posed));

    for (size_t instance = 2; instance < data.deformations.size(); ++instance)
    {
        const uint32_t first = data.deformations[instance].firstVertex;

        EXPECT_FLOAT_EQ(posed[first].pos.z, 0.0f);

        ExpectVec2(data.vertexTexCoords[first][2], Vec2(0.2f, 0.3f));

        ExpectVec4(posed[first].color, Vec4(0.4f, 0.2f, 0.8f, 1));
    }
}

TEST(SceneImportMorphAttributes, SkinnedInstancesSeparateAndFollowSourceWeightAnimationsAndPointers)
{
    MorphAttributeFixture fixture;

    sg::Scene scene;

    asset::FastGLTFLoader loader;

    ASSERT_NO_THROW(loader.LoadFromFile(fixture.Write(true, true, true), &scene));

    const sg::SceneAssetData& data = scene.GetAssetData();

    ASSERT_EQ(data.deformations.size(), 4u);

    for (size_t instance = 2; instance < data.deformations.size(); ++instance)
    {
        const uint32_t first = data.deformations[instance].firstVertex;

        // Joint translation is two; the skin source's authored translation ten is
        // ignored. The second virtual instance adds two in the source frame.
        EXPECT_FLOAT_EQ(loader.GetVertices()[first].pos.x,
                        2.0f + 2.0f * static_cast<float>(instance - 2));

        EXPECT_FLOAT_EQ(loader.GetVertices()[first].pos.z, 0.0f);
    }

    ASSERT_TRUE(sg::EvaluateSceneAnimation(scene, 0, 0.75f, false));

    HeapVector<asset::Vertex> posed;

    const VectorView<const asset::Vertex> bind(data.bindVertices.data(), data.bindVertices.size());

    ASSERT_TRUE(sg::ApplySceneDeformations(scene, bind, posed));

    for (size_t instance = 2; instance < data.deformations.size(); ++instance)
    {
        const uint32_t first = data.deformations[instance].firstVertex;

        EXPECT_FLOAT_EQ(posed[first].pos.x, 2.0f + 2.0f * static_cast<float>(instance - 2));

        EXPECT_FLOAT_EQ(posed[first].pos.z, 0.75f);

        ExpectVec2(data.vertexTexCoords[first][2], Vec2(0.8f, 0.6f));

        ExpectVec4(posed[first].color, Vec4(1.0f, 0.0f, 1.0f, 0.7f));
    }

    const float sourceWeight[]{0.5f};

    const VectorView<const float> weight(sourceWeight, 1);

    ASSERT_TRUE(sg::ValidateAnimationPointer(scene, "/nodes/3/weights/0", weight));

    ASSERT_TRUE(sg::ApplyAnimationPointer(scene, "/nodes/3/weights/0", weight));

    ASSERT_TRUE(sg::ApplySceneDeformations(scene, bind, posed));

    for (size_t instance = 2; instance < data.deformations.size(); ++instance)
    {
        const uint32_t first = data.deformations[instance].firstVertex;

        EXPECT_FLOAT_EQ(posed[first].pos.x, 2.0f + 2.0f * static_cast<float>(instance - 2));

        EXPECT_FLOAT_EQ(posed[first].pos.z, 0.5f);

        ExpectVec2(data.vertexTexCoords[first][2], Vec2(0.6f, 0.5f));

        ExpectVec4(posed[first].color, Vec4(0.8f, 0.0f, 1.0f, 0.8f));
    }
}

TEST(SceneImportMorphAttributes, CustomScalarTargetRetainsDecodedValuesAndItsTargetIndex)
{
    MorphAttributeFixture fixture;

    sg::Scene scene;

    asset::FastGLTFLoader loader;

    ASSERT_NO_THROW(loader.LoadFromFile(fixture.Write(false, false, false, true), &scene));

    const sg::VertexAttributeAsset* base = nullptr;

    const sg::VertexAttributeAsset* target = nullptr;

    for (const sg::VertexAttributeAsset& attribute : scene.GetAssetData().extraVertexAttributes)
    {
        if (attribute.semantic == "_TEMPERATURE")
        {
            if (attribute.morphTarget == -1)
            {
                base = &attribute;
            }
            else if (attribute.morphTarget == 0)
            {
                target = &attribute;
            }
        }
    }

    ASSERT_NE(base, nullptr);

    ASSERT_NE(target, nullptr);

    EXPECT_EQ(base->mesh, 0u);

    EXPECT_EQ(target->primitive, 0u);

    EXPECT_EQ(target->components, 1u);

    EXPECT_EQ(target->componentType, 5126u);

    EXPECT_FALSE(target->normalized);

    ASSERT_EQ(base->values.size(), 3u);

    ASSERT_EQ(target->values.size(), 3u);

    EXPECT_DOUBLE_EQ(base->values[0], 1.0);

    EXPECT_DOUBLE_EQ(base->values[2], 3.0);

    EXPECT_DOUBLE_EQ(target->values[0], 0.5);

    EXPECT_DOUBLE_EQ(target->values[1], 0.25);

    EXPECT_DOUBLE_EQ(target->values[2], -1.0);

    const uint32_t first = scene.GetAssetData().deformations[0].firstVertex;

    EXPECT_FLOAT_EQ(loader.GetVertices()[first].pos.z, 0.5f);
}
