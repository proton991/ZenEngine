#include "AssetLib/SceneLoader.h"
#include "AssetLib/SceneRuntimeBuilder.h"
#include "SceneGraph/SceneAnimation.h"
#include <gtest/gtest.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

using namespace zen;

namespace
{
std::string AsyncFixture(const char* name)
{
    return (std::filesystem::path(__FILE__).parent_path() / "Assets" / name).string();
}

void WaitForSceneGate(std::shared_future<void> gate)
{
    gate.wait();
}

class ParallelSceneFixture
{
public:
    ParallelSceneFixture()
    {
        const int64_t stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        m_path = std::filesystem::temp_directory_path() / ("zen_parallel_scene_" + std::to_string(stamp) + ".gltf");
        std::ostringstream document;
        document << R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":36,)"
                    R"("uri":"data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAA"}],)"
                    R"("bufferViews":[{"buffer":0,"byteLength":36}],)"
                    R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3",)"
                    R"("min":[0,0,0],"max":[1,1,0]}],"materials":[{},{}],"meshes":[)";
        for (uint32_t mesh = 0; mesh < 3; ++mesh)
        {
            if (mesh != 0)
            {
                document << ',';
            }
            document << R"({"primitives":[)";
            for (uint32_t primitive = 0; primitive < 17; ++primitive)
            {
                if (primitive != 0)
                {
                    document << ',';
                }
                document << R"({"attributes":{"POSITION":0},"material":)" << primitive % 2 << '}';
            }
            document << "]}";
        }
        document << R"(],"nodes":[{"mesh":0},{"mesh":1},{"mesh":2}],"scenes":[{"nodes":[0,1,2]}],"scene":0})";
        std::ofstream file(m_path, std::ios::binary);
        file << document.str();
        VERIFY_EXPR_MSG(bool(file), "Cannot write the parallel scene test fixture");
    }

    ~ParallelSceneFixture()
    {
        std::error_code error;
        std::filesystem::remove(m_path, error);
    }

    std::string Path() const
    {
        return m_path.string();
    }

private:
    std::filesystem::path m_path;
};

void ExpectGeometryEqual(const asset::LoadedScene& actual, const asset::LoadedScene& expected)
{
    ASSERT_EQ(actual.vertices.size(), expected.vertices.size());
    ASSERT_EQ(actual.indices.size(), expected.indices.size());
    for (size_t index = 0; index < actual.vertices.size(); ++index)
    {
        const asset::Vertex& a = actual.vertices[index];
        const asset::Vertex& b = expected.vertices[index];
        EXPECT_EQ(a.pos, b.pos);
        EXPECT_EQ(a.normal, b.normal);
        EXPECT_EQ(a.tangent, b.tangent);
        EXPECT_EQ(a.uv0, b.uv0);
        EXPECT_EQ(a.uv1, b.uv1);
        EXPECT_EQ(a.joint0, b.joint0);
        EXPECT_EQ(a.weight0, b.weight0);
        EXPECT_EQ(a.color, b.color);
    }
    for (size_t index = 0; index < actual.indices.size(); ++index)
    {
        EXPECT_EQ(actual.indices[index], expected.indices[index]);
    }
    EXPECT_EQ(actual.scene->GetAABB(), expected.scene->GetAABB());
    EXPECT_EQ(actual.normalizationCenter, expected.normalizationCenter);
    EXPECT_EQ(actual.normalizationScale, expected.normalizationScale);

    const HeapVector<sg::SubMesh*> actualMeshes   = actual.scene->GetComponents<sg::SubMesh>();
    const HeapVector<sg::SubMesh*> expectedMeshes = expected.scene->GetComponents<sg::SubMesh>();
    ASSERT_EQ(actualMeshes.size(), expectedMeshes.size());
    for (size_t index = 0; index < actualMeshes.size(); ++index)
    {
        const sg::SubMesh& a = *actualMeshes[index];
        const sg::SubMesh& b = *expectedMeshes[index];
        EXPECT_EQ(a.GetName(), b.GetName());
        EXPECT_EQ(a.assetMesh, b.assetMesh);
        EXPECT_EQ(a.assetPrimitive, b.assetPrimitive);
        EXPECT_EQ(a.topology, b.topology);
        EXPECT_EQ(a.GetMaterialIndex(), b.GetMaterialIndex());
        EXPECT_EQ(a.GetFirstIndex(), b.GetFirstIndex());
        EXPECT_EQ(a.GetIndexCount(), b.GetIndexCount());
        EXPECT_EQ(a.GetVertexCount(), b.GetVertexCount());
    }
    const sg::SceneAssetData& a = actual.scene->GetAssetData();
    const sg::SceneAssetData& b = expected.scene->GetAssetData();
    ASSERT_EQ(a.vertexTexCoords.size(), b.vertexTexCoords.size());
    for (size_t vertex = 0; vertex < a.vertexTexCoords.size(); ++vertex)
    {
        ASSERT_EQ(a.vertexTexCoords[vertex].size(), b.vertexTexCoords[vertex].size());
        for (size_t set = 0; set < a.vertexTexCoords[vertex].size(); ++set)
        {
            EXPECT_EQ(a.vertexTexCoords[vertex][set], b.vertexTexCoords[vertex][set]);
        }
    }
    ASSERT_EQ(a.deformations.size(), b.deformations.size());
    for (size_t index = 0; index < a.deformations.size(); ++index)
    {
        const sg::DeformationPrimitiveAsset& left  = a.deformations[index];
        const sg::DeformationPrimitiveAsset& right = b.deformations[index];
        EXPECT_EQ(left.firstVertex, right.firstVertex);
        EXPECT_EQ(left.subMesh, right.subMesh);
        EXPECT_EQ(left.flatNormals, right.flatNormals);
        ASSERT_EQ(left.sourceVertices.size(), right.sourceVertices.size());
        for (size_t vertex = 0; vertex < left.sourceVertices.size(); ++vertex)
        {
            EXPECT_EQ(left.sourceVertices[vertex], right.sourceVertices[vertex]);
        }
    }
    const HeapVector<sg::Texture*> actualTextures   = actual.scene->GetComponents<sg::Texture>();
    const HeapVector<sg::Texture*> expectedTextures = expected.scene->GetComponents<sg::Texture>();
    ASSERT_EQ(actualTextures.size(), expectedTextures.size());
    for (size_t index = 0; index < actualTextures.size(); ++index)
    {
        EXPECT_EQ(*actualTextures[index], *expectedTextures[index]);
        EXPECT_NE(actualTextures[index], expectedTextures[index]);
    }
}
} // namespace

