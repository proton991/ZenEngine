#include "Editor/Services/EditorController.h"
#include "Editor/Model/EditorText.h"
#include "SyntheticEnvironment.h"
#include "Graphics/RenderCore/V2/Renderer/RendererServer.h"
#include "Graphics/RenderCore/V2/ShaderProgram.h"
#include "Graphics/RHI/RHIOptions.h"
#include "Graphics/RHI/RHIShaderUtil.h"
#include "Platform/FileSystem.h"
#include "SceneGraph/Texture.h"
#include <gtest/gtest.h>
#include <chrono>
#include <thread>
#include <fstream>
#include <gli/gli.hpp>

namespace zen::editor
{
namespace
{
class EditorRendering : public testing::TestWithParam<RHIExecutionMode>
{};

std::string Fixture(const char* name)
{
    return std::string(ZEN_EDITOR_FIXTURES) + name;
}

bool Frame(rc::RenderDevice& device, EditorController& editor)
{
    EditorViewport& viewport = editor.GetViewport();

    bool valid               = device.GetRendererServer()->DispatchRenderWorkloads(viewport.GetRenderView());

    {
        rc::RenderGraph graph("EditorHeadlessOverlays");

        valid = valid && graph.Begin();

        if (valid)
        {
            viewport.BuildOverlays(graph);

            editor.GetMeshPreview().BuildGraph(graph);

            valid = graph.End() && device.ExecuteRenderGraph(graph);

            viewport.OnSubmitted(valid);
        }
    }

    device.NextFrame();

    editor.ProcessPicks();

    return valid;
}

void ResolvePick(rc::RenderDevice& device, EditorController& editor)
{
    for (uint32_t frame = 0; frame < 16 && editor.GetViewport().IsPickPending(); ++frame)
    {
        EXPECT_TRUE(Frame(device, editor));
    }

    EXPECT_FALSE(editor.GetViewport().IsPickPending());
}

Vec2 Project(const EditorCamera& camera, Vec3 world)
{
    const sg::Camera& view = camera.GetCamera();

    const Vec4 clip        = view.GetProjectionMatrix() * view.GetViewMatrix() * Vec4(world, 1);

    return Vec2(clip) / clip.w * 0.5f + Vec2(0.5f);
}

// Headless device setup leaves scene rendering uninitialized until explicitly requested.
UniquePtr<rc::RenderDevice> CreateDevice(RHIExecutionMode mode)
{
    RHIOptions::GetInstance().SetRayTracingEnabled(false);

    RHIOptions::GetInstance().SetValidationEnabled(true);

    UniquePtr<rc::RenderDevice> device = MakeUnique<rc::RenderDevice>(RHIAPIType::eVulkan, 2, mode);

    device->Init(nullptr);

    rc::ShaderProgramManager::GetInstance().BuildShaderPrograms(device.Get());

    EXPECT_EQ(device->GetRendererServer(), nullptr);

    device->InitializeRendererServer();

    return device;
}

void DestroyDevice(rc::RenderDevice& device, EditorController& editor)
{
    device.WaitForPreviousFrames();

    editor.Destroy();

    rc::ShaderProgramManager::GetInstance().Destroy();

    device.Destroy();
}

void WaitForImport(EditorController& editor)
{
    const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);

