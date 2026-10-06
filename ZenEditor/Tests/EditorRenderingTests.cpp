#include "Editor/Services/EditorController.h"
#include "Editor/Model/EditorText.h"
#include "SyntheticEnvironment.h"
#include "Graphics/RenderCore/V2/Renderer/RendererServer.h"
#include "Graphics/RenderCore/V2/Renderer/VoxelizerBase.h"
#include "Graphics/RenderCore/V2/Renderer/VoxelGIRenderer.h"
#include "Graphics/RenderCore/V2/ShaderProgram.h"
#include "Graphics/RHI/RHIOptions.h"
#include "Graphics/RHI/RHIShaderUtil.h"
#include "Platform/FileSystem.h"
#include "SceneGraph/Texture.h"
#include <gtest/gtest.h>
#include <spdlog/sinks/ostream_sink.h>
#include <chrono>
#include <thread>
#include <fstream>
#include <filesystem>
#include <cstdlib>
#include <sstream>
#include <gli/gli.hpp>

namespace zen::editor
{
namespace
{
class EditorRendering : public testing::TestWithParam<RHIExecutionMode>
{};

// Captures default-logger output for one scope while no frame is recording.
// spdlog's logger and sink APIs require std::shared_ptr.
class ScopedLogCapture
{
public:
    ScopedLogCapture() :
        m_previous(spdlog::default_logger()),
        m_logger(std::make_shared<spdlog::logger>("editor_rendering_test",
                                                  std::make_shared<spdlog::sinks::ostream_sink_mt>(m_output)))
    {
        spdlog::set_default_logger(m_logger);
    }

    ~ScopedLogCapture()
    {
        spdlog::set_default_logger(m_previous);
    }