TEST(SceneLoader, ConcurrentRequestsMatchSingleWorkerGeometryTexturesAndRemaps)
{
    ThreadPool<void, uint32_t> serial(1);
    asset::SceneLoader         loader(2, 4);
    const char*                fixtures[]{"complete_scene.gltf",       "complete_scene.glb",  "all_primitive_modes.gltf",
                                          "flat_normals_default.gltf", "texture_roles.gltf",  "vertex_attributes.gltf",
                                          "strip_scene.gltf",          "quantized_scene.gltf"};
    HeapVector<std::future<asset::SceneLoadResult>> requests;
    for (const char* fixture : fixtures)
    {
        requests.emplace_back(loader.LoadAsync(AsyncFixture(fixture)));
    }
    for (size_t index = 0; index < requests.size(); ++index)
    {
        SCOPED_TRACE(fixtures[index]);
        ASSERT_EQ(requests[index].wait_for(std::chrono::seconds(15)), std::future_status::ready);
        asset::SceneLoadResult actual   = requests[index].get();
        asset::SceneLoadResult expected = asset::LoadScene(AsyncFixture(fixtures[index]), &serial);
        ASSERT_TRUE(actual.scene) << actual.error;
        ASSERT_TRUE(expected.scene) << expected.error;
        EXPECT_TRUE(actual.error.empty());
        ExpectGeometryEqual(*actual.scene, *expected.scene);
    }
}

TEST(SceneLoader, OneProcessingWorkerHandlesConcurrentImportsAndIndependentFailures)
{
    asset::SceneLoader                              loader(2, 1);
    HeapVector<std::future<asset::SceneLoadResult>> requests;
    const char* fixtures[]{"invalid_texture_scene.gltf", "complete_scene.gltf", "unsupported_uv_scene.gltf",
                           "texture_roles.gltf",         "missing.gltf",        "all_primitive_modes.gltf"};
    for (const char* fixture : fixtures)
    {
        requests.emplace_back(loader.LoadAsync(AsyncFixture(fixture)));
    }
    for (size_t index = 0; index < requests.size(); ++index)
    {
        SCOPED_TRACE(fixtures[index]);
        ASSERT_EQ(requests[index].wait_for(std::chrono::seconds(15)), std::future_status::ready);
        asset::SceneLoadResult result = requests[index].get();
        EXPECT_EQ(bool(result.scene), index % 2 != 0);
        EXPECT_EQ(result.error.empty(), index % 2 != 0);
    }
}

