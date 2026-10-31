#include "Editor/Model/EditorActions.h"
#include "Editor/Model/EditorLog.h"
#include "Editor/Model/EditorPreferences.h"
#include "Editor/Model/EditorSelection.h"
#include "Editor/Model/ViewAxes.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>

namespace zen::editor
{
namespace
{
std::string Fixture(const char* name)
{
    return std::string(ZEN_EDITOR_FIXTURES) + name;
}

void Open(EditorScene& scene, const char* name)
{
    std::string error;

    UniquePtr<LoadedScene> loaded = ParseScene(Fixture(name), error);

    ASSERT_TRUE(loaded) << error;

    scene.Replace(std::move(loaded));
}

SceneAssetId FindAsset(const HeapVector<SceneAssetItem>& assets, SceneAssetKind kind)
{
    SceneAssetId result;

    for (const SceneAssetItem& item : assets)
    {
        if (item.id.kind == kind && result.generation == 0)
        {
            result = item.id;
        }
    }

    return result;
}

size_t CountAssets(const HeapVector<SceneAssetItem>& assets, SceneAssetKind kind)
{
    size_t count = 0;

    for (const SceneAssetItem& item : assets)
    {
        count += item.id.kind == kind ? 1 : 0;
    }

    return count;
}

std::filesystem::path MakeSettingsDirectory(const char* name)
{
    const std::filesystem::path directory = std::filesystem::temp_directory_path() / "ZenEditorModelTest" / name;

    std::error_code error;

    std::filesystem::remove_all(directory, error);

    std::filesystem::create_directories(directory, error);

    return directory;
}

TEST(EditorModel, DuplicateNamesRemainDistinctAndFilteredHierarchyKeepsAncestors)
{
    EditorScene scene;

    EditorSelection selection(scene);

    Open(scene, "inspection.gltf");

    ASSERT_EQ(scene.GetRoots().size(), 1u);

    EXPECT_EQ(scene.GetChildren(scene.GetRoots()[0]).size(), 4u);

    const HashMap<uint32_t, bool> filtered = scene.FilterHierarchy("DUPLICATE");

    EXPECT_EQ(filtered.size(), 3u);

    EXPECT_EQ(filtered.count(scene.GetRoots()[0].index), 1u);

    const NodeId first{scene.GetGeneration(), 1};

    const NodeId second{scene.GetGeneration(), 2};

    EXPECT_EQ(scene.Inspect(first).name, scene.Inspect(second).name);

    selection.SelectNode(first);

    EXPECT_EQ(selection.GetNode(), first);

    selection.SelectNode(second);

    EXPECT_EQ(selection.GetNode(), second);

    EXPECT_NE(first, second);

    EXPECT_TRUE(scene.Inspect({scene.GetGeneration(), 3}).hasLight);

    EXPECT_TRUE(scene.Inspect({scene.GetGeneration(), 4}).hasCamera);

    EXPECT_EQ(scene.Inspect(first).materials.size(), 1u);
}

TEST(EditorModel, FailedImportPreservesActiveSceneAndReplacementInvalidatesSelection)
{
    EditorScene scene;

    EditorSelection selection(scene);

    Open(scene, "inspection.gltf");

    const NodeId id{scene.GetGeneration(), 1};

    selection.SelectNode(id);

    const LoadedScene* active = scene.Get();

    std::string error;

    EXPECT_FALSE(ParseScene(Fixture("missing.gltf"), error));

    EXPECT_FALSE(error.empty());

    EXPECT_EQ(scene.Get(), active);

    EXPECT_EQ(selection.GetNode(), id);

    // Replacing the scene invalidates the selection before anyone clears it.
    Open(scene, "inspection.gltf");

    EXPECT_EQ(scene.Resolve(id), nullptr);

    EXPECT_EQ(selection.GetNode(), NodeId{});

    EXPECT_FALSE(scene.Inspect(selection.GetNode()).valid);
}

TEST(EditorModel, PickStampsChangeWithCameraTargetSceneAndSelection)
{
    EditorScene scene;

    EditorSelection selection(scene);

    EditorCamera camera;

    Open(scene, "inspection.gltf");

    const PickStamp stamp = MakePickStamp(scene, camera, selection, 10);

    EXPECT_EQ(stamp, MakePickStamp(scene, camera, selection, 10));

    EXPECT_NE(stamp, MakePickStamp(scene, camera, selection, 11));

    CameraInput input;

    input.orbit = Vec2(10, 0);

    camera.Apply(input);

    EXPECT_NE(stamp, MakePickStamp(scene, camera, selection, 10));

    const PickStamp moved = MakePickStamp(scene, camera, selection, 10);

    selection.SelectNode({scene.GetGeneration(), 2});

    EXPECT_NE(moved, MakePickStamp(scene, camera, selection, 10));

    const PickStamp selected = MakePickStamp(scene, camera, selection, 10);

    Open(scene, "inspection.gltf");

    EXPECT_NE(selected, MakePickStamp(scene, camera, selection, 10));
}

TEST(EditorModel, CameraProjectionAndSceneBoundsDoNotNeedAWindow)
{
    EditorScene scene;

    EditorCamera camera;

    Open(scene, "inspection.gltf");

    const NodeId id{scene.GetGeneration(), 1};

    sg::AABB bounds;

    ASSERT_TRUE(scene.GetBounds(id, bounds));

    camera.Frame(bounds);

    camera.SetExtent(800, 600);

    Vec3 origin;

    Vec3 direction;

    camera.MakeRay(Vec2(0.5f), origin, direction);

    EXPECT_NEAR(glm::dot(glm::normalize(bounds.GetCenter() - origin), direction), 1.0f, 0.0001f);

    camera.SetOrthographic(true);

    camera.MakeRay(Vec2(0.5f), origin, direction);

    EXPECT_NEAR(glm::dot(glm::normalize(bounds.GetCenter() - origin), direction), 1.0f, 0.0001f);

    Vec3 firstOrigin;

    Vec3 firstDirection;

    Vec3 secondOrigin;

    Vec3 secondDirection;

    camera.MakeRay(Vec2(0.2f), firstOrigin, firstDirection);

    camera.MakeRay(Vec2(0.8f), secondOrigin, secondDirection);

    EXPECT_LT(glm::length(firstDirection - secondDirection), 0.0001f);

    EXPECT_GT(glm::length(firstOrigin - secondOrigin), 0.1f);

    // The root's bounds include both meshes below it.
    sg::AABB root;

    ASSERT_TRUE(scene.GetBounds(scene.GetRoots()[0], root));

    EXPECT_GT(root.GetMax().x, bounds.GetMax().x);
}

TEST(EditorModel, CameraMoveSpeedStaysConsistentWhenFramingDifferentBounds)
{
    EditorCamera camera;

    CameraInput input;

    input.seconds   = 0.02f;

    input.move      = Vec3(0, 0, 1);

    input.moveSpeed = 0.25f;

    for (bool orthographic : {false, true})
    {
        camera.SetOrthographic(orthographic);

        // Whole scenes and small selections must use the same configured speed.
        for (float extent : {0.002f, 1.0f, 100.0f})
        {
            camera.Frame(sg::AABB(Vec3(-extent), Vec3(extent)));

            const Vec3 start = camera.GetCamera().GetPos();

            camera.Apply(input);

            EXPECT_NEAR(glm::length(camera.GetCamera().GetPos() - start), 0.005f, 0.00002f);
        }
    }
}

TEST(EditorModel, CameraMoveSpeedRespectsElapsedTimeBoostAndDiagonalLimit)
{
    EditorCamera camera;

    CameraInput input;

    input.seconds   = 0.02f;

    input.move      = Vec3(1, 1, 1);

    input.moveSpeed = 2.0f;

    for (bool fast : {false, true})
    {
        input.fast = fast;

        // Including pitched flight with overlapping forward and world-up motion.
        for (Vec3 direction : {Vec3(0, 0, -1), Vec3(0, 1, -1)})
        {
            camera.LookAlong(direction);

            const Vec3 start = camera.GetCamera().GetPos();

            camera.Apply(input);

            EXPECT_NEAR(glm::length(camera.GetCamera().GetPos() - start), fast ? 0.12f : 0.04f, 0.00001f);
        }
    }

    input.fast = false;

    input.move = Vec3(1, 0, 0);

    // A long frame is capped to prevent jumps; zero/negative time does not move.
    for (float seconds : {-1.0f, 0.0f, 0.01f, 0.05f, 1.0f})
    {
        input.seconds    = seconds;

        const Vec3 start = camera.GetCamera().GetPos();

        camera.Apply(input);

        EXPECT_NEAR(glm::length(camera.GetCamera().GetPos() - start), 2.0f * std::clamp(seconds, 0.0f, 0.1f), 0.00001f);
    }
}

TEST(EditorModel, CameraMoveSpeedRejectsNonFiniteAndOutOfRangeValues)
{
    EditorCamera camera;

    CameraInput input;

    input.seconds           = 0.1f;

    input.move              = Vec3(1, 0, 0);

    const float speeds[]    = {-1.0f, 0.0f, 1000.0f, std::numeric_limits<float>::infinity(),
                               std::numeric_limits<float>::quiet_NaN()};

    const float distances[] = {0.0001f, 0.0001f, 10.0f, 0.1f, 0.1f};

    for (uint32_t index = 0; index < 5; ++index)
    {
        input.moveSpeed  = speeds[index];

        const Vec3 start = camera.GetCamera().GetPos();

        camera.Apply(input);

        EXPECT_NEAR(glm::length(camera.GetCamera().GetPos() - start), distances[index], 0.00001f);
    }
}

const ViewAxisEnd& FindEnd(const ViewAxes& axes, uint32_t axis, bool negative)
{
    const ViewAxisEnd* found = &axes.ends[0];

    for (const ViewAxisEnd& end : axes.ends)
    {
        if (end.axis == axis && end.negative == negative)
        {
            found = &end;
        }
    }

    return *found;
}

TEST(EditorModel, ViewAxesFollowTheCameraAndLookAlongKeepsTheOrbitCenter)
{
    EditorCamera camera;

    camera.SetExtent(800, 600);

    camera.Frame(sg::AABB(Vec3(-1, 0, -1), Vec3(1, 2, 1)));

    const Vec3 center(0, 1, 0);

    const float distance = glm::length(camera.GetCamera().GetPos() - center);

    // The default view faces -Z: +X points right, +Y up (screen y grows down), +Z out.
    ViewAxes axes = ProjectViewAxes(camera.GetCamera().GetViewMatrix());

    EXPECT_NEAR(FindEnd(axes, 0, false).offset.x, 1.0f, 1e-4f);

    EXPECT_NEAR(FindEnd(axes, 1, false).offset.y, -1.0f, 1e-4f);

    EXPECT_EQ(axes.ends[5].GetDirection(), Vec3(0, 0, 1));

    EXPECT_EQ(axes.ends[0].GetDirection(), Vec3(0, 0, -1));

    uint64_t revision = camera.GetRevision();

    // Viewing from the +X side looks toward -X, with -Z to the right.
    camera.LookAlong(Vec3(-2, 0, 0));

    EXPECT_GT(camera.GetRevision(), revision);

    Vec3 eye = camera.GetCamera().GetPos();

    EXPECT_NEAR(glm::length(eye - center), distance, distance * 1e-4f);

    EXPECT_NEAR(eye.x - center.x, distance, distance * 1e-4f);

    axes = ProjectViewAxes(camera.GetCamera().GetViewMatrix());

    EXPECT_EQ(axes.ends[5].GetDirection(), Vec3(1, 0, 0));

    EXPECT_NEAR(FindEnd(axes, 2, false).offset.x, -1.0f, 1e-4f);

    // Looking straight down stays within the pitch limit with X right and -Z up.
    camera.LookAlong(Vec3(0, -1, 0));

    eye = camera.GetCamera().GetPos();

    EXPECT_GT(eye.y - center.y, distance * 0.99f);

    EXPECT_NEAR(glm::length(eye - center), distance, distance * 1e-4f);

    axes = ProjectViewAxes(camera.GetCamera().GetViewMatrix());

    EXPECT_EQ(axes.ends[5].GetDirection(), Vec3(0, 1, 0));

    EXPECT_NEAR(FindEnd(axes, 0, false).offset.x, 1.0f, 1e-4f);

    EXPECT_LT(FindEnd(axes, 2, true).offset.y, -0.99f);

    revision = camera.GetRevision();

    camera.LookAlong(Vec3(0.0f));

    EXPECT_EQ(camera.GetRevision(), revision);
}

TEST(EditorModel, OrbitByTurnsTheViewLikeAGrabbedSphereAroundTheOrbitCenter)
{
    EditorCamera camera;

    camera.SetExtent(800, 600);

    camera.Frame(sg::AABB(Vec3(-1, 0, -1), Vec3(1, 2, 1)));

    const Vec3 center(0, 1, 0);

    const float distance = glm::length(camera.GetCamera().GetPos() - center);

    // Dragging right a quarter turn carries the sphere's front (+Z) to its right edge.
    camera.OrbitBy(Vec2(glm::half_pi<float>(), 0.0f));

    Mat4 view = camera.GetCamera().GetViewMatrix();

    EXPECT_NEAR(ToViewAxesSpace(view, Vec3(0, 0, 1)).x, 1.0f, 1e-4f);

    EXPECT_NEAR(glm::length(camera.GetCamera().GetPos() - center), distance, distance * 1e-4f);

    camera.LookAlong(Vec3(0, 0, -1));

    // Dragging down carries the front down and raises the camera.
    camera.OrbitBy(Vec2(0.0f, 0.5f));

    view = camera.GetCamera().GetViewMatrix();

    EXPECT_NEAR(ToViewAxesSpace(view, Vec3(0, 0, 1)).y, std::sin(0.5f), 1e-4f);

    EXPECT_GT(camera.GetCamera().GetPos().y, center.y);

    // Pitch stops short of the pole instead of flipping over it.
    camera.OrbitBy(Vec2(0.0f, 10.0f));

    view = camera.GetCamera().GetViewMatrix();

    EXPECT_LT(ToViewAxesSpace(view, Vec3(0, 1, 0)).z, 1.0f);

    EXPECT_GT(ToViewAxesSpace(view, Vec3(0, 1, 0)).z, 0.99f);

    EXPECT_NEAR(glm::length(camera.GetCamera().GetPos() - center), distance, distance * 1e-4f);

    const uint64_t revision = camera.GetRevision();

    camera.OrbitBy(Vec2(0.0f));

    EXPECT_EQ(camera.GetRevision(), revision);
}

TEST(EditorModel, SceneAssetsShareMeshesAndMaterialsWithoutEnginePlaceholders)
{
    EditorScene scene;

    EditorSelection selection(scene);

    Open(scene, "inspection.gltf");

    const SceneAssetIndex& index            = scene.GetAssets();

    const HeapVector<SceneAssetItem> assets = index.Query("");

    // The loader's placeholder textures and its unused default material are not listed.
    EXPECT_EQ(CountAssets(assets, SceneAssetKind::Mesh), 1u);

    EXPECT_EQ(CountAssets(assets, SceneAssetKind::Material), 1u);

    EXPECT_EQ(CountAssets(assets, SceneAssetKind::Texture), 0u);

    EXPECT_EQ(CountAssets(assets, SceneAssetKind::Animation), 0u);

    const SceneAssetId mesh             = FindAsset(assets, SceneAssetKind::Mesh);

    const SceneAssetId material         = FindAsset(assets, SceneAssetKind::Material);

    const SceneAssetInspection meshData = index.Inspect(mesh);

    ASSERT_TRUE(meshData.valid);

    EXPECT_EQ(meshData.item.users, 2u);

    EXPECT_EQ(meshData.item.triangles, 1u);

    EXPECT_EQ(meshData.nodes.size(), 2u);

    ASSERT_EQ(meshData.materials.size(), 1u);

    EXPECT_EQ(meshData.materials[0], material);

    const SceneAssetInspection materialData = index.Inspect(material);

    ASSERT_EQ(materialData.usedBy.size(), 1u);

    EXPECT_EQ(materialData.usedBy[0], mesh);

    EXPECT_EQ(materialData.nodes.size(), 2u);

    EXPECT_TRUE(materialData.textures.empty());

    EXPECT_TRUE(materialData.doubleSided);

    EXPECT_NEAR(materialData.item.baseColor.x, 0.7f, 1e-5f);

    EXPECT_TRUE(index.Query("no such asset").empty());

    const NodeId node{scene.GetGeneration(), 1};

    selection.SelectNode(node);

    selection.SelectAsset(material);

    EXPECT_EQ(selection.GetAsset(), material);

    EXPECT_EQ(selection.GetNode(), NodeId{});

    selection.SelectNode(node);

    EXPECT_EQ(selection.GetAsset(), SceneAssetId{});

    selection.SelectAsset(material);

    Open(scene, "inspection.gltf");

    EXPECT_EQ(selection.GetAsset(), SceneAssetId{});

    EXPECT_FALSE(scene.GetAssets().Inspect(material).valid);

    EXPECT_TRUE(scene.GetAssets().Describe(material).name.empty());

    selection.SelectAsset(material);

    EXPECT_EQ(selection.GetAsset(), SceneAssetId{});
}

TEST(EditorModel, MeshAssetsResolveFromNodesAndBoundTheirVertices)
{
    EditorScene scene;

    Open(scene, "inspection.gltf");

    const SceneAssetId mesh = FindAsset(scene.GetAssets().Query(""), SceneAssetKind::Mesh);

    const sg::Mesh* source  = scene.GetAssets().ResolveMesh(mesh);

    ASSERT_NE(source, nullptr);

    EXPECT_EQ(scene.GetAssets().FindMesh(source), mesh);

    EXPECT_EQ(scene.GetAssets().FindMesh(nullptr), SceneAssetId{});

    // Both "Duplicate" nodes instance the same mesh asset; the lamp has none.
    EXPECT_EQ(scene.Inspect({scene.GetGeneration(), 1}).meshAsset, mesh);

    EXPECT_EQ(scene.Inspect({scene.GetGeneration(), 2}).meshAsset, mesh);

    EXPECT_EQ(scene.Inspect({scene.GetGeneration(), 3}).meshAsset, SceneAssetId{});

    // The fixture's triangle, in its own vertex space: node translations do not apply.
    sg::AABB bounds;

    ASSERT_TRUE(scene.GetMeshBounds(mesh, bounds));

    EXPECT_EQ(bounds.GetMin(), Vec3(-1.0f, -1.0f, 0.0f));

    EXPECT_EQ(bounds.GetMax(), Vec3(1.0f, 1.0f, 0.0f));

    EXPECT_FALSE(scene.GetMeshBounds({scene.GetGeneration(), SceneAssetKind::Material, 0}, bounds));

    Open(scene, "inspection.gltf");

    EXPECT_EQ(scene.GetAssets().ResolveMesh(mesh), nullptr);

    EXPECT_FALSE(scene.GetMeshBounds(mesh, bounds));
}

TEST(EditorModel, TextureIdentityUsesImporterMetadataInsteadOfDisplayNames)
{
    sg::Scene scene;

    UniquePtr<sg::Texture> color =
        MakeUnique<sg::Texture>("albedo", 7, 1, 1, asset::Format::R8G8B8A8_SRGB, std::vector<uint8_t>{255, 0, 0, 255});

    UniquePtr<sg::Texture> authored =
        MakeUnique<sg::Texture>("albedo_linear", 8, 1, 1, asset::Format::R8G8B8A8_UNORM, std::vector<uint8_t>{0, 255, 0, 255});

    UniquePtr<sg::Texture> generated =
        MakeUnique<sg::Texture>("An unrelated display name", 9, 1, 1, asset::Format::R8G8B8A8_UNORM,
                                std::vector<uint8_t>(color->GetBytes().begin(), color->GetBytes().end()));

    generated->linearSourceIndex          = color->index;

    UniquePtr<sg::Material> material      = MakeUnique<sg::Material>("Material");

    material->m_pBaseColorTexture         = color.Get();

    material->m_pMetallicRoughnessTexture = generated.Get();

    material->m_pNormalTexture            = authored.Get();

    const sg::Texture* expectedColor      = color.Get();

    const sg::Texture* expectedAuthored   = authored.Get();

    scene.AddComponent(std::move(color));

    scene.AddComponent(std::move(authored));

    scene.AddComponent(std::move(generated));

    scene.AddComponent(std::move(material));

    SceneAssetIndex index;

    index.Build(&scene, 1);

    const HeapVector<SceneAssetItem> assets = index.Query("");

    EXPECT_EQ(CountAssets(assets, SceneAssetKind::Texture), 2u);

    const SceneAssetInspection inspection = index.Inspect(FindAsset(assets, SceneAssetKind::Material));

    ASSERT_EQ(inspection.textures.size(), 3u);

    EXPECT_EQ(inspection.textures[0].texture, inspection.textures[1].texture);

    EXPECT_NE(inspection.textures[0].texture, inspection.textures[2].texture);

    EXPECT_EQ(index.ResolveTexture(inspection.textures[0].texture), expectedColor);

    EXPECT_EQ(index.ResolveTexture(inspection.textures[2].texture), expectedAuthored);
}

TEST(EditorModel, TexturesFoldLinearCopiesAndAnimationsListTheirTargets)
{
    EditorScene scene;

    Open(scene, "textured.gltf");

    const SceneAssetIndex& index            = scene.GetAssets();

    const HeapVector<SceneAssetItem> assets = index.Query("");

    // The image is sampled as sRGB color and as linear data; the loader's linear copy is folded in.
    ASSERT_EQ(CountAssets(assets, SceneAssetKind::Texture), 1u);

    ASSERT_EQ(CountAssets(assets, SceneAssetKind::Animation), 1u);

    const SceneAssetId texture             = FindAsset(assets, SceneAssetKind::Texture);

    const SceneAssetId material            = FindAsset(assets, SceneAssetKind::Material);

    const SceneAssetInspection textureData = index.Inspect(texture);

    EXPECT_EQ(textureData.item.width, 4u);

    EXPECT_EQ(textureData.item.height, 2u);

    EXPECT_EQ(textureData.item.format, asset::Format::R8G8B8A8_SRGB);

    EXPECT_EQ(textureData.mipLevels, 3u);

    EXPECT_EQ(textureData.bytes, 32u);

    ASSERT_EQ(textureData.usedBy.size(), 1u);

    EXPECT_EQ(textureData.usedBy[0], material);

    const SceneAssetInspection materialData = index.Inspect(material);

    EXPECT_EQ(materialData.item.baseColorTexture, texture);

    ASSERT_EQ(materialData.textures.size(), 2u);

    EXPECT_STREQ(materialData.textures[0].slot, "Base Color");

    EXPECT_STREQ(materialData.textures[1].slot, "Metallic Roughness");

    EXPECT_EQ(materialData.textures[0].texture, texture);

    EXPECT_EQ(materialData.textures[1].texture, texture);

    const SceneAssetInspection animation = index.Inspect(FindAsset(assets, SceneAssetKind::Animation));

    EXPECT_EQ(animation.item.name, "Rise");

    EXPECT_FLOAT_EQ(animation.item.duration, 2.0f);

    EXPECT_EQ(animation.item.channels, 1u);

    ASSERT_EQ(animation.nodes.size(), 1u);

    ASSERT_NE(scene.Resolve(animation.nodes[0]), nullptr);

    EXPECT_EQ(scene.Resolve(animation.nodes[0])->GetName(), "Panel");

    EXPECT_EQ(index.Query("rise").size(), 1u);

    const sg::Texture* source = index.ResolveTexture(texture);

    ASSERT_NE(source, nullptr);

    EXPECT_EQ(index.ResolveTexture(material), nullptr);

    TexturePreview preview;

    ASSERT_TRUE(MakeTexturePreview(*source, 2, preview));

    ASSERT_EQ(preview.width, 2u);

    ASSERT_EQ(preview.height, 1u);

    // Each preview texel averages the 2x2 source block it covers, in the stored encoding.
    const uint8_t expected[] = {96, 96, 32, 255, 128, 64, 191, 255};

    EXPECT_TRUE(std::equal(preview.pixels.begin(), preview.pixels.end(), std::begin(expected)));

    ASSERT_TRUE(MakeTexturePreview(*source, 128, preview));

    EXPECT_EQ(preview.width, 4u);

    EXPECT_EQ(preview.height, 2u);

    EXPECT_EQ(preview.pixels[4 * 5], 128);

    sg::Texture compressed("Compressed", 0, 4, 4, asset::Format::BC7_SRGB_BLOCK, std::vector<uint8_t>(16));

    EXPECT_FALSE(MakeTexturePreview(compressed, 128, preview));

    EXPECT_TRUE(preview.pixels.empty());
}

TEST(EditorModel, SpatialPreviewUsesLogicalDimensionsTexelsAndMutableOverrides)
{
    SharedPtr<asset::ImagePixels, MultiThreadCounter> pixels = MakeShared<asset::ImagePixels, MultiThreadCounter>();
    pixels->width                                            = 32;
    pixels->height                                           = 16;
    pixels->bytes                                            = HeapVector<uint8_t>(32 * 16 * 4, 255);
    sg::Texture reference;
    reference.width  = 8;
    reference.height = 4;
    reference.format = asset::Format::R8G8B8A8_SRGB;
    reference.bytesData.resize(8 * 4 * 4);
    for (uint32_t y = 0; y < 4; ++y)
    {
        for (uint32_t x = 0; x < 8; ++x)
        {
            const uint8_t color[]{uint8_t(10 + x * 10), uint8_t(20 + y * 20), 30, 0};
            std::copy(std::begin(color), std::end(color), reference.bytesData.begin() + (y * 8 + x) * 4);
            std::copy(std::begin(color), std::end(color), pixels->bytes.begin() + ((y + 4) * 32 + x + 4) * 4);
        }
    }
    pixels->mips.push_back(pixels->bytes);
    UniquePtr<sg::Texture> owner   = MakeUnique<sg::Texture>("Spatial tile");
    sg::Texture&           texture = *owner;
    texture.width                  = 32;
    texture.height                 = 16;
    texture.pixels                 = pixels;
    texture.atlasRegion            = {4, 4, 8, 4};
    for (asset::Format format : {asset::Format::R8G8B8A8_UNORM, asset::Format::R8G8B8A8_SRGB})
    {
        texture.format = reference.format = format;
        for (uint32_t bound : {1u, 2u, 4u, 8u, 32u})
        {
            TexturePreview preview;
            TexturePreview expected;
            ASSERT_TRUE(MakeTexturePreview(texture, bound, preview));
            ASSERT_TRUE(MakeTexturePreview(reference, bound, expected));
            EXPECT_EQ(preview.width, expected.width);
            EXPECT_EQ(preview.height, expected.height);
            EXPECT_TRUE(
                std::equal(preview.pixels.begin(), preview.pixels.end(), expected.pixels.begin(), expected.pixels.end()));
        }
    }
    sg::Texture other;
    other.width       = texture.width;
    other.height      = texture.height;
    other.format      = texture.format;
    other.pixels      = texture.pixels;
    other.atlasRegion = texture.atlasRegion;
    EXPECT_EQ(texture, other);
    other.atlasRegion = {16, 4, 8, 4};
    EXPECT_NE(texture, other);
    texture.bytesData = reference.bytesData;
    texture.width     = 8;
    texture.height    = 4;
    EXPECT_EQ(texture.GetAtlasRegion(), asset::ImageRegion{});
    TexturePreview local;
    ASSERT_TRUE(MakeTexturePreview(texture, 8, local));
    EXPECT_EQ(local.width, 8u);
    EXPECT_EQ(local.height, 4u);
    EXPECT_EQ(local.pixels[0], 10u);
    asset::TextureInfo info;
    info.width = info.height = 1;
    info.format              = asset::Format::R8G8B8A8_UNORM;
    info.data                = {40, 50, 60, 255};
    texture.Init(0, info);
    EXPECT_EQ(texture.atlasRegion, asset::ImageRegion{});
    EXPECT_FALSE(texture.pixels);
    texture.bytesData.clear();
    texture.width       = 32;
    texture.height      = 16;
    texture.pixels      = pixels;
    texture.atlasRegion = {4, 4, 8, 4};
    sg::Scene scene;
    scene.AddComponent(std::move(owner));
    SceneAssetIndex index;
    index.Build(&scene, 1);
    const HeapVector<SceneAssetItem> assets     = index.Query("");
    const SceneAssetId               id         = FindAsset(assets, SceneAssetKind::Texture);
    const SceneAssetInspection       inspection = index.Inspect(id);
    ASSERT_TRUE(inspection.valid);
    EXPECT_EQ(inspection.item.width, 8u);
    EXPECT_EQ(inspection.item.height, 4u);
    EXPECT_EQ(inspection.mipLevels, 1u);
    EXPECT_EQ(inspection.bytes, 32u * 16u * 4u); // Shared physical storage, not a copied tile.
}

TEST(EditorModel, SpatialPreviewRejectsInvalidRegionsAndPhysicalStorage)
{
    SharedPtr<asset::ImagePixels, MultiThreadCounter> pixels = MakeShared<asset::ImagePixels, MultiThreadCounter>();
    pixels->width = pixels->height = 16;
    pixels->bytes                  = HeapVector<uint8_t>(16 * 16 * 4, 127);
    pixels->mips.push_back(pixels->bytes);
    sg::Texture texture;
    texture.width = texture.height = 16;
    texture.format                 = asset::Format::R8G8B8A8_UNORM;
    texture.pixels                 = pixels;
    TexturePreview preview;
    for (const asset::ImageRegion region :
         {asset::ImageRegion{0, 0, 0, 4}, {UINT32_MAX, 4, 4, 4}, {4, 4, UINT32_MAX, 4}, {15, 4, 4, 4}})
    {
        texture.atlasRegion = region;
        EXPECT_FALSE(MakeTexturePreview(texture, 4, preview));
        EXPECT_TRUE(preview.pixels.empty());
    }
    texture.atlasRegion = {4, 4, 4, 4};
    EXPECT_FALSE(MakeTexturePreview(texture, 0, preview));
    pixels->mips[0].pop_back();
    EXPECT_FALSE(MakeTexturePreview(texture, 4, preview));
    pixels->mips[0] = pixels->bytes;
    pixels->mips.emplace_back(8 * 8 * 4, 127);
    EXPECT_FALSE(MakeTexturePreview(texture, 4, preview));
    EXPECT_TRUE(preview.pixels.empty());
    pixels->mips.clear();
    EXPECT_FALSE(MakeTexturePreview(texture, 4, preview));
}

TEST(EditorModel, ArrayTexturePreviewsSelectLogicalLayersAndMutableOverrides)
{
    SharedPtr<asset::ImagePixels, MultiThreadCounter> pixels = MakeShared<asset::ImagePixels, MultiThreadCounter>();
    pixels->width = pixels->height = 2;
    pixels->arrayLayers            = 2;
    pixels->bytes                  = HeapVector<uint8_t>(32, 40);
    std::fill(pixels->bytes.begin() + 16, pixels->bytes.end(), 180);
    pixels->mips.push_back(pixels->bytes);
    sg::Texture texture;
    texture.width = texture.height = 2;
    texture.format                 = asset::Format::R8G8B8A8_UNORM;
    texture.pixels                 = pixels;
    TexturePreview preview;
    for (uint32_t layer = 0; layer < 2; ++layer)
    {
        texture.arrayLayer = layer;
        ASSERT_TRUE(MakeTexturePreview(texture, 1, preview));
        ASSERT_EQ(preview.pixels.size(), 4u);
        EXPECT_EQ(preview.pixels[0], layer == 0 ? 40 : 180);
    }
    texture.arrayLayer = 2;
    EXPECT_FALSE(MakeTexturePreview(texture, 1, preview));
    EXPECT_TRUE(preview.pixels.empty());
    texture.arrayLayer = 1;
    pixels->mips[0].pop_back();
    EXPECT_FALSE(MakeTexturePreview(texture, 1, preview));
    texture.bytesData.resize(16, 70);
    ASSERT_TRUE(MakeTexturePreview(texture, 1, preview));
    EXPECT_EQ(preview.pixels[0], 70);
    EXPECT_EQ(texture.GetArrayLayers(), 1u);
    EXPECT_EQ(texture.GetArrayLayer(), 0u);
}

TEST(EditorModel, BC7ArrayPreviewSelectsTheLayerWithinTheChosenMip)
{
    const uint8_t red[]   = {0x40, 0x32, 0x99, 0x42, 0x51, 0x28, 0x80, 0x40, 0, 0, 0, 0, 0, 0, 0, 0};
    const uint8_t green[] = {0x40, 0x85, 0x42, 0xab, 0xf5, 0x78, 0x40, 0x20, 0, 0, 0, 0, 0, 0, 0, 0};
    SharedPtr<asset::ImagePixels, MultiThreadCounter> pixels = MakeShared<asset::ImagePixels, MultiThreadCounter>();
    pixels->width                                            = 5;
    pixels->height                                           = 7;
    pixels->format                                           = asset::Format::BC7_SRGB_BLOCK;
    pixels->arrayLayers                                      = 2;
    pixels->mips.emplace_back(128, 0); // Invalid unsampled base blocks on both layers.
    pixels->mips.emplace_back(red, std::size(red));
    pixels->mips.back().push_back(VectorView<const uint8_t>(green, std::size(green)));
    pixels->bytes = pixels->mips[0];
    sg::Texture texture;
    texture.width  = 5;
    texture.height = 7;
    texture.format = pixels->format;
    texture.pixels = pixels;
    TexturePreview preview;
    for (uint32_t layer = 0; layer < 2; ++layer)
    {
        texture.arrayLayer = layer;
        ASSERT_TRUE(MakeTexturePreview(texture, 3, preview));
        EXPECT_EQ(preview.width, 2u);
        EXPECT_EQ(preview.height, 3u);
        EXPECT_EQ(preview.pixels[0], layer == 0 ? 200 : 20);
        EXPECT_EQ(preview.pixels[1], layer == 0 ? 40 : 180);
        EXPECT_FALSE(MakeTexturePreview(texture, 7, preview));
    }
    texture.arrayLayer = 2;
    EXPECT_FALSE(MakeTexturePreview(texture, 3, preview));
}

TEST(EditorModel, Bc7TexturePreviewSelectsSuppliedMipsAndPreservesStoredColor)
{
    // Mode 6 constant blocks with even endpoint values and zero indices.
    const uint8_t red[]   = {0x40, 0x32, 0x99, 0x42, 0x51, 0x28, 0x80, 0x40, 0, 0, 0, 0, 0, 0, 0, 0};
    const uint8_t green[] = {0x40, 0x85, 0x42, 0xab, 0xf5, 0x78, 0x40, 0x20, 0, 0, 0, 0, 0, 0, 0, 0};
    sg::Texture   texture;
    texture.width  = 5;
    texture.height = 7;
    texture.format = asset::Format::BC7_SRGB_BLOCK;
    texture.mipBytes.emplace_back(64, 0);                   // Invalid unsampled base blocks.
    texture.mipBytes.emplace_back(red, std::size(red));     // 2x3.
    texture.mipBytes.emplace_back(green, std::size(green)); // 1x1.
    TexturePreview preview;
    ASSERT_TRUE(MakeTexturePreview(texture, 3, preview));
    EXPECT_EQ(preview.width, 2u);
    EXPECT_EQ(preview.height, 3u);
    ASSERT_EQ(preview.pixels.size(), 24u);
    for (size_t offset = 0; offset < preview.pixels.size(); offset += 4)
    {
        EXPECT_EQ(preview.pixels[offset], 200u);
        EXPECT_EQ(preview.pixels[offset + 1], 40u);
        EXPECT_EQ(preview.pixels[offset + 2], 20u);
        EXPECT_EQ(preview.pixels[offset + 3], 255u);
    }
    ASSERT_TRUE(MakeTexturePreview(texture, 1, preview));
    EXPECT_EQ(preview.width, 1u);
    EXPECT_EQ(preview.height, 1u);
    EXPECT_EQ(preview.pixels[0], 20u);
    EXPECT_EQ(preview.pixels[1], 180u);
    EXPECT_EQ(preview.pixels[2], 60u);
    EXPECT_EQ(preview.pixels[3], 255u);
    EXPECT_FALSE(MakeTexturePreview(texture, 7, preview));
    EXPECT_TRUE(preview.pixels.empty());
    EXPECT_FALSE(MakeTexturePreview(texture, 0, preview));
    EXPECT_TRUE(preview.pixels.empty());
    texture.width = 0;
    EXPECT_FALSE(MakeTexturePreview(texture, 3, preview));
    EXPECT_TRUE(preview.pixels.empty());
    texture.width = 5;
    texture.mipBytes[1].pop_back();
    EXPECT_FALSE(MakeTexturePreview(texture, 3, preview));
    EXPECT_TRUE(preview.pixels.empty());
    texture.mipBytes.clear();
    texture.width  = 1;
    texture.height = 1;
    texture.format = asset::Format::BC7_UNORM_BLOCK;
    texture.bytesData.assign(std::begin(red), std::end(red));
    ASSERT_TRUE(MakeTexturePreview(texture, 128, preview));
    EXPECT_EQ(preview.width, 1u);
    EXPECT_EQ(preview.height, 1u);
    EXPECT_EQ(preview.pixels[0], 200u);
    EXPECT_EQ(preview.pixels[3], 255u);
    texture.format = asset::Format::BC5_UNORM_BLOCK;
    EXPECT_FALSE(MakeTexturePreview(texture, 128, preview));
    EXPECT_TRUE(preview.pixels.empty());
}

TEST(EditorModel, BC4AndMarkedBC5PreviewsUseSuppliedMipsAndRejectInterpretationMismatch)
{
    for (const bool normal : {false, true})
    {
        sg::Texture texture;
        texture.width  = 5;
        texture.height = 7;
        texture.format = normal ? asset::Format::BC5_UNORM_BLOCK : asset::Format::BC4_UNORM_BLOCK;
        texture.interpretation =
            normal ? asset::TextureInterpretation::NormalXYPositiveZ : asset::TextureInterpretation::Default;
        const uint32_t      blockBytes = normal ? 16u : 8u;
        HeapVector<uint8_t> block(blockBytes, 0);
        block[0] = block[1] = 128;
        if (normal)
        {
            block[8] = block[9] = 128;
        }
        texture.mipBytes.emplace_back(4 * blockBytes - 1, 0); // Unused malformed base.
        texture.mipBytes.push_back(block);
        texture.mipBytes.push_back(block);
        TexturePreview preview;
        ASSERT_TRUE(MakeTexturePreview(texture, 3, preview));
        EXPECT_EQ(preview.width, 2u);
        EXPECT_EQ(preview.height, 3u);
        for (size_t offset = 0; offset < preview.pixels.size(); offset += 4)
        {
            EXPECT_EQ(preview.pixels[offset], 128u);
            EXPECT_EQ(preview.pixels[offset + 1], 128u);
            EXPECT_EQ(preview.pixels[offset + 2], normal ? 255u : 128u);
            EXPECT_EQ(preview.pixels[offset + 3], 255u);
        }
        ASSERT_TRUE(MakeTexturePreview(texture, 1, preview));
        EXPECT_EQ(preview.pixels.size(), 4u);
        EXPECT_FALSE(MakeTexturePreview(texture, 7, preview));
        EXPECT_TRUE(preview.pixels.empty());
        texture.mipBytes[2].pop_back();
        EXPECT_FALSE(MakeTexturePreview(texture, 1, preview));
        texture.mipBytes[2] = block;
        texture.interpretation =
            normal ? asset::TextureInterpretation::Default : asset::TextureInterpretation::NormalXYPositiveZ;
        EXPECT_FALSE(MakeTexturePreview(texture, 1, preview));
        texture.interpretation = static_cast<asset::TextureInterpretation>(99);
        EXPECT_FALSE(MakeTexturePreview(texture, 1, preview));
    }
}

TEST(EditorModel, RecentFilesAreNormalizedDeduplicatedAndBounded)
{
    RecentFiles recent;

    recent.Add(Fixture("inspection.gltf"));

    recent.Add(Fixture("textured.gltf"));

    recent.Add(Fixture("inspection.gltf"));

    ASSERT_EQ(recent.Get().size(), 2u);

    EXPECT_TRUE(recent.Get()[0].ends_with("/Fixtures/inspection.gltf")) << recent.Get()[0];

    EXPECT_TRUE(recent.Get()[1].ends_with("/Fixtures/textured.gltf")) << recent.Get()[1];

    HeapVector<std::string> stored;

    stored.push_back("");

    for (int index = 0; index < 12; ++index)
    {
        stored.push_back("scene" + std::to_string(index) + ".glb");

        stored.push_back("scene0.glb");
    }

    recent.Set(stored);

    ASSERT_EQ(recent.Get().size(), kMaxEditorRecentFiles);

    EXPECT_EQ(recent.Get()[0], "scene0.glb");

    EXPECT_EQ(recent.Get()[1], "scene1.glb");

    recent.Clear();

    EXPECT_TRUE(recent.Get().empty());
}

TEST(EditorModel, PreferencesRoundTripByPanelIdAndMigrateEarlierVersions)
{
    const std::filesystem::path directory = MakeSettingsDirectory("RoundTrip");

    EditorPreferences preferences;

    EXPECT_FALSE(LoadEditorPreferences(directory, preferences));

    preferences.panels["Assets"]      = false;

    preferences.panels["Custom.Tool"] = true;

    HeapVector<std::string> recent;

    recent.push_back("C:/Scenes/a b.gltf");

    recent.push_back("D:/\"quoted\".glb");

    preferences.recentFiles.Set(recent);

    ASSERT_TRUE(SaveEditorPreferences(directory, preferences));

    EditorPreferences loaded;

    ASSERT_TRUE(LoadEditorPreferences(directory, loaded));

    EXPECT_EQ(loaded.panels, preferences.panels);

    const HeapVector<std::string>& written = preferences.recentFiles.Get();

    const HeapVector<std::string>& read    = loaded.recentFiles.Get();

    EXPECT_TRUE(std::equal(read.begin(), read.end(), written.begin(), written.end()));

    EXPECT_TRUE(loaded.showSceneControls);

    preferences.showSceneControls = false;

    preferences.cameraMoveSpeed   = 0.1234567f;

    ASSERT_TRUE(SaveEditorPreferences(directory, preferences));

    ASSERT_TRUE(LoadEditorPreferences(directory, loaded));

    EXPECT_FALSE(loaded.showSceneControls);

    EXPECT_FLOAT_EQ(loaded.cameraMoveSpeed, preferences.cameraMoveSpeed);

    {
        // Options from newer builds are skipped; known ones still apply.
        std::ofstream future(directory / "preferences-v3.txt", std::ios::trunc);

        future << "ZenEditorPreferences3\noption \"future.option\" 1\noption \"scene.controls_hint\" 0\n";
    }

    EditorPreferences forward;

    ASSERT_TRUE(LoadEditorPreferences(directory, forward));

    EXPECT_FALSE(forward.showSceneControls);

    EXPECT_FLOAT_EQ(forward.cameraMoveSpeed, kDefaultEditorCameraMoveSpeed);

    {
        std::ofstream corrupt(directory / "preferences-v3.txt", std::ios::trunc);

        corrupt << "ZenEditorPreferences3\npanel \"Assets\" maybe\n";
    }

    EditorPreferences unchanged;

    unchanged.panels["Output"] = false;

    EXPECT_FALSE(LoadEditorPreferences(directory, unchanged));

    EXPECT_EQ(unchanged.panels.size(), 1u);

    const std::filesystem::path legacy = MakeSettingsDirectory("Legacy");

    {
        std::ofstream first(legacy / "preferences-v1.txt");

        first << "ZenEditorPreferences1 5 \"D:/Models\"\n";
    }

    EditorPreferences migrated;

    ASSERT_TRUE(LoadEditorPreferences(legacy, migrated));

    EXPECT_TRUE(migrated.panels.at("Hierarchy"));

    EXPECT_FALSE(migrated.panels.at("SceneViewport"));

    EXPECT_TRUE(migrated.panels.at("Inspector"));

    EXPECT_FALSE(migrated.panels.at("Output"));

    EXPECT_TRUE(migrated.recentFiles.Get().empty());

    EXPECT_FLOAT_EQ(migrated.cameraMoveSpeed, kDefaultEditorCameraMoveSpeed);

    {
        std::ofstream second(legacy / "preferences-v2.txt");

        second << "ZenEditorPreferences2 63 1\n\"D:/Models/Sponza.gltf\"\n";
    }

    ASSERT_TRUE(LoadEditorPreferences(legacy, migrated));

    EXPECT_TRUE(migrated.panels.at("Output"));

    ASSERT_EQ(migrated.recentFiles.Get().size(), 1u);

    EXPECT_EQ(migrated.recentFiles.Get()[0], "D:/Models/Sponza.gltf");
}

TEST(EditorModel, CameraSpeedPreferencesRecoverInvalidValuesAndPreserveOtherOptions)
{
    const std::filesystem::path directory = MakeSettingsDirectory("CameraSpeed");

    const char* values[]                  = {"-1", "0", "1000", "nan", "inf", "invalid", "1e1000", "0.001", "100"};

    const float expected[]                = {0.001f, 0.001f, 100.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.001f, 100.0f};

    for (uint32_t index = 0; index < 9; ++index)
    {
        {
            std::ofstream file(directory / "preferences-v3.txt", std::ios::trunc);

            file << "ZenEditorPreferences3\nnumber \"camera.move_speed\" " << values[index]
                 << "\nnumber \"future.option\" ignored\noption \"scene.controls_hint\" 0\n";
        }

        EditorPreferences preferences;

        ASSERT_TRUE(LoadEditorPreferences(directory, preferences));

        EXPECT_FLOAT_EQ(preferences.cameraMoveSpeed, expected[index]) << values[index];

        EXPECT_FALSE(preferences.showSceneControls);
    }

    EditorPreferences preferences;

    preferences.cameraMoveSpeed = std::numeric_limits<float>::quiet_NaN();

    ASSERT_TRUE(SaveEditorPreferences(directory, preferences));

    EditorPreferences loaded;

    ASSERT_TRUE(LoadEditorPreferences(directory, loaded));

    EXPECT_FLOAT_EQ(loaded.cameraMoveSpeed, kDefaultEditorCameraMoveSpeed);
}

TEST(EditorModel, ActionsDispatchByIdAndShortcutRespectingEnabledState)
{
    EditorActions registry;

    int opened             = 0;

    int framed             = 0;

    bool canFrame          = false;

    const uint16_t control = uint16_t(platform::KeyModifier::Control);

    registry.Register({actions::Open, "Open...", {platform::Key::O, control}, "", nullptr, [&opened]() { ++opened; }});

    registry.Register({actions::FrameAll,
                       "Frame All",
                       {platform::Key::Home, 0},
                       "Open a scene first.",
                       [&canFrame]() { return canFrame; },
                       [&framed]() { ++framed; }});

    EXPECT_TRUE(registry.Execute(actions::Open));

    EXPECT_FALSE(registry.Execute("missing.action"));

    EXPECT_FALSE(registry.Execute(actions::FrameAll));

    EXPECT_FALSE(registry.ExecuteShortcut({platform::Key::Home, 0}));

    canFrame = true;

    EXPECT_TRUE(registry.ExecuteShortcut({platform::Key::Home, 0}));

    // Exact modifiers: Ctrl+Home is not Home, and lock keys are ignored.
    EXPECT_FALSE(registry.ExecuteShortcut({platform::Key::Home, control}));

    EXPECT_TRUE(registry.ExecuteShortcut({platform::Key::O, uint16_t(control | uint16_t(platform::KeyModifier::NumLock))}));

    EXPECT_EQ(opened, 2);

    EXPECT_EQ(framed, 1);

    EXPECT_EQ(FormatShortcut(registry.Find(actions::Open)->shortcut), "Ctrl+O");

    EXPECT_EQ(FormatShortcut({platform::Key::F5, uint16_t(platform::KeyModifier::Shift)}), "Shift+F5");

    EXPECT_EQ(FormatShortcut({}), "");
}

TEST(EditorModel, ShortcutLabelsFollowPlatformModifierNames)
{
    const uint16_t super = uint16_t(platform::KeyModifier::Super);

    const uint16_t alt   = uint16_t(platform::KeyModifier::Alt);

#if defined(ZEN_MACOS)
    EXPECT_EQ(FormatShortcut({platform::Key::O, super}), "Cmd+O");

    EXPECT_EQ(FormatShortcut({platform::Key::Left, alt}), "Option+Left");

    EXPECT_EQ(FormatShortcut({platform::Key::Z, uint16_t(super | uint16_t(platform::KeyModifier::Shift))}), "Shift+Cmd+Z");
#else
    EXPECT_EQ(FormatShortcut({platform::Key::O, super}), "Super+O");

    EXPECT_EQ(FormatShortcut({platform::Key::Left, alt}), "Alt+Left");

    EXPECT_EQ(FormatShortcut({platform::Key::Z, uint16_t(super | uint16_t(platform::KeyModifier::Shift))}), "Shift+Super+Z");
#endif
}

void RegisterDuplicateAction()
{
    EditorActions registry;

    registry.Register({"same", "Same", {}, "", nullptr, []() {}});

    registry.Register({"same", "Same", {}, "", nullptr, []() {}});
}

TEST(EditorModelDeathTest, DuplicateActionIdsAbort)
{
    EXPECT_DEATH(RegisterDuplicateAction(), "registered twice");
}

TEST(EditorModel, EmptySceneAndBoundedFilteredLogs)
{
    EditorScene scene;

    EditorSelection selection(scene);

    Open(scene, "empty.gltf");

    EXPECT_FALSE(scene.Inspect(selection.GetNode()).valid);

    EditorLog log;

    const uint64_t empty = log.GetRevision();

    for (int index = 0; index < 2100; ++index)
    {
        log.Append(index == 2099 ? 4 : 2, index == 2099 ? "Final ERROR" : "old entry");
    }

    EXPECT_EQ(log.Query(0, "").size(), 2048u);

    EXPECT_EQ(log.Query(4, "error").size(), 1u);

    // Views reuse a query until the revision changes; queries alone do not change it.
    const uint64_t filled = log.GetRevision();

    EXPECT_NE(filled, empty);

    EXPECT_EQ(log.GetRevision(), filled);

    log.Clear();

    EXPECT_TRUE(log.Query(0, "").empty());

    EXPECT_NE(log.GetRevision(), filled);
}
TEST(EditorLog, IndependentViewCopiesOnlyNewEntriesAndKeepsClearLocal)
{
    EditorLog                  log;
    HeapVector<EditorLogEntry> view;
    uint64_t                   cursor = 0;
    log.Append(2, "first");
    log.AppendSince(cursor, view);
    ASSERT_EQ(view.size(), 1u);
    log.AppendSince(cursor, view);
    EXPECT_EQ(view.size(), 1u);

    view.clear();
    log.AppendSince(cursor, view);
    EXPECT_TRUE(view.empty());
    EXPECT_EQ(log.Query(0, "").size(), 1u);
    log.Append(3, "second");
    log.AppendSince(cursor, view);
    ASSERT_EQ(view.size(), 1u);
    EXPECT_EQ(view[0].text, "second");

    log.Clear();
    log.Append(4, "after editor clear");
    log.AppendSince(cursor, view);
    ASSERT_EQ(view.size(), 2u);
    EXPECT_EQ(view[1].text, "after editor clear");
    for (int index = 0; index < 2100; ++index)
    {
        log.Append(2, std::to_string(index));
    }
    log.AppendSince(cursor, view);
    ASSERT_EQ(view.size(), 2048u);
    EXPECT_EQ(view.front().text, "52");
    EXPECT_EQ(view.back().text, "2099");
}
} // namespace
} // namespace zen::editor