    std::string GetText() const
    {
        return m_output.str();
    }

private:
    std::ostringstream              m_output;
    std::shared_ptr<spdlog::logger> m_previous;
    std::shared_ptr<spdlog::logger> m_logger;
};

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

HeapVector<uint8_t> ReadScenePixels(rc::RenderDevice& device, RHITexture* image)
{
    device.WaitForPreviousFrames();

    const uint32_t width  = image->GetWidth();

    const uint32_t height = image->GetHeight();

    RHIBufferCreateInfo info;

    info.size         = uint64_t(width) * height * 4;

    info.allocateType = RHIBufferAllocateType::eGPU;

    info.usageFlags.SetFlags(RHIBufferUsageFlagBits::eStorageBuffer, RHIBufferUsageFlagBits::eTransferSrcBuffer);

    RHIBuffer* packed = device.CreateBuffer(info);

    info.allocateType = RHIBufferAllocateType::eCPURead;

    info.usageFlags.SetFlag(RHIBufferUsageFlagBits::eTransferDstBuffer);

    RHIBuffer* readback = device.CreateBuffer(info);

    RHISampler* sampler = device.CreateSampler(RHISamplerCreateInfo::CreateLinearRepeat());

    rc::RenderGraph graph("DebugViewReadback");

    EXPECT_TRUE(graph.Begin());

    rc::RDGComputePassDesc pass;

    pass.SetShaderProgramName("CaptureFrameSP");

    pass.BindSampledTexture("sourceColor", sampler, image->GetDefaultView());

    pass.BindStorageBuffer("packedColor", packed, rc::RDGContentGuarantee::eFullWrite);

    graph.AddComputePass(std::move(pass)).RecordPassCommands([width, height](rc::RDGPassCmdEncoder& encoder) {
        encoder.Dispatch((width + 7) / 8, (height + 7) / 8, 1);
    });

    graph.AddTransferPass("ReadDebugView").CopyBuffer(packed, readback, {0, 0, info.size}).NeverCull();

    EXPECT_TRUE(graph.End());

    EXPECT_TRUE(device.ExecuteRenderGraph(graph));

    device.WaitForPreviousFrames();

    const uint8_t* pixels = readback->Map();

    HeapVector<uint8_t> result(pixels, static_cast<size_t>(info.size));

    readback->Unmap();

    device.DestroyBuffer(packed);

    device.DestroyBuffer(readback);

    return result;
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

TEST_P(EditorRendering, RenderingConfigurationLightsCameraAndResourceApply)
{
    UniquePtr<rc::RenderDevice> device = CreateDevice(GetParam());

    EditorController editor(*device);

    ASSERT_TRUE(editor.Init());

    ASSERT_TRUE(editor.Load(Fixture("inspection.gltf")));

    ASSERT_TRUE(editor.ResizeViewport(128, 128));

    const size_t imported = editor.GetRenderingState().GetDraft().lights.size();

    ASSERT_GT(imported, 0u);

    ASSERT_TRUE(editor.AddBoundsLights(true));

    ASSERT_TRUE(editor.UpdateRenderingSettings());

    EXPECT_EQ(editor.GetViewport().GetRenderScene()->GetLights().GetEntries().size(), imported + 8);

    ASSERT_TRUE(editor.AddBoundsLights(true));

    ASSERT_TRUE(editor.AddBoundsLights(true));

    const size_t pendingLights = editor.GetRenderingState().GetDraft().lights.size();

    EXPECT_FALSE(editor.AddBoundsLights(true));

    EXPECT_EQ(editor.GetRenderingState().GetDraft().lights.size(), pendingLights);

    EXPECT_EQ(editor.GetViewport().GetRenderScene()->GetLights().GetEntries().size(), imported + 8);

    rc::RenderingSettings settings = editor.GetRenderingState().GetDraft();

    settings.algorithm             = rc::RenderAlgorithm::ePBR;

    settings.lights.clear();

    settings.cameraLight.enabled = true;

    ASSERT_TRUE(editor.StageRenderingSettings(settings));

    ASSERT_TRUE(editor.UpdateRenderingSettings());

    const uint64_t lightsRevision = editor.GetViewport().GetRenderScene()->GetLights().GetRevision();

    ASSERT_TRUE(Frame(*device, editor));

    const rc::SceneUniformData before =
        *reinterpret_cast<const rc::SceneUniformData*>(editor.GetViewport().GetRenderScene()->GetSceneUniformData());

    EXPECT_EQ(before.lightInfo.x, 0.0f);

    EXPECT_GT(before.cameraLight.colorIntensity.w, 0.0f);

    editor.GetCamera().OrbitBy(Vec2(0.3f, 0.1f));

    ASSERT_TRUE(Frame(*device, editor));

    const rc::SceneUniformData after =
        *reinterpret_cast<const rc::SceneUniformData*>(editor.GetViewport().GetRenderScene()->GetSceneUniformData());

    EXPECT_NE(before.cameraLight.positionRange, after.cameraLight.positionRange);

    EXPECT_EQ(editor.GetViewport().GetRenderScene()->GetLights().GetRevision(), lightsRevision);

    editor.ResetRenderingLights();

    ASSERT_TRUE(editor.UpdateRenderingSettings());

    EXPECT_EQ(editor.GetViewport().GetRenderScene()->GetLights().GetEntries().size(), imported);

    settings                  = editor.GetRenderingState().GetDraft();

    const uint32_t grid       = editor.GetViewport().GetSnapshot().settings.resolution;

    const uint32_t staged     = grid == 64 ? 128 : 64;

    const uint32_t lightCount = static_cast<uint32_t>(settings.lights.size());

    settings.gi.resolution    = staged;

    {
        ScopedLogCapture log;

        ASSERT_TRUE(editor.StageRenderingSettings(settings));

        ASSERT_TRUE(editor.UpdateRenderingSettings());

        EXPECT_EQ(editor.GetViewport().GetSnapshot().settings.resolution, grid);

        // Light, environment, cone and output edits keep previewing while the grid waits for Apply.
        settings.environment.intensity           = 2.5f;

        settings.lights.front().light.intensity *= 0.5f;

        settings.gi.cone.indirectIntensity       = 1.5f;

        settings.debug.output                    = rc::DebugOutput::eDepth;

        ASSERT_TRUE(editor.StageRenderingSettings(settings));

        ASSERT_TRUE(editor.UpdateRenderingSettings()) << editor.GetRenderingState().GetError();

        EXPECT_FLOAT_EQ(editor.GetViewport().GetEnvironment().intensity, 2.5f);

        EXPECT_FLOAT_EQ(editor.GetViewport().GetRenderScene()->GetLights().GetEntries().front().light.intensity,
                        settings.lights.front().light.intensity);

        EXPECT_FLOAT_EQ(editor.GetViewport().GetSnapshot().settings.cone.indirectIntensity, 1.5f);

        EXPECT_EQ(device->GetRendererServer()->GetDebugSelection().output, rc::DebugOutput::eDepth);

        EXPECT_EQ(editor.GetViewport().GetSnapshot().settings.resolution, grid);

        EXPECT_TRUE(editor.GetRenderingState().NeedsResourceApply());

        EXPECT_TRUE(editor.GetRenderingState().IsPending());

        // Previews never rebuild GI resources, so they never log a resource application.
        EXPECT_EQ(log.GetText().find("Applied runtime GI settings"), std::string::npos) << log.GetText();

        settings.debug.output = rc::DebugOutput::eFinal;

        ASSERT_TRUE(editor.StageRenderingSettings(settings, true));

        ASSERT_TRUE(editor.UpdateRenderingSettings());

        EXPECT_NE(log.GetText().find("resources=recreated"), std::string::npos) << log.GetText();
    }

    EXPECT_EQ(editor.GetViewport().GetSnapshot().settings.resolution, staged);

    EXPECT_FALSE(editor.GetRenderingState().IsPending());

    EXPECT_EQ(editor.GetViewport().GetRenderScene()->GetLights().GetEntries().size(), lightCount);

    settings.algorithm              = rc::RenderAlgorithm::eVoxelGI;

    settings.gi.shadowMapResolution = 128;

    ASSERT_TRUE(editor.StageRenderingSettings(settings, true));

    ASSERT_TRUE(editor.UpdateRenderingSettings());

    ASSERT_TRUE(Frame(*device, editor));

    EXPECT_EQ(editor.GetViewport().GetSnapshot().status.effective, rc::RenderAlgorithm::eVoxelGI);

    settings.gi.averagedReflectance    = true;

    settings.gi.reflectanceBudgetBytes = 1;

    ASSERT_TRUE(editor.StageRenderingSettings(settings, true));

    EXPECT_FALSE(editor.UpdateRenderingSettings());

    EXPECT_FALSE(editor.GetRenderingState().GetError().empty());

    EXPECT_FALSE(editor.GetRenderingState().GetApplied().gi.averagedReflectance);

    settings                                = editor.GetRenderingState().GetApplied();

    const uint64_t revision                 = editor.GetRenderingState().GetAppliedRevision();

    settings.lights.front().light.direction = Vec3(0);

    EXPECT_FALSE(editor.StageRenderingSettings(settings));

    EXPECT_FALSE(editor.UpdateRenderingSettings());

    EXPECT_EQ(editor.GetRenderingState().GetAppliedRevision(), revision);

    ASSERT_TRUE(Frame(*device, editor));

    EXPECT_TRUE(device->GetCurrentFrameRDG()->GetWarnings().empty());

    DestroyDevice(*device, editor);
}

TEST_P(EditorRendering, VoxelDebugShowsOpaqueSRGBAlbedoWithoutLighting)
{
    UniquePtr<rc::RenderDevice> device = CreateDevice(GetParam());

    EditorController editor(*device);

    ASSERT_TRUE(editor.Init());

    ASSERT_TRUE(editor.Load(Fixture("inspection.gltf")));

    editor.GetSelection().SelectNode({editor.GetScene().GetGeneration(), 1});

    editor.FrameSelection();

    ASSERT_TRUE(editor.ResizeViewport(96, 64));

    rc::RenderingSettings settings = editor.GetRenderingState().GetDraft();

    settings.algorithm             = rc::RenderAlgorithm::eVoxelGI;

    settings.debug.output          = rc::DebugOutput::eVoxels;

    settings.gi.resolution         = 64;

    settings.lights.clear();

    settings.environment.intensity = 0;

    settings.environment.skybox    = false;

    settings.cameraLight.enabled   = false;

    for (const platform::VoxelizerMode mode : {platform::VoxelizerMode::eCompute, platform::VoxelizerMode::eGeometry})
    {
        if (mode == platform::VoxelizerMode::eCompute || device->GetGPUInfo().supportGeometryShader)
        {
            SCOPED_TRACE(static_cast<int>(mode));

            settings.gi.voxelizer = mode;

            ASSERT_TRUE(editor.StageRenderingSettings(settings, true));

            ASSERT_TRUE(editor.UpdateRenderingSettings()) << editor.GetRenderingState().GetError();

            // Check the initial volume build and the cached visualization on the next frame.
            for (uint32_t frame = 0; frame < 2; ++frame)
            {
                ASSERT_TRUE(Frame(*device, editor));

                EXPECT_TRUE(editor.GetViewport().GetSnapshot().debug.available);

                EXPECT_TRUE(device->GetCurrentFrameRDG()->GetWarnings().empty());

                const HeapVector<uint8_t> pixels = ReadScenePixels(*device, editor.GetViewport().GetRenderView().color);

                ASSERT_EQ(pixels.size(), 96u * 64 * 4);

                const size_t center = (32 * 96 + 48) * 4;

                // The fixture's linear base color is (0.7, 0.35, 0.1).
                EXPECT_NEAR(pixels[center], 218, 2);

                EXPECT_NEAR(pixels[center + 1], 160, 2);

                EXPECT_NEAR(pixels[center + 2], 89, 2);

                EXPECT_EQ(pixels[center + 3], 255);
            }
        }
    }

    DestroyDevice(*device, editor);
}

TEST_P(EditorRendering, DebugOutputsRetireAcrossModeSceneAndExtentChanges)
{
    UniquePtr<rc::RenderDevice> device = CreateDevice(GetParam());

    EditorController editor(*device);

    ASSERT_TRUE(editor.Init());

    ASSERT_TRUE(editor.Load(Fixture("inspection.gltf")));

    editor.GetSelection().SelectNode({editor.GetScene().GetGeneration(), 1});

    editor.FrameSelection();

    ASSERT_TRUE(editor.ResizeViewport(96, 64));

    rc::RenderingSettings settings         = editor.GetRenderingState().GetDraft();

    settings.algorithm                     = rc::RenderAlgorithm::ePBR;

    settings.gi.resolution                 = 64;

    settings.gi.shadowMapResolution        = 128;

    settings.debug.lightId                 = settings.lights.front().id;

    settings.lights.front().light.position = editor.GetCamera().GetCamera().GetPos();

    settings.debug.face                    = 5;

    HeapVector<uint8_t> albedo;

    sg::AABB selectedBounds;

    ASSERT_TRUE(editor.GetScene().GetBounds(editor.GetSelection().GetNode(), selectedBounds));

    const Vec4 centerView = editor.GetCamera().GetCamera().GetViewMatrix() * Vec4(selectedBounds.GetCenter(), 1);

    for (const rc::DebugOutput output :
         {rc::DebugOutput::eFinal, rc::DebugOutput::eAlbedo, rc::DebugOutput::eNormal, rc::DebugOutput::eDepth,
          rc::DebugOutput::eShadow, rc::DebugOutput::eVoxelSlice, rc::DebugOutput::eVoxels})
    {
        settings.debug.output      = output;

        settings.debug.maximum     = output == rc::DebugOutput::eDepth ? 10.0f : 1.0f;

        const sg::AABB voxelBounds = device->GetRendererServer()->RequestVoxelizer()->GetVoxelBounds();

        settings.debug.slice       = static_cast<uint32_t>((selectedBounds.GetCenter().z - voxelBounds.GetMin().z)
                                                           / voxelBounds.GetMaxExtent() * settings.gi.resolution);

        ASSERT_TRUE(editor.StageRenderingSettings(settings, true));

        ASSERT_TRUE(editor.UpdateRenderingSettings()) << editor.GetRenderingState().GetError();

        ASSERT_TRUE(Frame(*device, editor)) << static_cast<int>(output);

        const rc::DebugOutputDescription description = editor.GetViewport().GetSnapshot().debug;

        EXPECT_TRUE(description.available) << static_cast<int>(output) << ": " << description.reason;

        EXPECT_EQ(description.output, output);

        EXPECT_TRUE(device->GetCurrentFrameRDG()->GetWarnings().empty());

        const HeapVector<uint8_t> pixels = ReadScenePixels(*device, editor.GetViewport().GetRenderView().color);

        EXPECT_EQ(pixels.size(), 96u * 64 * 4);

        if (output == rc::DebugOutput::eShadow)
        {
            uint8_t darkest = 255;

            for (size_t pixel = 0; pixel < pixels.size(); pixel += 4)
            {
                darkest = std::min(darkest, pixels[pixel]);
            }

            EXPECT_LT(darkest, 250);
        }

        if (output == rc::DebugOutput::eVoxelSlice)
        {
            uint8_t brightest = 0;

            for (size_t pixel = 0; pixel < pixels.size(); pixel += 4)
            {
                brightest = std::max(brightest, pixels[pixel]);
            }

            EXPECT_GT(brightest, 0);
        }

        if (output == rc::DebugOutput::eAlbedo)
        {
            albedo              = pixels;

            const size_t center = (32 * 96 + 48) * 4;

            EXPECT_NEAR(pixels[center], 218, 2);

            EXPECT_NEAR(pixels[center + 1], 160, 2);

            EXPECT_NEAR(pixels[center + 2], 89, 2);
        }

        if (output == rc::DebugOutput::eDepth)
        {
            const size_t center = (32 * 96 + 48) * 4;

            EXPECT_NEAR(pixels[center], std::abs(centerView.z) / 10.0f * 255.0f, 2);
        }

        if (output == rc::DebugOutput::eNormal)
        {
            EXPECT_NE(std::memcmp(albedo.data(), pixels.data(), pixels.size()), 0);

            const size_t center = (32 * 96 + 48) * 4;

            EXPECT_NEAR(pixels[center], 128, 1);

            EXPECT_NEAR(pixels[center + 1], 128, 1);

            EXPECT_NEAR(pixels[center + 2], 255, 1);
        }
    }

    settings.debug.output  = rc::DebugOutput::eDepth;

    settings.debug.maximum = 10.0f;

    ASSERT_TRUE(editor.StageRenderingSettings(settings));

    ASSERT_TRUE(editor.UpdateRenderingSettings());

    editor.GetCamera().SetOrthographic(true);

    ASSERT_TRUE(editor.ResizeViewport(73, 51));

    ASSERT_TRUE(Frame(*device, editor));

    EXPECT_EQ(editor.GetViewport().GetSnapshot().debug.width, editor.GetViewport().GetRenderView().width);

    const HeapVector<uint8_t> orthographic = ReadScenePixels(*device, editor.GetViewport().GetRenderView().color);

    const rc::RenderView& resized          = editor.GetViewport().GetRenderView();

    const size_t centerPixel               = (resized.height / 2 * resized.width + resized.width / 2) * 4;

    EXPECT_NEAR(orthographic[centerPixel], std::abs(centerView.z) / 10.0f * 255.0f, 2);

    settings.debug.linearDepth = false;

    settings.debug.maximum     = 1.0f;

    ASSERT_TRUE(editor.StageRenderingSettings(settings));

    ASSERT_TRUE(editor.UpdateRenderingSettings());

    ASSERT_TRUE(Frame(*device, editor));

    const HeapVector<uint8_t> raw = ReadScenePixels(*device, editor.GetViewport().GetRenderView().color);

    const Vec4 clip               = editor.GetCamera().GetCamera().GetProjectionMatrix() * centerView;

    EXPECT_NEAR(raw[centerPixel], clip.z / clip.w * 255.0f, 2);

    ASSERT_TRUE(editor.Load(std::string(ZEN_EDITOR_SHARED_FIXTURES) + "all_primitive_modes.gltf"));

    settings              = editor.GetRenderingState().GetDraft();

    settings.debug.output = rc::DebugOutput::eNormal;

    ASSERT_TRUE(editor.StageRenderingSettings(settings));

    ASSERT_TRUE(editor.UpdateRenderingSettings());

    ASSERT_TRUE(Frame(*device, editor));

    EXPECT_FALSE(editor.GetViewport().GetSnapshot().debug.available);

    EXPECT_FALSE(editor.GetViewport().GetSnapshot().debug.reason.empty());

    const HeapVector<uint8_t> unavailable = ReadScenePixels(*device, editor.GetViewport().GetRenderView().color);

    for (size_t pixel = 4; pixel < unavailable.size(); pixel += 4)
    {
        EXPECT_EQ(std::memcmp(unavailable.data(), unavailable.data() + pixel, 4), 0);
    }

    ASSERT_TRUE(editor.Load(Fixture("inspection.gltf")));

    ASSERT_TRUE(Frame(*device, editor));

    EXPECT_TRUE(editor.GetViewport().GetSnapshot().debug.available);

    EXPECT_TRUE(device->GetCurrentFrameRDG()->GetWarnings().empty());

    DestroyDevice(*device, editor);
}

TEST_P(EditorRendering, VoxelDebugViewsSurviveGridDownsizingAndAnUnusedSliceSelection)
{
    UniquePtr<rc::RenderDevice> device = CreateDevice(GetParam());

    EditorController editor(*device);

    ASSERT_TRUE(editor.Init());

    ASSERT_TRUE(editor.Load(Fixture("inspection.gltf")));

    ASSERT_TRUE(editor.ResizeViewport(96, 64));

    rc::RenderingSettings settings = editor.GetRenderingState().GetDraft();

    settings.gi.resolution         = 128;

    settings.debug.output          = rc::DebugOutput::eVoxelSlice;

    settings.debug.slice           = 100;

    ASSERT_TRUE(editor.StageRenderingSettings(settings, true));

    ASSERT_TRUE(editor.UpdateRenderingSettings()) << editor.GetRenderingState().GetError();

    ASSERT_TRUE(Frame(*device, editor));

    EXPECT_TRUE(editor.GetViewport().GetSnapshot().debug.available);

    settings.gi.resolution = 64;

    EXPECT_TRUE(editor.StageRenderingSettings(settings)) << editor.GetRenderingState().GetError();

    EXPECT_TRUE(editor.UpdateRenderingSettings()) << editor.GetRenderingState().GetError();

    EXPECT_EQ(editor.GetRenderingState().GetApplied().gi.resolution, 128u);

    EXPECT_TRUE(editor.GetRenderingState().NeedsResourceApply());

    EXPECT_TRUE(Frame(*device, editor));

    EXPECT_TRUE(editor.GetViewport().GetSnapshot().debug.available);

    EXPECT_TRUE(editor.StageRenderingSettings(editor.GetRenderingState().GetDraft(), true));

    EXPECT_TRUE(editor.UpdateRenderingSettings()) << editor.GetRenderingState().GetError();

    EXPECT_TRUE(Frame(*device, editor));

    EXPECT_EQ(editor.GetRenderingState().GetApplied().gi.resolution, 64u);

    EXPECT_EQ(editor.GetViewport().GetSnapshot().debug.width, 64u);

    EXPECT_TRUE(editor.GetViewport().GetSnapshot().debug.available);

    settings              = editor.GetRenderingState().GetDraft();

    settings.debug.output = rc::DebugOutput::eVoxels;

    settings.debug.slice  = 100;

    EXPECT_TRUE(editor.StageRenderingSettings(settings, true));

    EXPECT_TRUE(editor.UpdateRenderingSettings()) << editor.GetRenderingState().GetError();

    EXPECT_TRUE(Frame(*device, editor));

    EXPECT_EQ(editor.GetViewport().GetSnapshot().debug.width, 64u);

    EXPECT_EQ(editor.GetViewport().GetSnapshot().debug.output, rc::DebugOutput::eVoxels);

    EXPECT_TRUE(editor.GetViewport().GetSnapshot().debug.available);

    EXPECT_TRUE(device->GetCurrentFrameRDG()->GetWarnings().empty());

    DestroyDevice(*device, editor);
}

TEST_P(EditorRendering, CameraLightChangesDirectPixelsWithoutSceneLights)
{
    UniquePtr<rc::RenderDevice> device = CreateDevice(GetParam());

    EditorController editor(*device);

    ASSERT_TRUE(editor.Init());

    for (const std::string& path : {Fixture("inspection.gltf"), Fixture("forward_lit.gltf")})
    {
        ASSERT_TRUE(editor.Load(path));

        ASSERT_TRUE(editor.ResizeViewport(64, 64));

        rc::RenderingSettings settings = editor.GetRenderingState().GetDraft();

        settings.algorithm             = rc::RenderAlgorithm::ePBR;

        settings.debug.output          = rc::DebugOutput::eFinal;

        settings.lights.clear();

        settings.environment.intensity = 0.0f;

        settings.environment.skybox    = false;

        settings.lightMarkers          = false;

        settings.cameraLight.enabled   = false;

        ASSERT_TRUE(editor.StageRenderingSettings(settings));

        ASSERT_TRUE(editor.UpdateRenderingSettings());

        ASSERT_TRUE(Frame(*device, editor));

        const HeapVector<uint8_t> dark = ReadScenePixels(*device, editor.GetViewport().GetRenderView().color);

        const uint64_t revision        = editor.GetViewport().GetRenderScene()->GetLights().GetRevision();

        settings.cameraLight.enabled   = true;

        settings.cameraLight.range     = 4.0f;

        settings.cameraLight.intensity = 0.75f;

        ASSERT_TRUE(editor.StageRenderingSettings(settings));

        ASSERT_TRUE(editor.UpdateRenderingSettings());

        ASSERT_TRUE(Frame(*device, editor));

        const HeapVector<uint8_t> lit = ReadScenePixels(*device, editor.GetViewport().GetRenderView().color);

        uint64_t darkSum              = 0;

        uint64_t litSum               = 0;

        for (size_t index = 0; index < lit.size(); ++index)
        {
            if (index % 4 != 3)
            {
                darkSum += dark[index];

                litSum  += lit[index];
            }
        }

        EXPECT_GT(litSum, darkSum) << path;

        EXPECT_EQ(editor.GetViewport().GetRenderScene()->GetLights().GetRevision(), revision);
    }

    DestroyDevice(*device, editor);
}

HeapVector<uint8_t> ReadBallRadianceSlice(rc::RenderDevice& device, EditorController& editor)
{
    rc::RenderGraph graph("LightBallRadianceReadback");

    EXPECT_TRUE(graph.Begin());

    rc::DebugVisualizationData data;

    data.selection.z = 2;

    sg::AABB surfaceBounds;

    EXPECT_TRUE(editor.GetScene().GetBounds({editor.GetScene().GetGeneration(), 1}, surfaceBounds));

    rc::VoxelizerBase* voxelizer = device.GetRendererServer()->RequestVoxelizer();

    data.selection.w             = static_cast<uint32_t>((surfaceBounds.GetCenter().z - voxelizer->GetVoxelBounds().GetMin().z)
                                                         / voxelizer->GetVoxelSize());

    rc::RDGGraphicsPassDesc pass =
        rc::MakeDebugVisualizationPass(editor.GetViewport().GetRenderView(), "RenderDebugVolumeSP", data);

    RHISampler* sampler = device.CreateSampler({});

    pass.BindSampledTexture("sourceImage", sampler,
                            device.GetRendererServer()->RequestVoxelGI()->GetRadianceTexture()->GetDefaultView());

    rc::AddDebugVisualizationPass(graph, std::move(pass));

    EXPECT_TRUE(graph.End());

    EXPECT_TRUE(device.ExecuteRenderGraph(graph));

    return ReadScenePixels(device, editor.GetViewport().GetRenderView().color);
}

uint64_t PixelEnergy(const HeapVector<uint8_t>& pixels)
{
    uint64_t sum = 0;

    for (size_t index = 0; index < pixels.size(); ++index)
    {
        if (index % 4 != 3)
        {
            sum += pixels[index];
        }
    }

    return sum;
}

// A small occluder casts a visible offset shadow on the larger receiver. Compare
// on/off/on frames with GI and environment contributions disabled to isolate it.
TEST_P(EditorRendering, MeshShadowsChangeDirectLightingWithoutApply)
{
    UniquePtr<rc::RenderDevice> device = CreateDevice(GetParam());

    EditorController editor(*device);

    ASSERT_TRUE(editor.Init());

    ASSERT_TRUE(editor.Load(Fixture("shadow_toggle.gltf")));

    ASSERT_TRUE(editor.ResizeViewport(256, 256));

    editor.GetCamera().GetCamera().SetPose(Vec3(0, 0, 1.5f), Vec3(0));

    rc::RenderingSettings settings = editor.GetRenderingState().GetDraft();

    settings.algorithm             = rc::RenderAlgorithm::eVoxelGI;

    settings.debug.output          = rc::DebugOutput::eFinal;

    settings.lights.clear();

    settings.lightMarkers                = false;

    settings.cameraLight.enabled         = false;

    settings.environment.intensity       = 0;

    settings.environment.skybox          = false;

    settings.gi.resolution               = 64;

    settings.gi.shadowMapResolution      = 256;

    settings.gi.cone.indirectIntensity   = 0;

    settings.gi.cone.environmentLighting = false;

    settings.gi.cone.emissiveLighting    = false;

    settings.gi.cone.analyticLighting    = true;

    rc::RenderingLight light;

    light.id              = 1;

    light.light.type      = rc::SceneLightType::eDirectional;

    light.light.direction = glm::normalize(Vec3(1, 0, -1));

    light.light.intensity = 3;

    settings.lights.push_back(light);

    ASSERT_TRUE(editor.StageRenderingSettings(settings, true));

    ASSERT_TRUE(editor.UpdateRenderingSettings());

    const char* sources[] = {"directional", "point", "light-ball"};

    for (uint32_t source = 0; source < 3; ++source)
    {
        SCOPED_TRACE(sources[source]);

        light.light.type      = source == 0 ? rc::SceneLightType::eDirectional : rc::SceneLightType::ePoint;

        light.light.position  = Vec3(-0.4f, 0, 0.6f);

        light.light.intensity = source == 0 ? 3.0f : 0.5f;

        light.light.range     = 2;

        settings.lights.clear();

        if (source != 2)
        {
            settings.lights.push_back(light);
        }

        settings.cameraLight.enabled      = source == 2;

        settings.cameraLight.followCamera = false;

        settings.cameraLight.position     = light.light.position;

        settings.cameraLight.intensity    = light.light.intensity;

        settings.cameraLight.range        = light.light.range;

        uint64_t energy[3]{};

        for (uint32_t sample = 0; sample < 3; ++sample)
        {
            settings.gi.cone.shadows = sample != 1;

            ASSERT_TRUE(editor.StageRenderingSettings(settings));

            ASSERT_TRUE(editor.UpdateRenderingSettings());

            EXPECT_FALSE(editor.GetRenderingState().IsPending());

            ASSERT_TRUE(Frame(*device, editor));

            ASSERT_EQ(editor.GetViewport().GetSnapshot().status.effective, rc::RenderAlgorithm::eVoxelGI);

            const HeapVector<uint8_t> pixels = ReadScenePixels(*device, editor.GetViewport().GetRenderView().color);

            energy[sample]                   = PixelEnergy(pixels);

            const char* captureDirectory     = std::getenv("ZEN_EDITOR_SHADOW_CAPTURE_DIR");

            if (captureDirectory != nullptr)
            {
                const std::filesystem::path path =
                    std::filesystem::path(captureDirectory)
                    / (std::string("mesh-shadows-") + sources[source] + "-" + std::to_string(sample) + ".ppm");

                std::ofstream capture(path, std::ios::binary);

                ASSERT_TRUE(capture.is_open());

                capture << "P6\n256 256\n255\n";

                for (size_t index = 0; index < pixels.size(); index += 4)
                {
                    capture.write(reinterpret_cast<const char*>(pixels.data() + index), 3);
                }
            }
        }

        EXPECT_GT(energy[0], 10000u);

        EXPECT_GT(energy[1], energy[0] + 10000u);

        EXPECT_EQ(energy[2], energy[0]);
    }

    DestroyDevice(*device, editor);
}

TEST_P(EditorRendering, LightBallRelightsVoxelsWithoutRevoxelizingAndHoldsPosition)
{
    UniquePtr<rc::RenderDevice> device = CreateDevice(GetParam());

    EditorController editor(*device);

    ASSERT_TRUE(editor.Init());

    ASSERT_TRUE(editor.Load(Fixture("inspection.gltf")));

    ASSERT_TRUE(editor.ResizeViewport(128, 128));

    rc::RenderingSettings settings = editor.GetRenderingState().GetDraft();

    settings.algorithm             = rc::RenderAlgorithm::eVoxelGI;

    settings.debug.output          = rc::DebugOutput::eFinal;

    settings.lights.clear();

    settings.environment.intensity       = 0;

    settings.environment.skybox          = false;

    settings.gi.resolution               = 64;

    settings.gi.shadowMapResolution      = 128;

    settings.gi.cone.environmentLighting = false;

    settings.gi.cone.emissiveLighting    = false;

    settings.gi.cone.analyticLighting    = true;

    settings.gi.cone.shadows             = true;

    settings.cameraLight.enabled         = true;

    settings.cameraLight.followCamera    = false;

    settings.cameraLight.intensity       = 0.01f;

    settings.cameraLight.range           = 0.3f;

    sg::AABB bounds;

    ASSERT_TRUE(editor.GetScene().GetBounds({editor.GetScene().GetGeneration(), 1}, bounds));

    settings.cameraLight.position = bounds.GetCenter() + Vec3(0, 0, 0.08f);

    ASSERT_TRUE(editor.StageRenderingSettings(settings, true));

    ASSERT_TRUE(editor.UpdateRenderingSettings());

    ASSERT_TRUE(Frame(*device, editor));

    ASSERT_EQ(editor.GetViewport().GetSnapshot().status.effective, rc::RenderAlgorithm::eVoxelGI);

    const uint64_t geometry    = device->GetRendererServer()->RequestVoxelizer()->GetGeometryRevision();

    const uint64_t sceneLights = editor.GetViewport().GetRenderScene()->GetLights().GetRevision();

    const uint64_t lighting    = editor.GetViewport().GetRenderScene()->GetLightingRevision();

    const uint64_t lit         = PixelEnergy(ReadBallRadianceSlice(*device, editor));

    EXPECT_GT(lit, 100u);

    editor.GetCamera().OrbitBy(Vec2(0.3f, 0.1f));

    ASSERT_TRUE(Frame(*device, editor));

    EXPECT_EQ(editor.GetViewport().GetRenderScene()->GetLightingRevision(), lighting);

    EXPECT_EQ(PixelEnergy(ReadBallRadianceSlice(*device, editor)), lit);

    settings.cameraLight.position += Vec3(0, 0, 2);

    ASSERT_TRUE(editor.StageRenderingSettings(settings));

    ASSERT_TRUE(editor.UpdateRenderingSettings());

    ASSERT_TRUE(Frame(*device, editor));

    EXPECT_GT(editor.GetViewport().GetRenderScene()->GetLightingRevision(), lighting);

    EXPECT_EQ(PixelEnergy(ReadBallRadianceSlice(*device, editor)), 0u);

    EXPECT_EQ(device->GetRendererServer()->RequestVoxelizer()->GetGeometryRevision(), geometry);

    EXPECT_EQ(editor.GetViewport().GetRenderScene()->GetLights().GetRevision(), sceneLights);

    settings.cameraLight.followCamera = true;

    ASSERT_TRUE(editor.StageRenderingSettings(settings));

    ASSERT_TRUE(editor.UpdateRenderingSettings());

    ASSERT_TRUE(Frame(*device, editor));

    const uint64_t following = editor.GetViewport().GetRenderScene()->GetLightingRevision();

    editor.GetCamera().OrbitBy(Vec2(0.1f, 0));

    ASSERT_TRUE(Frame(*device, editor));

    EXPECT_GT(editor.GetViewport().GetRenderScene()->GetLightingRevision(), following);

    settings.cameraLight.enabled = false;

    ASSERT_TRUE(editor.StageRenderingSettings(settings));

    ASSERT_TRUE(editor.UpdateRenderingSettings());

    ASSERT_TRUE(Frame(*device, editor));

    EXPECT_EQ(PixelEnergy(ReadBallRadianceSlice(*device, editor)), 0u);

    EXPECT_EQ(device->GetRendererServer()->RequestVoxelizer()->GetGeometryRevision(), geometry);

    DestroyDevice(*device, editor);
}

// Opt-in visual check: set ZEN_EDITOR_LIGHT_BALL_SCENE to Sponza.gltf and run
// with --gtest_also_run_disabled_tests --gtest_filter=*LightBallSponzaCapture*.
TEST_P(EditorRendering, DISABLED_LightBallSponzaCapture)
{
    const char* scenePath = std::getenv("ZEN_EDITOR_LIGHT_BALL_SCENE");

    ASSERT_NE(scenePath, nullptr);

    std::error_code directoryError;

    std::filesystem::create_directories("build/rendering-validation", directoryError);

    ASSERT_FALSE(directoryError) << directoryError.message();

    UniquePtr<rc::RenderDevice> device = CreateDevice(GetParam());

    EditorController editor(*device);

    ASSERT_TRUE(editor.Init());

    ASSERT_TRUE(editor.Load(scenePath));

    ASSERT_TRUE(editor.ResizeViewport(800, 600));

    rc::RenderingSettings settings = editor.GetRenderingState().GetDraft();

    settings.algorithm             = rc::RenderAlgorithm::eVoxelGI;

    settings.debug.output          = rc::DebugOutput::eFinal;

    settings.lights.clear();

    settings.lightMarkers                = false;

    settings.environment.intensity       = 0;

    settings.environment.skybox          = false;

    settings.gi.resolution               = 128;

    settings.gi.shadowMapResolution      = 512;

    settings.gi.cone.environmentLighting = false;

    settings.gi.cone.emissiveLighting    = false;

    settings.gi.cone.analyticLighting    = true;

    settings.gi.cone.shadows             = true;

    settings.cameraLight.enabled         = true;

    settings.cameraLight.color           = Vec3(1.0f, 0.15f, 0.03f);

    const sg::AABB bounds                = editor.GetViewport().GetRenderScene()->GetAABB();

    const Vec3 eye                       = bounds.GetCenter() + Vec3(0, -0.08f, 0.11f);

    editor.GetCamera().GetCamera().SetPose(eye, eye + Vec3(0, -0.25f, -1));

    ASSERT_TRUE(editor.StageRenderingSettings(settings, true));

    ASSERT_TRUE(editor.UpdateRenderingSettings());

    uint64_t withoutGI = 0;

    uint64_t withGI    = 0;

    for (uint32_t sample = 0; sample < 4; ++sample)
    {
        settings.gi.cone.indirectIntensity = sample == 0 ? 0.0f : 1.0f;

        if (sample == 2)
        {
            editor.GetCamera().GetCamera().SetPose(eye + Vec3(0.1f, 0, 0), eye + Vec3(0.1f, -0.25f, -1));
        }

        if (sample == 3)
        {
            const rc::SceneUniformData& data =
                *reinterpret_cast<const rc::SceneUniformData*>(editor.GetViewport().GetRenderScene()->GetSceneUniformData());

            settings.cameraLight.position     = Vec3(data.cameraLight.positionRange);

            settings.cameraLight.followCamera = false;

            editor.GetCamera().GetCamera().SetPose(eye + Vec3(0.12f, 0, 0.025f), settings.cameraLight.position);
        }

        ASSERT_TRUE(editor.StageRenderingSettings(settings));

        ASSERT_TRUE(editor.UpdateRenderingSettings());

        ASSERT_TRUE(Frame(*device, editor));

        ASSERT_EQ(editor.GetViewport().GetSnapshot().status.effective, rc::RenderAlgorithm::eVoxelGI);

        const HeapVector<uint8_t> pixels = ReadScenePixels(*device, editor.GetViewport().GetRenderView().color);

        EXPECT_GT(PixelEnergy(pixels), 10000u) << sample;

        if (sample == 0)
        {
            withoutGI = PixelEnergy(pixels);
        }

        if (sample == 1)
        {
            withGI = PixelEnergy(pixels);
        }

        const rc::RenderView& view = editor.GetViewport().GetRenderView();

        const std::string path = "build/rendering-validation/light-ball-" + std::to_string(static_cast<int>(GetParam())) + "-"
                               + std::to_string(sample) + ".ppm";

        std::ofstream image(path, std::ios::binary);

        ASSERT_TRUE(image.is_open());

        image << "P6\n" << view.width << " " << view.height << "\n255\n";

        for (size_t pixel = 0; pixel < pixels.size(); pixel += 4)
        {
            image.write(reinterpret_cast<const char*>(pixels.data() + pixel), 3);
        }
    }

    EXPECT_GT(withGI, withoutGI);

    DestroyDevice(*device, editor);
}

INSTANTIATE_TEST_SUITE_P(Execution, EditorRendering, testing::Values(RHIExecutionMode::eInline, RHIExecutionMode::eThreaded));
} // namespace
} // namespace zen::editor