TEST(SceneLoader, BoundedProcessingPreservesOrderAcrossMeshBoundaries)
{
    const ParallelSceneFixture          fixture;
    asset::SceneLoader                  loader(2, 2);
    ThreadPool<void, uint32_t>          serial(1);
    std::future<asset::SceneLoadResult> first    = loader.LoadAsync(fixture.Path());
    std::future<asset::SceneLoadResult> second   = loader.LoadAsync(fixture.Path());
    asset::SceneLoadResult              expected = asset::LoadScene(fixture.Path(), &serial);
    ASSERT_TRUE(expected.scene) << expected.error;
    for (std::future<asset::SceneLoadResult>* request : {&first, &second})
    {
        ASSERT_EQ(request->wait_for(std::chrono::seconds(15)), std::future_status::ready);
        asset::SceneLoadResult actual = request->get();
        ASSERT_TRUE(actual.scene) << actual.error;
        ExpectGeometryEqual(*actual.scene, *expected.scene);
        const HeapVector<sg::SubMesh*> meshes = actual.scene->scene->GetComponents<sg::SubMesh>();
        ASSERT_EQ(meshes.size(), 51u);
        ASSERT_EQ(actual.scene->vertices.size(), 153u);
        ASSERT_EQ(actual.scene->indices.size(), 153u);
        for (uint32_t index = 0; index < meshes.size(); ++index)
        {
            EXPECT_EQ(meshes[index]->assetMesh, index / 17);
            EXPECT_EQ(meshes[index]->assetPrimitive, index % 17);
            EXPECT_EQ(meshes[index]->GetMaterialIndex(), (index % 17) % 2);
            EXPECT_EQ(meshes[index]->GetFirstIndex(), index * 3);
            EXPECT_EQ(actual.scene->vertices[index * 3].normal, Vec4(0, 0, 1, 0));
        }
        for (uint32_t index = 0; index < actual.scene->indices.size(); ++index)
        {
            EXPECT_EQ(actual.scene->indices[index], index);
        }
    }
}

TEST(SceneLoader, ShutdownDrainsQueuedRequestsAndResultsOutliveTheWorkers)
{
    HeapVector<std::future<asset::SceneLoadResult>> requests;
    {
        asset::SceneLoader loader(1, 1);
        for (uint32_t index = 0; index < 6; ++index)
        {
            requests.emplace_back(loader.LoadAsync(AsyncFixture("complete_scene.gltf")));
        }
        // Dropping a future does not cancel or detach a task that borrows the service.
        loader.LoadAsync(AsyncFixture("texture_roles.gltf"));
    }
    for (std::future<asset::SceneLoadResult>& request : requests)
    {
        EXPECT_EQ(request.wait_for(std::chrono::seconds(0)), std::future_status::ready);
        asset::SceneLoadResult result = request.get();
        ASSERT_TRUE(result.scene) << result.error;
        EXPECT_EQ(result.scene->scene->GetRenderableCount(), 1u);
        const sg::SceneAssetData& data = result.scene->scene->GetAssetData();
        HeapVector<asset::Vertex> posed;
        EXPECT_TRUE(sg::ApplySceneDeformations(
            *result.scene->scene, VectorView<const asset::Vertex>(data.bindVertices.data(), data.bindVertices.size()), posed));
        EXPECT_EQ(posed.size(), result.scene->vertices.size());
    }
}

TEST(SceneImporter, RegistryDispatchAndFailuresUseStructuredResults)
{
    ASSERT_NE(asset::FindSceneImportFormat("example.GLTF"), nullptr);
    EXPECT_EQ(asset::FindSceneImportFormat("example.GLTF")->id, "gltf");
    EXPECT_EQ(asset::FindSceneImportFormat("example.GLB")->id, "gltf");
    EXPECT_EQ(asset::FindSceneImportFormat("example.unsupported"), nullptr);
    EXPECT_FALSE(asset::GetSceneImportFormats().empty());

    asset::SceneLoadResult unsupported = asset::LoadScene("model.unsupported");
    EXPECT_FALSE(unsupported.scene);
    EXPECT_EQ(unsupported.status, asset::SceneImportStatus::UnsupportedFormat);
    ASSERT_EQ(unsupported.diagnostics.size(), 1u);
    EXPECT_EQ(unsupported.diagnostics[0].source, "model.unsupported");
    EXPECT_EQ(unsupported.diagnostics[0].message, unsupported.error);

    asset::SceneLoadResult missing = asset::LoadScene(AsyncFixture("missing.gltf"));
    EXPECT_EQ(missing.status, asset::SceneImportStatus::SourceUnavailable);
    EXPECT_FALSE(missing.scene);
    asset::SceneLoadResult empty = asset::LoadScene(std::string{});
    EXPECT_EQ(empty.status, asset::SceneImportStatus::InvalidRequest);
}

