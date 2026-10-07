#include "Editor/Model/InspectorNavigation.h"
#include <gtest/gtest.h>

namespace zen::editor
{
namespace
{
class InspectorBrowsing : public testing::Test
{
protected:
    void SetUp() override
    {
        std::string error;

        UniquePtr<LoadedScene> loaded = ParseScene(std::string(ZEN_EDITOR_FIXTURES) + "textured.gltf", error);

        ASSERT_TRUE(loaded) << error;

        scene.Replace(std::move(loaded));

        for (const SceneAssetItem& item : scene.GetAssets().Query(""))
        {
            switch (item.id.kind)
            {
                case SceneAssetKind::Mesh: mesh = {{}, item.id}; break;
                case SceneAssetKind::Material: material = {{}, item.id}; break;
                case SceneAssetKind::Texture: texture = {{}, item.id}; break;
                case SceneAssetKind::Animation: break;
            }
        }

        const SceneAssetInspection inspection = scene.GetAssets().Inspect(mesh.asset);

        ASSERT_FALSE(inspection.nodes.empty());

        node = {inspection.nodes[0], {}};

        selection.SelectNode(node.node);

        navigation.Synchronize();
    }

    EditorScene         scene;
    EditorSelection     selection{scene};
    InspectorNavigation navigation{scene, selection};
    InspectionTarget    node;
    InspectionTarget    mesh;
    InspectionTarget    material;
    InspectionTarget    texture;
};

TEST_F(InspectorBrowsing, OpeningMaterialPreservesSelectionAndSupportsReturnToNode)
{
    const uint64_t revision = selection.GetRevision();

    navigation.Open(material);

    ASSERT_EQ(navigation.GetTabs().size(), 2u);

    EXPECT_EQ(navigation.GetTabs()[0].GetTarget(), node);

    EXPECT_EQ(navigation.GetActiveTab().GetTarget(), material);

    EXPECT_EQ(selection.GetNode(), node.node);

    EXPECT_EQ(selection.GetRevision(), revision);

    navigation.Back();

    EXPECT_EQ(navigation.GetActiveTab().GetTarget(), node);

    EXPECT_FALSE(navigation.GetActiveTab().CanGoBack());

    EXPECT_TRUE(navigation.GetActiveTab().CanGoForward());

    navigation.Forward();

    EXPECT_EQ(navigation.GetActiveTab().GetTarget(), material);

    EXPECT_FALSE(navigation.GetActiveTab().CanGoForward());

    EXPECT_EQ(selection.GetRevision(), revision);
}

TEST_F(InspectorBrowsing, HistoryActionsShareAvailabilityAndDispatchOnlyWithinTheirShortcutScope)
{
    EditorActions registry;

    navigation.RegisterActions(registry);

    ASSERT_NE(registry.Find(actions::InspectorBack), nullptr);

    ASSERT_NE(registry.Find(actions::InspectorForward), nullptr);

    const EditorShortcut back    = registry.Find(actions::InspectorBack)->shortcut;

    const EditorShortcut forward = registry.Find(actions::InspectorForward)->shortcut;

#if defined(ZEN_MACOS)
    EXPECT_EQ(FormatShortcut(back), "Option+Left");

    EXPECT_EQ(FormatShortcut(forward), "Option+Right");
#else
    EXPECT_EQ(FormatShortcut(back), "Alt+Left");

    EXPECT_EQ(FormatShortcut(forward), "Alt+Right");
#endif

    EXPECT_FALSE(registry.Execute(actions::InspectorBack));

    EXPECT_FALSE(registry.Execute(actions::InspectorForward));

    navigation.Open(material);

    EXPECT_TRUE(registry.IsEnabled(actions::InspectorBack));

    EXPECT_FALSE(registry.ExecuteShortcut(back));

    EXPECT_EQ(navigation.GetActiveTab().GetTarget(), material);

    EXPECT_TRUE(registry.Execute(actions::InspectorBack));

    EXPECT_EQ(navigation.GetActiveTab().GetTarget(), node);

    EXPECT_FALSE(registry.IsEnabled(actions::InspectorBack));

    EXPECT_TRUE(registry.ExecuteShortcut(forward, EditorShortcutScope::Inspector));

    EXPECT_EQ(navigation.GetActiveTab().GetTarget(), material);

    EXPECT_TRUE(registry.ExecuteShortcut(back, EditorShortcutScope::Inspector));

    EXPECT_TRUE(registry.Execute(actions::InspectorForward));

    EXPECT_EQ(navigation.GetActiveTab().GetTarget(), material);
}

TEST_F(InspectorBrowsing, AvailabilityQueriesDoNotSynchronizeOrDispatchStaleHistory)
{
    EditorActions registry;

    navigation.RegisterActions(registry);

    navigation.Open(material);

    const uint32_t tab = navigation.GetActiveTab().id;

    selection.SelectAsset(texture.asset);

    EXPECT_FALSE(registry.IsEnabled(actions::InspectorBack));

    EXPECT_FALSE(registry.Execute(actions::InspectorBack));

    EXPECT_EQ(navigation.GetActiveTab().id, tab);

    navigation.Synchronize();

    EXPECT_EQ(navigation.GetActiveTab().id, 0u);

    EXPECT_EQ(navigation.GetActiveTab().GetTarget(), texture);

    navigation.Activate(tab);

    EXPECT_TRUE(registry.IsEnabled(actions::InspectorBack));

    scene.Replace({});

    EXPECT_FALSE(registry.IsEnabled(actions::InspectorBack));

    EXPECT_FALSE(registry.Execute(actions::InspectorBack));

    EXPECT_EQ(navigation.GetTabs().size(), 2u);

    navigation.Synchronize();

    EXPECT_EQ(navigation.GetTabs().size(), 1u);
}

TEST_F(InspectorBrowsing, NewNavigationDiscardsForwardBranchAndRepeatedTargetsDoNotDuplicateHistory)
{
    navigation.Open(material);

    navigation.Open(texture);

    ASSERT_EQ(navigation.GetTabs().size(), 2u);

    navigation.Back();

    navigation.Open(material);

    EXPECT_TRUE(navigation.GetActiveTab().CanGoForward());

    navigation.Open(mesh);

    EXPECT_FALSE(navigation.GetActiveTab().CanGoForward());

    navigation.Forward();

    EXPECT_EQ(navigation.GetActiveTab().GetTarget(), mesh);

    navigation.Back();

    EXPECT_EQ(navigation.GetActiveTab().GetTarget(), material);

    navigation.Back();

    EXPECT_EQ(navigation.GetActiveTab().GetTarget(), node);
}

TEST_F(InspectorBrowsing, SeparateTabsKeepHistoryAndReuseAnAlreadyOpenTarget)
{
    navigation.Open(material);

    const uint32_t materialTab = navigation.GetActiveTab().id;

    navigation.Open(texture, true);

    const uint32_t textureTab = navigation.GetActiveTab().id;

    EXPECT_NE(materialTab, textureTab);

    ASSERT_EQ(navigation.GetTabs().size(), 3u);

    navigation.Activate(materialTab);

    EXPECT_EQ(navigation.GetActiveTab().GetTarget(), material);

    navigation.Activate(textureTab);

    navigation.Back();

    EXPECT_EQ(navigation.GetActiveTab().GetTarget(), material);

    navigation.Forward();

    EXPECT_EQ(navigation.GetActiveTab().GetTarget(), texture);

    navigation.Activate(0);

    navigation.Open(texture);

    EXPECT_EQ(navigation.GetActiveTab().id, textureTab);

    EXPECT_EQ(navigation.GetTabs().size(), 3u);
}

TEST_F(InspectorBrowsing, ExternalSelectionFocusesSelectionWithoutOverwritingReferenceTabs)
{
    navigation.Open(material);

    const uint32_t tab = navigation.GetActiveTab().id;

    // Re-selecting the current node must also bring Selection back into view.
    selection.SelectNode(node.node);

    navigation.Synchronize();

    EXPECT_EQ(navigation.GetActiveTab().id, 0u);

    selection.SelectAsset(texture.asset);

    navigation.Synchronize();

    EXPECT_EQ(navigation.GetActiveTab().GetTarget(), texture);

    EXPECT_FALSE(navigation.GetActiveTab().CanGoBack());

    navigation.Activate(tab);

    EXPECT_EQ(navigation.GetActiveTab().GetTarget(), material);

    navigation.Back();

    EXPECT_EQ(navigation.GetActiveTab().GetTarget(), node);

    EXPECT_EQ(selection.GetAsset(), texture.asset);

    selection.Clear();

    navigation.Synchronize();

    EXPECT_EQ(navigation.GetActiveTab().id, 0u);

    EXPECT_EQ(navigation.GetActiveTab().GetTarget(), InspectionTarget{});

    EXPECT_EQ(navigation.GetTabs().size(), 2u);
}

TEST_F(InspectorBrowsing, ClosingTabsKeepsTheActivePageAndSelectionTabCannotBeClosed)
{
    navigation.Open(material);

    const uint32_t materialTab = navigation.GetActiveTab().id;

    navigation.Open(texture, true);

    const uint32_t textureTab = navigation.GetActiveTab().id;

    navigation.Close(materialTab);

    EXPECT_EQ(navigation.GetActiveTab().id, textureTab);

    navigation.Close(0);

    navigation.Close(UINT32_MAX);

    navigation.Activate(materialTab);

    EXPECT_EQ(navigation.GetActiveTab().id, textureTab);

    navigation.Close(textureTab);

    ASSERT_EQ(navigation.GetTabs().size(), 1u);

    EXPECT_EQ(navigation.GetActiveTab().id, 0u);

    EXPECT_EQ(navigation.GetActiveTab().GetTarget(), node);
}

TEST_F(InspectorBrowsing, SceneReplacementClearsTabsAndRejectsStaleOrInvalidReferences)
{
    navigation.Open(material);

    const uint32_t oldTab = navigation.GetActiveTab().id;

    std::string error;

    UniquePtr<LoadedScene> loaded = ParseScene(std::string(ZEN_EDITOR_FIXTURES) + "textured.gltf", error);

    ASSERT_TRUE(loaded) << error;

    scene.Replace(std::move(loaded));

    // Even without an explicit selection clear, old generations must disappear.
    navigation.Open(material);

    ASSERT_EQ(navigation.GetTabs().size(), 1u);

    EXPECT_EQ(navigation.GetActiveTab().GetTarget(), InspectionTarget{});

    navigation.Back();

    navigation.Forward();

    navigation.Open({});

    navigation.Open({{scene.GetGeneration(), UINT32_MAX}, {}});

    const InspectionTarget currentMaterial{{}, {scene.GetGeneration(), SceneAssetKind::Material, 0}};

    navigation.Open({{scene.GetGeneration(), 0}, currentMaterial.asset});

    EXPECT_EQ(navigation.GetTabs().size(), 1u);

    navigation.Open(currentMaterial);

    EXPECT_EQ(navigation.GetTabs().size(), 2u);

    EXPECT_NE(navigation.GetActiveTab().id, oldTab);

    EXPECT_EQ(navigation.GetActiveTab().GetTarget(), currentMaterial);

    EXPECT_FALSE(navigation.GetActiveTab().CanGoBack());
}

TEST_F(InspectorBrowsing, LongBrowsingSessionsKeepHistoryBounded)
{
    navigation.Open(material);

    for (int index = 0; index < 150; ++index)
    {
        navigation.Open(index % 2 == 0 ? texture : material);
    }

    EXPECT_LE(navigation.GetActiveTab().history.size(), 64u);

    while (navigation.GetActiveTab().CanGoBack())
    {
        navigation.Back();
    }

    while (navigation.GetActiveTab().CanGoForward())
    {
        navigation.Forward();
    }

    EXPECT_EQ(navigation.GetActiveTab().GetTarget(), material);
}
} // namespace
} // namespace zen::editor
