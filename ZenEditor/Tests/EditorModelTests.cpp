#include "Editor/Model/EditorActions.h"
#include "Editor/Model/EditorLog.h"
#include "Editor/Model/EditorPreferences.h"
#include "Editor/Model/EditorSelection.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <fstream>

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

TEST(EditorModel, BoundsPickingAndProjectionDoNotNeedAWindow)
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

    EXPECT_EQ(scene.PickBounds(origin, direction), id);

    camera.SetOrthographic(true);

    camera.MakeRay(Vec2(0.5f), origin, direction);

    EXPECT_EQ(scene.PickBounds(origin, direction), id);

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
        MakeUnique<sg::Texture>("An unrelated display name", 9, 1, 1, asset::Format::R8G8B8A8_UNORM, color->bytesData);

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

    {
        std::ofstream second(legacy / "preferences-v2.txt");

        second << "ZenEditorPreferences2 63 1\n\"D:/Models/Sponza.gltf\"\n";
    }

    ASSERT_TRUE(LoadEditorPreferences(legacy, migrated));

    EXPECT_TRUE(migrated.panels.at("Output"));

    ASSERT_EQ(migrated.recentFiles.Get().size(), 1u);

    EXPECT_EQ(migrated.recentFiles.Get()[0], "D:/Models/Sponza.gltf");
}

TEST(EditorModel, ActionsDispatchByIdAndShortcutRespectingEnabledState)
{
    EditorActions registry;

    int opened             = 0;

    int framed             = 0;

    bool canFrame          = false;

    const uint16_t control = uint16_t(platform::KeyModifier::Control);

    registry.Register({actions::Open, "Open...", {platform::Key::O, control}, "", nullptr, [&opened]() {
                           ++opened;
                       }});

    registry.Register({actions::FrameAll,
                       "Frame All",
                       {platform::Key::Home, 0},
                       "Open a scene first.",
                       [&canFrame]() { return canFrame; },
                       [&framed]() {
                           ++framed;
                       }});

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

void RegisterDuplicateAction()
{
    EditorActions registry;

    registry.Register({"same", "Same", {}, "", nullptr, []() {
                       }});

    registry.Register({"same", "Same", {}, "", nullptr, []() {
                       }});
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

    for (int index = 0; index < 2100; ++index)
    {
        log.Append(index == 2099 ? 4 : 2, index == 2099 ? "Final ERROR" : "old entry");
    }

    EXPECT_EQ(log.Query(0, "").size(), 2048u);

    EXPECT_EQ(log.Query(4, "error").size(), 1u);

    log.Clear();

    EXPECT_TRUE(log.Query(0, "").empty());
}
} // namespace
} // namespace zen::editor