TEST(SceneImporter, ExplicitSourceSceneAndProgressAreRequestLocal)
{
    asset::SceneLoader        loader(2, 2);
    asset::SceneImportRequest request{AsyncFixture("complete_scene.gltf")};
    request.settings.sceneIndex = 0;
    request.control             = SharedPtr<asset::SceneImportControl, MultiThreadCounter>(new asset::SceneImportControl);
    std::future<asset::SceneLoadResult> selected     = loader.LoadAsync(request);
    std::future<asset::SceneLoadResult> normal       = loader.LoadAsync(request.path);
    asset::SceneLoadResult              result       = selected.get();
    asset::SceneLoadResult              defaultScene = normal.get();
    ASSERT_TRUE(result.scene) << result.error;
    ASSERT_TRUE(defaultScene.scene) << defaultScene.error;
    EXPECT_EQ(result.status, asset::SceneImportStatus::Success);
    EXPECT_EQ(result.format, "gltf");
    EXPECT_EQ(request.control->GetStage(), asset::SceneImportStage::Complete);
    ASSERT_EQ(result.scene->scene->GetNodes().size(), 1u);
    EXPECT_EQ(result.scene->scene->GetNodes()[0]->GetIndex(), 6u);
    EXPECT_GT(defaultScene.scene->scene->GetNodes().size(), 1u);

    request.settings.sceneIndex = 200;
    request.control.Reset();
    asset::SceneLoadResult invalid = asset::LoadScene(request);
    EXPECT_FALSE(invalid.scene);
    EXPECT_EQ(invalid.status, asset::SceneImportStatus::InvalidData);
}

TEST(SceneImporter, CancellationBeforeDispatchAndDuringProcessingDoesNotPublishPartialScenes)
{
    const ParallelSceneFixture fixture;
    asset::SceneImportRequest  request{fixture.Path()};
    request.useCache = false;
    request.control  = SharedPtr<asset::SceneImportControl, MultiThreadCounter>(new asset::SceneImportControl);
    request.control->Cancel();
    asset::SceneLoadResult cancelled = asset::LoadScene(request);
    EXPECT_EQ(cancelled.status, asset::SceneImportStatus::Cancelled);
    EXPECT_FALSE(cancelled.scene);
    EXPECT_EQ(request.control->GetStage(), asset::SceneImportStage::Cancelled);

    request.control = SharedPtr<asset::SceneImportControl, MultiThreadCounter>(new asset::SceneImportControl);
    ThreadPool<void, uint32_t> processing(1);
    ThreadPool<void, uint32_t> imports(1);
    std::promise<void>         release;
    std::future<void>          gate = processing.Push(&WaitForSceneGate, release.get_future().share());
    asset::SceneLoadResult     (*load)(const asset::SceneImportRequest&, ThreadPool<void, uint32_t>*) = &asset::LoadScene;
    std::future<asset::SceneLoadResult>         pending  = imports.Push(load, request, &processing);
    const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (request.control->GetStage() != asset::SceneImportStage::ProcessingMeshes
           && pending.wait_for(std::chrono::seconds(0)) != std::future_status::ready
           && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::yield();
    }
    EXPECT_EQ(request.control->GetStage(), asset::SceneImportStage::ProcessingMeshes);
    request.control->Cancel();
    release.set_value();
    gate.get();
    asset::SceneLoadResult result = pending.get();
    EXPECT_FALSE(result.scene);
    EXPECT_EQ(result.status, asset::SceneImportStatus::Cancelled);
    EXPECT_EQ(request.control->GetStage(), asset::SceneImportStage::Cancelled);
    asset::SceneLoadResult next = asset::LoadScene(request.path, &processing);
    EXPECT_TRUE(next.scene) << next.error;
}