    while (editor.GetLoadState().stage == SceneLoadStage::Reading && std::chrono::steady_clock::now() < deadline)
    {
        editor.UpdateSceneLoad();

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    ASSERT_NE(editor.GetLoadState().stage, SceneLoadStage::Reading);
}

TEST_P(EditorRendering, OffscreenSceneAndAsyncVisibleSurfacePickingWithoutWindowOrImGui)
{
    UniquePtr<rc::RenderDevice> device = CreateDevice(GetParam());

    ASSERT_NE(device->GetRendererServer(), nullptr);

    EditorController editor(*device);

    ASSERT_TRUE(editor.Init());

    ASSERT_TRUE(editor.Load(Fixture("inspection.gltf")));

    ASSERT_TRUE(editor.ResizeViewport(320, 240));

    const NodeId first{editor.GetScene().GetGeneration(), 1};

    editor.GetSelection().SelectNode(first);

    editor.FrameSelection();

    EXPECT_TRUE(Frame(*device, editor));

    editor.Pick(Vec2(0.5f));

    ResolvePick(*device, editor);

    EXPECT_EQ(editor.GetSelection().GetNode(), first);

    // This corner is inside the triangle's AABB but outside its visible surface.
    // CPU feedback selects the mesh; the asynchronous ID pass must clear it.
    sg::AABB bounds;

    ASSERT_TRUE(editor.GetScene().GetBounds(first, bounds));

    const Vec3 corner = glm::mix(bounds.GetMin(), bounds.GetMax(), Vec3(0.9f, 0.9f, 0.5f));

    editor.Pick(Project(editor.GetCamera(), corner));

    EXPECT_EQ(editor.GetSelection().GetNode(), first);

    ResolvePick(*device, editor);

    EXPECT_EQ(editor.GetSelection().GetNode(), NodeId{});

    // A pick issued before a resize and a newer selection must not overwrite them.
    editor.Pick(Vec2(0.5f));

    EXPECT_TRUE(Frame(*device, editor));

    ASSERT_TRUE(editor.ResizeViewport(411, 267));

    editor.GetSelection().SelectNode({editor.GetScene().GetGeneration(), 2});

    ResolvePick(*device, editor);

    EXPECT_EQ(editor.GetSelection().GetNode(), (NodeId{editor.GetScene().GetGeneration(), 2}));

    EXPECT_FALSE(editor.Load(Fixture("missing.gltf")));

    EXPECT_FALSE(editor.GetError().empty());

    EXPECT_TRUE(editor.GetViewport().HasScene());

    EXPECT_EQ(editor.GetScene().GetGeneration(), first.generation);

    ASSERT_TRUE(editor.Load(Fixture("inspection.gltf")));

    EXPECT_TRUE(editor.GetError().empty());

    EXPECT_GT(editor.GetScene().GetGeneration(), first.generation);

    EXPECT_TRUE(Frame(*device, editor));

    ASSERT_TRUE(editor.Load(Fixture("empty.gltf")));

    EXPECT_FALSE(editor.GetViewport().HasScene());

    EXPECT_EQ(editor.GetSelection().GetNode(), NodeId{});

    EXPECT_EQ(editor.GetPreferences().recentFiles.Get().size(), 2u);

    DestroyDevice(*device, editor);
}

TEST_P(EditorRendering, TexturePreviewsFollowTheOpenScene)
{
    UniquePtr<rc::RenderDevice> device = CreateDevice(GetParam());

    EditorController editor(*device);

    ASSERT_TRUE(editor.Init());

    ASSERT_TRUE(editor.Load(Fixture("textured.gltf")));

    ASSERT_TRUE(editor.ResizeViewport(64, 64));

    SceneAssetId texture;

    SceneAssetId mesh;

    for (const SceneAssetItem& item : editor.GetScene().GetAssets().Query(""))
    {
        texture = item.id.kind == SceneAssetKind::Texture ? item.id : texture;

        mesh    = item.id.kind == SceneAssetKind::Mesh ? item.id : mesh;
    }

    RHITexture* preview = editor.GetViewport().GetAssetPreview(texture);

    ASSERT_NE(preview, nullptr);

    EXPECT_EQ(preview->GetWidth(), 4u);

    EXPECT_EQ(preview->GetHeight(), 2u);

    EXPECT_EQ(editor.GetViewport().GetAssetPreview(mesh), nullptr);

    // The staged preview upload is submitted by ordinary frame execution.
    EXPECT_TRUE(Frame(*device, editor));

    ASSERT_TRUE(editor.Load(Fixture("inspection.gltf")));

    EXPECT_EQ(editor.GetViewport().GetAssetPreview(texture), nullptr);

    EXPECT_TRUE(Frame(*device, editor));

    DestroyDevice(*device, editor);
}

TEST_P(EditorRendering, ActionsAndDeferredLoadsRunThroughTheController)
{
    UniquePtr<rc::RenderDevice> device = CreateDevice(GetParam());

    EditorController editor(*device);

    ASSERT_TRUE(editor.Init());

    EditorActions& registry = editor.GetActions();

    EXPECT_FALSE(registry.IsEnabled(actions::FrameAll));

    EXPECT_FALSE(registry.IsEnabled(actions::Save));

    EXPECT_FALSE(registry.Find(actions::Save)->disabledReason.empty());

    EXPECT_TRUE(registry.ExecuteShortcut({platform::Key::O, uint16_t(platform::KeyModifier::Control)}));

    EXPECT_TRUE(editor.TakeFileOpenRequest());

    EXPECT_FALSE(editor.TakeFileOpenRequest());

    EXPECT_TRUE(editor.RequestLoad(Fixture("inspection.gltf")));

    EXPECT_EQ(editor.GetLoadState().stage, SceneLoadStage::Queued);

    EXPECT_TRUE(editor.UpdateSceneLoad());

    WaitForImport(editor);

    ASSERT_EQ(editor.GetLoadState().stage, SceneLoadStage::Preparing);

    EXPECT_TRUE(editor.UpdateSceneLoad());

    EXPECT_TRUE(editor.UpdateSceneLoad());

    EXPECT_EQ(editor.GetLoadState().stage, SceneLoadStage::Complete);

    EXPECT_TRUE(editor.UpdateSceneLoad());

    EXPECT_FALSE(editor.UpdateSceneLoad());

    ASSERT_NE(editor.GetScene().Get(), nullptr);

    EXPECT_TRUE(registry.IsEnabled(actions::FrameAll));

    EXPECT_FALSE(registry.IsEnabled(actions::FrameSelection));

    editor.GetSelection().SelectNode({editor.GetScene().GetGeneration(), 1});

    const uint64_t revision = editor.GetCamera().GetRevision();

    EXPECT_TRUE(registry.ExecuteShortcut({platform::Key::F, 0}));

    EXPECT_NE(editor.GetCamera().GetRevision(), revision);

    InspectorNavigation& navigation = editor.GetInspector();

    // Getting the Inspector does not apply a pending selection update.
    EXPECT_EQ(navigation.GetActiveTab().GetTarget(), InspectionTarget{});

    navigation.Synchronize();

    const NodeId first = editor.GetSelection().GetNode();

    navigation.Open({{}, editor.GetScene().Inspect(first).meshAsset});

    EXPECT_TRUE(registry.IsEnabled(actions::InspectorBack));

    const uint32_t tab = navigation.GetActiveTab().id;

    editor.GetSelection().SelectNode({editor.GetScene().GetGeneration(), 2});

    const EditorController& readOnly = editor;

    EXPECT_EQ(editor.GetInspector().GetActiveTab().id, tab);

    EXPECT_EQ(readOnly.GetInspector().GetActiveTab().id, tab);

    EXPECT_FALSE(registry.IsEnabled(actions::InspectorBack));

    navigation.Synchronize();

    EXPECT_EQ(navigation.GetActiveTab().id, 0u);

    EXPECT_EQ(navigation.GetActiveTab().GetTarget().node, editor.GetSelection().GetNode());

    DestroyDevice(*device, editor);
}

TEST_P(EditorRendering, InteractiveLoadingPublishesInStagesAndPreservesTheSceneOnFailure)
{
    UniquePtr<rc::RenderDevice> device = CreateDevice(GetParam());

    // Match the editor's default. Voxel visualization alone misses stale PBR
    // texture bindings when the old scene renders between prepare and commit.
    device->GetRendererServer()->SetRenderOption(rc::RenderOption::ePBR);

    EditorController editor(*device);

    ASSERT_TRUE(editor.Init());

    ASSERT_TRUE(editor.Load(Fixture("inspection.gltf")));

    ASSERT_TRUE(editor.ResizeViewport(160, 120));

    const uint64_t generation = editor.GetScene().GetGeneration();

    const NodeId selected{generation, 1};

    editor.GetSelection().SelectNode(selected);

    EXPECT_FALSE(editor.RequestLoad(""));

    ASSERT_TRUE(editor.RequestLoad(Fixture("textured.gltf")));

    EXPECT_FALSE(editor.RequestLoad(Fixture("empty.gltf")));

    EXPECT_FALSE(editor.Load(Fixture("empty.gltf")));

    EXPECT_FALSE(editor.GetActions().IsEnabled(actions::Open));

    ASSERT_TRUE(editor.UpdateSceneLoad());

    EXPECT_EQ(editor.GetLoadState().stage, SceneLoadStage::Reading);

    EXPECT_TRUE(Frame(*device, editor));

    WaitForImport(editor);

    ASSERT_EQ(editor.GetLoadState().stage, SceneLoadStage::Preparing);

    EXPECT_EQ(editor.GetScene().GetGeneration(), generation);

    EXPECT_EQ(editor.GetSelection().GetNode(), selected);

    EXPECT_TRUE(Frame(*device, editor));

    ASSERT_TRUE(editor.UpdateSceneLoad());

    ASSERT_EQ(editor.GetLoadState().stage, SceneLoadStage::Publishing);

    EXPECT_EQ(editor.GetScene().GetGeneration(), generation);

    EXPECT_TRUE(Frame(*device, editor));

    ASSERT_TRUE(editor.UpdateSceneLoad());

    EXPECT_EQ(editor.GetLoadState().stage, SceneLoadStage::Complete);

    EXPECT_EQ(editor.GetScene().GetGeneration(), generation + 1);

    EXPECT_EQ(editor.GetSelection().GetNode(), NodeId{});

    EXPECT_TRUE(Frame(*device, editor));

    editor.UpdateSceneLoad();

    EXPECT_FALSE(editor.GetLoadState().IsActive());

    ASSERT_TRUE(editor.RequestLoad(Fixture("missing.gltf")));

    editor.UpdateSceneLoad();

    WaitForImport(editor);

    EXPECT_EQ(editor.GetLoadState().stage, SceneLoadStage::Failed);

    EXPECT_FALSE(editor.GetError().empty());

    EXPECT_EQ(editor.GetScene().GetGeneration(), generation + 1);

    EXPECT_TRUE(Frame(*device, editor));

    editor.DismissLoadError();

    EXPECT_EQ(editor.GetLoadState().stage, SceneLoadStage::Idle);

    EXPECT_TRUE(editor.GetActions().IsEnabled(actions::Open));

    // Teardown must join a worker and release its result even if nobody polls it.
    ASSERT_TRUE(editor.RequestLoad(Fixture("textured.gltf")));

    editor.UpdateSceneLoad();

    DestroyDevice(*device, editor);
}

TEST(EditorMeshPreviewShader, DeclaresTheSceneVertexLayoutAndSharedConstants)
{
    RHIShaderGroupSPIRVPtr spirv = MakeRefCountPtr<RHIShaderGroupSPIRV>();

    spirv->SetStageSPIRV(RHIShaderStage::eVertex, platform::FileSystem::LoadSpvFile("Editor/mesh_preview.vert.spv"));

    spirv->SetStageSPIRV(RHIShaderStage::eFragment, platform::FileSystem::LoadSpvFile("Editor/mesh_preview.frag.spv"));

    RHIShaderGroupInfo info;

    ASSERT_TRUE(RHIShaderUtil::ReflectShaderGroupInfo(spirv, info));

    // The preview binds the scene vertex buffer, so the reflected stride must match it.
    EXPECT_EQ(info.vertexInputAttributes.size(), 8u);

    EXPECT_EQ(info.vertexBindingStride, sizeof(asset::Vertex));

    EXPECT_EQ(info.vertexInputAttributes[1].offset, offsetof(asset::Vertex, normal));

    // View-projection, eye and mode.
    EXPECT_EQ(info.pushConstants.size, 84u);
}

TEST_P(EditorRendering, MeshPreviewRendersSelectedMeshesInItsOwnImage)
{
    UniquePtr<rc::RenderDevice> device = CreateDevice(GetParam());

    EditorController editor(*device);

    ASSERT_TRUE(editor.Init());

    ASSERT_TRUE(editor.Load(Fixture("inspection.gltf")));

    const HeapVector<SceneAssetItem> assets = editor.GetScene().GetAssets().Query("");

    SceneAssetId mesh;

    SceneAssetId material;

    for (const SceneAssetItem& item : assets)
    {
        mesh     = item.id.kind == SceneAssetKind::Mesh ? item.id : mesh;

        material = item.id.kind == SceneAssetKind::Material ? item.id : material;
    }

    EXPECT_FALSE(editor.PreviewMesh(material, 200, 150));

    ASSERT_TRUE(editor.PreviewMesh(mesh, 200, 150));

    MeshPreviewRenderer& preview = editor.GetMeshPreview();

    ASSERT_NE(preview.GetImage(), nullptr);

    EXPECT_EQ(preview.GetWidth(), 200u);

    EXPECT_EQ(preview.GetHeight(), 152u);

    const uint64_t revision = preview.GetImageRevision();

    EXPECT_TRUE(Frame(*device, editor));

    // Wireframe adds an overlay pass where line fill is supported.
    editor.GetMeshPreviewSettings().wireframe = true;

    editor.GetMeshPreviewSettings().shading   = MeshPreviewShading::Normals;

    ASSERT_TRUE(editor.PreviewMesh(mesh, 200, 150));

    EXPECT_TRUE(Frame(*device, editor));

    EXPECT_EQ(preview.GetImageRevision(), revision);

    // Orbiting changes only the preview camera; resizing replaces the image.
    const uint64_t sceneCamera = editor.GetCamera().GetRevision();

    CameraInput input;

    input.orbit = Vec2(20, 5);

    editor.GetMeshPreviewCamera().Apply(input);

    EXPECT_EQ(editor.GetCamera().GetRevision(), sceneCamera);

    ASSERT_TRUE(editor.PreviewMesh(mesh, 320, 240));

    EXPECT_GT(preview.GetImageRevision(), revision);

    EXPECT_TRUE(Frame(*device, editor));

    // Without GPU geometry there is nothing to preview, and stale IDs are rejected.
    ASSERT_TRUE(editor.Load(Fixture("empty.gltf")));

    EXPECT_FALSE(editor.PreviewMesh(mesh, 200, 150));

    EXPECT_TRUE(Frame(*device, editor));

    DestroyDevice(*device, editor);
}

TEST_P(EditorRendering, SkinnedMeshPreviewFramesThePreparedGeometry)
{
    UniquePtr<rc::RenderDevice> device = CreateDevice(GetParam());

    EditorController editor(*device);

    ASSERT_TRUE(editor.Init());

    ASSERT_TRUE(editor.Load(std::string(ZEN_EDITOR_SHARED_FIXTURES) + "complete_scene.gltf"));

    SceneAssetId mesh;

    for (const UniquePtr<sg::Node>& node : editor.GetScene().Get()->scene->GetNodes())
    {
        if (node->skinIndex >= 0)
        {
            mesh = editor.GetScene().Inspect({editor.GetScene().GetGeneration(), node->GetIndex()}).meshAsset;
        }
    }

    ASSERT_NE(mesh.generation, 0u);

    // Start in a narrow Inspector: framing must also use this frame's image aspect.
    ASSERT_TRUE(editor.PreviewMesh(mesh, 120, 240));

    const sg::Camera& camera        = editor.GetMeshPreviewCamera().GetCamera();

    const Mat4 viewProjection       = camera.GetProjectionMatrix() * camera.GetViewMatrix();

    const rc::RenderScene& rendered = *editor.GetViewport().GetRenderScene();

    sg::AABB renderedBounds;

    for (const sg::SubMesh* primitive : editor.GetScene().GetAssets().ResolveMesh(mesh)->GetSubMeshes())
    {
        for (uint32_t offset = 0; offset < primitive->GetIndexCount(); ++offset)
        {
            const uint32_t index = rendered.GetIndices()[primitive->GetFirstIndex() + offset];

            const Vec3 position(rendered.GetVertices()[index].pos);

            renderedBounds.SetMin(position);

            renderedBounds.SetMax(position);

            const Vec4 clip = viewProjection * Vec4(position, 1);

            ASSERT_GT(clip.w, 0.0f);

            EXPECT_LE(std::abs(clip.x / clip.w), 1.0f);

            EXPECT_LE(std::abs(clip.y / clip.w), 1.0f);

            EXPECT_GE(clip.z / clip.w, 0.0f);

            EXPECT_LE(clip.z / clip.w, 1.0f);
        }
    }

    sg::AABB previewBounds;

    ASSERT_TRUE(editor.GetScene().GetMeshBounds(mesh, previewBounds));

    EXPECT_EQ(previewBounds, renderedBounds);

    EXPECT_TRUE(Frame(*device, editor));

    DestroyDevice(*device, editor);
}

TEST_P(EditorRendering, DiscardedCandidatePreservesUnrenderedEnvironmentWork)
{
    UniquePtr<rc::RenderDevice> device = CreateDevice(GetParam());

    device->GetRendererServer()->SetRenderOption(rc::RenderOption::ePBR);

    EditorController editor(*device);

    ASSERT_TRUE(editor.Init());

    ASSERT_TRUE(editor.ResizeViewport(160, 120));

    for (bool renderBeforeDiscard : {false, true})
    {
        ASSERT_TRUE(editor.Load(Fixture("inspection.gltf")));

        const uint64_t generation = editor.GetScene().GetGeneration();

        std::string error;

        UniquePtr<LoadedScene> candidate = ParseScene(Fixture("textured.gltf"), error);

        ASSERT_TRUE(candidate);

        // The current scene has not rendered yet, as when its viewport is hidden.
        ASSERT_TRUE(editor.GetViewport().PrepareScene(*candidate, error));

        if (renderBeforeDiscard)
        {
            // Rendering the old scene must leave the candidate's work queued independently.
            EXPECT_TRUE(Frame(*device, editor));

            EXPECT_TRUE(device->GetCurrentFrameRDG()->GetWarnings().empty());
        }

        editor.GetViewport().DiscardScene();

        candidate.Reset();

        EXPECT_EQ(editor.GetScene().GetGeneration(), generation);

        EXPECT_TRUE(Frame(*device, editor));

        EXPECT_TRUE(device->GetCurrentFrameRDG()->GetWarnings().empty());
    }

    ASSERT_TRUE(editor.Load(Fixture("textured.gltf")));

    EXPECT_TRUE(Frame(*device, editor));

    EXPECT_TRUE(device->GetCurrentFrameRDG()->GetWarnings().empty());

    DestroyDevice(*device, editor);
}

TEST_P(EditorRendering, FailedTextureUploadRejectsCandidateAndPreservesActiveScene)
{
    UniquePtr<rc::RenderDevice> device = CreateDevice(GetParam());

    device->GetRendererServer()->SetRenderOption(rc::RenderOption::ePBR);

    EditorController editor(*device);

    ASSERT_TRUE(editor.Init());

    ASSERT_TRUE(editor.Load(Fixture("inspection.gltf")));

    ASSERT_TRUE(editor.ResizeViewport(160, 120));

    const uint64_t generation     = editor.GetScene().GetGeneration();

    const rc::RenderScene* active = editor.GetViewport().GetRenderScene();

    const NodeId selection{generation, 1};

    editor.GetSelection().SelectNode(selection);

    std::string error;

    UniquePtr<LoadedScene> candidate = ParseScene(Fixture("textured.gltf"), error);

    ASSERT_TRUE(candidate);

    const HeapVector<sg::Texture*> textures = candidate->scene->GetComponents<sg::Texture>();

    ASSERT_FALSE(textures.empty());

    // Allocation succeeds, but the copy cannot cover the declared image.
    textures[0]->bytesData.resize(1);

    textures[0]->mipBytes.clear();

    EXPECT_FALSE(editor.GetViewport().PrepareScene(*candidate, error));

    EXPECT_FALSE(error.empty());

    candidate.Reset();

    EXPECT_EQ(editor.GetViewport().GetRenderScene(), active);

    EXPECT_EQ(editor.GetScene().GetGeneration(), generation);

    EXPECT_EQ(editor.GetSelection().GetNode(), selection);

    EXPECT_TRUE(Frame(*device, editor));

    EXPECT_TRUE(device->GetCurrentFrameRDG()->GetWarnings().empty());

    ASSERT_TRUE(editor.Load(Fixture("textured.gltf")));

    EXPECT_TRUE(Frame(*device, editor));

    DestroyDevice(*device, editor);
}

TEST_P(EditorRendering, EnvironmentSwitchIsTransactionalAndSurvivesSceneReplacement)
{
    UniquePtr<rc::RenderDevice> device = CreateDevice(GetParam());

    device->GetRendererServer()->SetRenderOption(rc::RenderOption::ePBR);

    EditorController editor(*device);

    ASSERT_TRUE(editor.Init());

    EXPECT_FALSE(editor.RequestEnvironmentTexture("papermill.ktx"));

    ASSERT_TRUE(editor.Load(Fixture("inspection.gltf")));

    ASSERT_TRUE(editor.ResizeViewport(160, 120));

    const uint64_t generation = editor.GetScene().GetGeneration();

    const NodeId selection{generation, 1};

    editor.GetSelection().SelectNode(selection);

    const rc::RenderScene* scene = editor.GetViewport().GetRenderScene();

    uint64_t revision            = scene->GetEnvironmentRevision();

    RHIResourcePtr<RHITexture> original(scene->GetEnvTexture().pSkybox);

    const std::filesystem::path directory = std::filesystem::temp_directory_path() / "ZenEditor-environment-switch";

    std::filesystem::create_directories(directory);

    HeapVector<std::string> skies;

    for (uint8_t tint = 0; tint < 4; ++tint)
    {
        const std::filesystem::path file = directory / ("sky-" + std::to_string(tint) + ".hdr");

        ASSERT_TRUE(WriteSyntheticPanorama(file, 256, uint8_t(tint * 32)));

        skies.push_back(PathToUtf8(file));
    }

    const std::string path = skies[0];

    // Replace before the old sky's preprocessing has ever executed.
    ASSERT_TRUE(editor.RequestEnvironmentTexture(path));

    EXPECT_FALSE(editor.RequestLoad(Fixture("textured.gltf")));

    EXPECT_FALSE(editor.RequestEnvironmentTexture("papermill.ktx"));

    EXPECT_TRUE(editor.UpdateEnvironment());

    EXPECT_TRUE(editor.GetEnvironmentError().empty());

    EXPECT_EQ(editor.GetViewport().GetRenderScene(), scene);

    EXPECT_EQ(editor.GetScene().GetGeneration(), generation);

    EXPECT_EQ(editor.GetSelection().GetNode(), selection);

    EXPECT_GT(scene->GetEnvironmentRevision(), revision);

    EXPECT_NE(scene->GetEnvTexture().pSkybox, original.Get());

    EXPECT_TRUE(scene->GetEnvTexture().IsComplete());

    EXPECT_EQ(scene->GetEnvTexture().pSkybox->GetFormat(), DataFormat::eR32G32B32A32SFloat);

    ASSERT_TRUE(Frame(*device, editor));

    EXPECT_TRUE(device->GetCurrentFrameRDG()->GetWarnings().empty());

    ASSERT_TRUE(device->PrepareForResourceReconfiguration());

    EXPECT_EQ(original->GetRefCount(), 1u);

    original.Reset();

    RHITexture* active = scene->GetEnvTexture().pSkybox;

    revision           = scene->GetEnvironmentRevision();

    ASSERT_TRUE(editor.RequestEnvironmentTexture("missing-environment.hdr"));

    EXPECT_TRUE(editor.UpdateEnvironment());

    EXPECT_FALSE(editor.GetEnvironmentError().empty());

    EXPECT_EQ(scene->GetEnvTexture().pSkybox, active);

    EXPECT_EQ(scene->GetEnvironmentRevision(), revision);

    EXPECT_EQ(editor.GetViewport().GetEnvironment().texturePath, path);

    ASSERT_TRUE(editor.GetViewport().SetEnvironmentLighting(2.0f, 45.0f, true, false));

    EXPECT_GT(scene->GetEnvironmentRevision(), revision);

    ASSERT_TRUE(Frame(*device, editor));

    EXPECT_TRUE(device->GetCurrentFrameRDG()->GetWarnings().empty());

    ASSERT_TRUE(editor.Load(Fixture("textured.gltf")));

    scene = editor.GetViewport().GetRenderScene();

    EXPECT_EQ(editor.GetViewport().GetEnvironment().texturePath, path);

    const rc::SceneUniformData& uniform = *reinterpret_cast<const rc::SceneUniformData*>(scene->GetSceneUniformData());

    EXPECT_FLOAT_EQ(uniform.environment.x, 2.0f);

    EXPECT_FLOAT_EQ(uniform.environment.y, glm::radians(45.0f));

    EXPECT_FLOAT_EQ(uniform.environment.w, 0.0f);

    editor.GetViewport().SetRenderMode(rc::RenderOption::eVoxelGI);

    ASSERT_TRUE(Frame(*device, editor));

    EXPECT_EQ(editor.GetViewport().GetSnapshot().renderedMode, rc::RenderOption::eVoxelGI);

    // Repeated switches without rendering must cancel every retired pending job.
    for (const std::string& sky : skies)
    {
        ASSERT_TRUE(editor.RequestEnvironmentTexture(sky));

        EXPECT_TRUE(editor.UpdateEnvironment());

        EXPECT_TRUE(editor.GetEnvironmentError().empty());
    }

    ASSERT_TRUE(editor.RequestEnvironmentTexture(""));

    EXPECT_TRUE(editor.UpdateEnvironment());

    EXPECT_TRUE(editor.GetEnvironmentError().empty());

    EXPECT_TRUE(editor.GetViewport().GetEnvironment().texturePath.empty());

    EXPECT_EQ(scene->GetEnvTexture().pSkybox->GetFormat(), DataFormat::eR16G16B16A16SFloat);

    ASSERT_TRUE(Frame(*device, editor));

    EXPECT_TRUE(device->GetCurrentFrameRDG()->GetWarnings().empty());

    ASSERT_TRUE(editor.RequestEnvironmentTexture(path));

    EXPECT_TRUE(editor.UpdateEnvironment());

    ASSERT_TRUE(editor.Load(Fixture("empty.gltf")));

    // An override can still be cleared when the newly opened scene has no GPU geometry.
    ASSERT_TRUE(editor.RequestEnvironmentTexture(""));

    EXPECT_TRUE(editor.UpdateEnvironment());

    EXPECT_TRUE(editor.GetViewport().GetEnvironment().texturePath.empty());

    EXPECT_TRUE(editor.GetEnvironmentError().empty());

    ASSERT_TRUE(editor.RequestLoad(Fixture("inspection.gltf")));

    EXPECT_FALSE(editor.RequestEnvironmentTexture(path));

    DestroyDevice(*device, editor);

    std::filesystem::remove_all(directory);
}

TEST_P(EditorRendering, RestoringAuthoredEnvironmentRestoresIntensityAndOrientation)
{
    UniquePtr<rc::RenderDevice> device = CreateDevice(GetParam());

    device->GetRendererServer()->SetRenderOption(rc::RenderOption::ePBR);

    EditorController editor(*device);

    ASSERT_TRUE(editor.Init());

    ASSERT_TRUE(editor.Load(Fixture("inspection.gltf")));

    ASSERT_TRUE(editor.ResizeViewport(160, 120));

    // Simulate the metadata/pixels produced by the glTF image-based-light importer.
    sg::SceneAssetData& asset = editor.GetScene().Get()->scene->GetAssetData();

    sg::ImageBasedLightAsset light;

    light.intensity = 3.0f;

    light.rotation  = glm::angleAxis(glm::radians(30.0f), Vec3(0, 1, 0));

    light.size = light.mipLevels = 1;

    light.specularMipFaces.resize(6);

    for (HeapVector<Vec4>& face : light.specularMipFaces)
    {
        face.push_back(Vec4(1));
    }

    light.irradianceCoefficients.push_back(Vec3(1));

    asset.imageBasedLights.push_back(light);

    asset.imageBasedLight = 0;

    ASSERT_TRUE(editor.RequestEnvironmentTexture(""));

    EXPECT_TRUE(editor.UpdateEnvironment());

    EXPECT_TRUE(editor.GetEnvironmentError().empty());

    const rc::RenderScene* scene = editor.GetViewport().GetRenderScene();

    EXPECT_TRUE(scene->GetEnvTexture().authoredCubemaps);

    const rc::SceneUniformData& uniform = *reinterpret_cast<const rc::SceneUniformData*>(scene->GetSceneUniformData());

    const Vec4 orientation              = uniform.environmentOrientation;

    EXPECT_FLOAT_EQ(uniform.environment.x, 3.0f);

    EXPECT_FLOAT_EQ(uniform.environmentProperties.x, 1.0f);

    ASSERT_TRUE(editor.GetViewport().SetEnvironmentLighting(2, 15, false, true));

    EXPECT_FLOAT_EQ(uniform.environment.x, 6.0f);

    // A path relative to Data/Textures, using the repository's default cubemap.
    ASSERT_TRUE(editor.RequestEnvironmentTexture("papermill.ktx"));

    EXPECT_TRUE(editor.UpdateEnvironment());

    EXPECT_TRUE(editor.GetEnvironmentError().empty());

    EXPECT_FALSE(scene->GetEnvTexture().authoredCubemaps);

    EXPECT_FLOAT_EQ(uniform.environment.x, 2.0f);

    EXPECT_FLOAT_EQ(uniform.environmentProperties.x, 0.0f);

    EXPECT_EQ(uniform.environmentOrientation, Vec4(0, 0, 0, 1));

    ASSERT_TRUE(Frame(*device, editor));

    ASSERT_TRUE(editor.RequestEnvironmentTexture(""));

    EXPECT_TRUE(editor.UpdateEnvironment());

    EXPECT_TRUE(editor.GetEnvironmentError().empty());

    EXPECT_TRUE(scene->GetEnvTexture().authoredCubemaps);

    EXPECT_FLOAT_EQ(uniform.environment.x, 6.0f);

    EXPECT_FLOAT_EQ(uniform.environment.y, glm::radians(15.0f));

    EXPECT_FLOAT_EQ(uniform.environment.z, 0.0f);

    EXPECT_FLOAT_EQ(uniform.environment.w, 1.0f);

    EXPECT_EQ(uniform.environmentOrientation, orientation);

    ASSERT_TRUE(Frame(*device, editor));

    EXPECT_TRUE(device->GetCurrentFrameRDG()->GetWarnings().empty());

    DestroyDevice(*device, editor);
}

TEST_P(EditorRendering, CubemapContainersRejectTruncatedAndUnsupportedFiles)
{
    UniquePtr<rc::RenderDevice> device = CreateDevice(GetParam());

    EditorController editor(*device);

    ASSERT_TRUE(editor.Init());

    ASSERT_TRUE(editor.Load(Fixture("inspection.gltf")));

    ASSERT_TRUE(editor.ResizeViewport(160, 120));

    for (const char* extension : {".ktx", ".dds"})
    {
        const std::filesystem::path file =
            std::filesystem::temp_directory_path() / (std::string("ZenEditor-environment-container") + extension);

        gli::texture_cube cube(gli::FORMAT_RGBA16_SFLOAT_PACK16, gli::texture_cube::extent_type(4), 3);

        std::memset(cube.data(), 0, cube.size());

        ASSERT_TRUE(gli::save(cube, file.string()));

        ASSERT_TRUE(editor.RequestEnvironmentTexture(file.string()));

        EXPECT_TRUE(editor.UpdateEnvironment());

        EXPECT_TRUE(editor.GetEnvironmentError().empty());

        ASSERT_TRUE(Frame(*device, editor));

        RHITexture* active = editor.GetViewport().GetRenderScene()->GetEnvTexture().pSkybox;

        // Complete header but incomplete pixel data must never reach GLI's memcpy.
        std::filesystem::resize_file(file, std::filesystem::file_size(file) - 1);

        ASSERT_TRUE(editor.RequestEnvironmentTexture(file.string()));

        EXPECT_TRUE(editor.UpdateEnvironment());

        EXPECT_FALSE(editor.GetEnvironmentError().empty());

        EXPECT_EQ(editor.GetViewport().GetRenderScene()->GetEnvTexture().pSkybox, active);

        std::filesystem::resize_file(file, 3);

        ASSERT_TRUE(editor.RequestEnvironmentTexture(file.string()));

        EXPECT_TRUE(editor.UpdateEnvironment());

        EXPECT_FALSE(editor.GetEnvironmentError().empty());

        EXPECT_EQ(editor.GetViewport().GetRenderScene()->GetEnvTexture().pSkybox, active);

        gli::texture2d image(gli::FORMAT_RGBA8_UNORM_PACK8, gli::texture2d::extent_type(4), 1);

        std::memset(image.data(), 0, image.size());

        ASSERT_TRUE(gli::save(image, file.string()));

        ASSERT_TRUE(editor.RequestEnvironmentTexture(file.string()));

        EXPECT_TRUE(editor.UpdateEnvironment());

        EXPECT_FALSE(editor.GetEnvironmentError().empty());

        EXPECT_EQ(editor.GetViewport().GetRenderScene()->GetEnvTexture().pSkybox, active);

        ASSERT_TRUE(Frame(*device, editor));

        EXPECT_TRUE(device->GetCurrentFrameRDG()->GetWarnings().empty());

        std::filesystem::remove(file);
    }

    DestroyDevice(*device, editor);
}

INSTANTIATE_TEST_SUITE_P(Execution, EditorRendering, testing::Values(RHIExecutionMode::eInline, RHIExecutionMode::eThreaded));
} // namespace
} // namespace zen::editor