TEST(SceneRuntimeBuilder, RuntimeInstancesSharePixelsAndOwnTheirMutableState)
{
    asset::SceneImportResult imported = asset::ImportScene({AsyncFixture("texture_roles.gltf")});
    ASSERT_TRUE(imported.asset) << imported.error;
    ASSERT_FALSE(imported.asset->images.empty());
    ThreadPool<void, uint32_t>          builders(2);
    std::future<asset::SceneLoadResult> first  = builders.Push(&asset::BuildSceneRuntime, imported.asset, nullptr);
    std::future<asset::SceneLoadResult> second = builders.Push(&asset::BuildSceneRuntime, imported.asset, nullptr);
    asset::SceneLoadResult              a      = first.get();
    asset::SceneLoadResult              b      = second.get();
    ASSERT_TRUE(a.scene) << a.error;
    ASSERT_TRUE(b.scene) << b.error;
    ExpectGeometryEqual(*a.scene, *b.scene);
    const HeapVector<sg::Texture*> aTextures = a.scene->scene->GetComponents<sg::Texture>();
    const HeapVector<sg::Texture*> bTextures = b.scene->scene->GetComponents<sg::Texture>();
    ASSERT_GE(imported.asset->textures.size(), 2u);
    for (size_t index = 0; index < imported.asset->textures.size(); ++index)
    {
        const asset::TextureAsset& texture = imported.asset->textures[index];
        EXPECT_EQ(aTextures[index]->pixels.Get(), imported.asset->images[texture.image.value].pixels.Get());
        EXPECT_EQ(aTextures[index]->GetBytes().data(), bTextures[index]->GetBytes().data());
        EXPECT_TRUE(aTextures[index]->bytesData.empty());
    }
    const HeapVector<sg::Material*> aMaterials = a.scene->scene->GetComponents<sg::Material>();
    const HeapVector<sg::Material*> bMaterials = b.scene->scene->GetComponents<sg::Material>();
    ASSERT_FALSE(aMaterials.empty());
    const Vec4 original            = bMaterials[0]->baseColorFactor;
    aMaterials[0]->baseColorFactor = Vec4(0.25f);
    EXPECT_EQ(bMaterials[0]->baseColorFactor, original);
    EXPECT_EQ(imported.asset->materials[0].baseColorFactor, original);
    imported.asset.Reset();
    a.scene.Reset();
    EXPECT_FALSE(bTextures[0]->GetBytes().empty());
}

TEST(SceneRuntimeBuilder, RetainsSourceDefinitionsAndRejectsMalformedInternalReferences)
{
    asset::SceneImportResult imported = asset::ImportScene({AsyncFixture("complete_scene.gltf")});
    ASSERT_TRUE(imported.asset) << imported.error;
    asset::SceneLoadResult loaded = asset::BuildSceneRuntime(imported.asset);
    ASSERT_TRUE(loaded.scene) << loaded.error;
    EXPECT_GT(imported.asset->nodes.size(), loaded.scene->scene->GetNodes().size());
    EXPECT_LT(imported.asset->vertices.size(), loaded.scene->vertices.size());
    ASSERT_FALSE(loaded.scene->scene->GetAssetData().deformations.empty());
    const size_t           vertexCount = imported.asset->vertices.size();
    asset::SceneLoadResult another     = asset::BuildSceneRuntime(imported.asset);
    ASSERT_TRUE(another.scene) << another.error;
    ExpectGeometryEqual(*another.scene, *loaded.scene);
    EXPECT_EQ(imported.asset->vertices.size(), vertexCount);
    ASSERT_FALSE(loaded.scene->vertices.empty());
    const Vec4 position            = loaded.scene->vertices[0].pos;
    const Vec4 bindPosition        = imported.asset->vertices[0].pos;
    another.scene->vertices[0].pos = Vec4(100);
    EXPECT_EQ(loaded.scene->vertices[0].pos, position);
    EXPECT_EQ(imported.asset->vertices[0].pos, bindPosition);


    SharedPtr<asset::SceneAsset, MultiThreadCounter> invalid(new asset::SceneAsset(*imported.asset));
    invalid->nodes[0].children.push_back(asset::NodeId{UINT32_MAX - 1});
    asset::SceneLoadResult rejected = asset::BuildSceneRuntime(invalid);
    EXPECT_FALSE(rejected.scene);
    EXPECT_EQ(rejected.status, asset::SceneImportStatus::InvalidData);
    invalid->nodes           = imported.asset->nodes;
    invalid->nodes[0].parent = asset::NodeId{0};
    invalid->nodes[0].children.push_back(asset::NodeId{0});
    rejected = asset::BuildSceneRuntime(invalid);
    EXPECT_FALSE(rejected.scene);
    invalid->nodes = imported.asset->nodes;
    ASSERT_FALSE(invalid->textures.empty());
    invalid->textures[0].image = asset::ImageId{UINT32_MAX};
    rejected                   = asset::BuildSceneRuntime(invalid);
    EXPECT_FALSE(rejected.scene);
    EXPECT_EQ(rejected.status, asset::SceneImportStatus::InvalidData);
}

TEST(SceneRuntimeBuilder, PlacementIsExplicitAndLeavesTheCanonicalAssetInAuthoredUnits)
{
    asset::SceneImportResult imported = asset::ImportScene({AsyncFixture("complete_scene.gltf")});
    ASSERT_TRUE(imported.asset) << imported.error;
    asset::SceneLoadResult first  = asset::BuildSceneRuntime(imported.asset);
    asset::SceneLoadResult second = asset::BuildSceneRuntime(imported.asset);
    ASSERT_TRUE(first.scene) << first.error;
    ASSERT_TRUE(second.scene) << second.error;
    const sg::AABB original        = first.scene->scene->GetAABB();
    const Vec3     rootTranslation = imported.asset->nodes[0].translation;
    asset::PrepareScenePlacement(*first.scene->scene, asset::ScenePlacement::PreserveAuthored);
    EXPECT_EQ(first.scene->scene->GetAABB(), original);
    EXPECT_FLOAT_EQ(first.scene->scene->GetAssetData().unitScale, 1.0f);
    asset::PrepareScenePlacement(*second.scene->scene, asset::ScenePlacement::CenterAndNormalize);
    EXPECT_NEAR(second.scene->scene->GetAABB().GetMaxExtent(), 1.0f, 1e-5f);
    EXPECT_NEAR(glm::length(second.scene->scene->GetAABB().GetCenter()), 0.0f, 1e-5f);
    EXPECT_EQ(first.scene->scene->GetAABB(), original);
    EXPECT_EQ(imported.asset->nodes[0].translation, rootTranslation);
    EXPECT_FLOAT_EQ(second.scene->scene->GetAssetData().unitScale, 1.0f / original.GetMaxExtent());
}

TEST(SceneRuntimeBuilder, TypedAnimationSurvivesMetadataRemovalAndUpdatesSharedDefinitions)
{
    asset::SceneImportResult imported = asset::ImportScene({AsyncFixture("typed_animation_targets.gltf")});
    ASSERT_TRUE(imported.asset) << imported.error;
    SharedPtr<asset::SceneAsset, MultiThreadCounter> stripped(new asset::SceneAsset(*imported.asset));
    stripped->sourceDocument.clear();
    ASSERT_EQ(stripped->animations.size(), 1u);
    for (asset::AnimationChannel& channel : stripped->animations[0].channels)
    {
        EXPECT_NE(channel.target.property, asset::AnimatedProperty::Invalid);
        channel.sourceTarget.clear();
    }
    imported.asset.Reset();
    asset::SceneLoadResult first  = asset::BuildSceneRuntime(stripped);
    asset::SceneLoadResult second = asset::BuildSceneRuntime(stripped);
    ASSERT_TRUE(first.scene) << first.error;
    ASSERT_TRUE(second.scene) << second.error;
    ASSERT_EQ(first.scene->scene->GetNodes().size(), 2u);
    ASSERT_TRUE(first.scene->scene->GetAssetData().source->document.empty());
    ASSERT_TRUE(sg::EvaluateSceneAnimation(*first.scene->scene, 0, 0.5f, false));
    EXPECT_FLOAT_EQ(first.scene->scene->GetComponents<sg::Material>()[0]->metallicFactor, 0.5f);
    EXPECT_FLOAT_EQ(second.scene->scene->GetComponents<sg::Material>()[0]->metallicFactor, 0.2f);
    EXPECT_FLOAT_EQ(stripped->materials[0].metallicFactor, 0.2f);
    const HeapVector<sg::SceneCamera*> cameras = first.scene->scene->GetComponents<sg::SceneCamera>();
    const HeapVector<sg::Light*>       lights  = first.scene->scene->GetComponents<sg::Light>();
    ASSERT_EQ(cameras.size(), 2u);
    ASSERT_EQ(lights.size(), 2u);
    for (uint32_t index = 0; index < 2; ++index)
    {
        EXPECT_FLOAT_EQ(cameras[index]->farPlane, 0.5f);
        EXPECT_FLOAT_EQ(lights[index]->GetProperties().intensity, 0.5f);
    }
    EXPECT_EQ(stripped->nodes[2].translation, Vec3(0));
}
