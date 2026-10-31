#include "Editor/Services/EditorController.h"
#include "Graphics/RenderCore/V2/RenderGraph/RDGDebugSnapshot.h"
#include "Editor/Model/EditorText.h"
#include "SyntheticEnvironment.h"
#include "Graphics/RenderCore/V2/Renderer/DeferredLightingRenderer.h"
#include "Graphics/RenderCore/V2/Renderer/HybridGIRenderer.h"
#include "Graphics/RenderCore/V2/Renderer/RendererServer.h"
#include "Graphics/RenderCore/V2/Renderer/VoxelizerBase.h"
#include "Graphics/RenderCore/V2/Renderer/VoxelGIRenderer.h"
#include "Graphics/RenderCore/V2/ShaderProgram.h"
#include "Graphics/RenderCore/V2/ScenePresentation.h"
#include "Graphics/RHI/RHIOptions.h"
#include "Graphics/VulkanRHI/VulkanRHI.h"
#include "Graphics/RHI/RHIShaderUtil.h"
#include "Platform/FileSystem.h"
#include "Platform/InputController.h"
#include "SceneGraph/Texture.h"
#include "AssetLib/SceneAssetIO.h"
#include <gtest/gtest.h>
#include <spdlog/sinks/ostream_sink.h>
#include <chrono>
#include <thread>
#include <fstream>
#include <filesystem>
#include <cstdlib>
#include <sstream>
#include <gli/gli.hpp>

namespace zen::runtime
{
// Inspect queued input without depending on the desktop's foreground-window policy.
struct RuntimeWindowTestAccess
{
    static void ProcessShortcuts(RuntimeWindow& session)
    {
        session.ProcessShortcuts();
    }
    static void DeliverWheel(RuntimeWindow& session, float delta)
    {
        platform::InputEvent event;
        event.type   = platform::InputEventType::Wheel;
        event.deltaY = delta;
        session.OnInput(event);
    }
    static float PendingWheel(const RuntimeWindow& session)
    {
        return session.m_wheel;
    }
};
} // namespace zen::runtime

namespace zen::editor
{
namespace
{
#include "EditorRenderingTestSupport.inl"

TEST_P(EditorRendering, RenderGraphCaptureTracksSceneSettingsAndExcludesPresentationOnlyFrames)
{
    platform::NativeWindow    window({"Render graph capture test", true, 400, 300, 0, false});
    EditorRenderingScope      scope(GetParam());
    rc::RenderDevice&         device = *scope.device;
    EditorController&         editor = scope.editor;
    EditorRenderingValidation validation;
    RHIViewport*              presentation = device.CreateViewport(&window, 400, 300, false);
    ASSERT_NE(presentation, nullptr);
    ASSERT_TRUE(editor.Init());
    ASSERT_TRUE(editor.Load(Fixture("inspection.gltf")));
    ASSERT_TRUE(editor.ResizeViewport(96, 64));
    rc::RenderingSettings settings = editor.GetRenderingState().GetDraft();
    settings.algorithm             = rc::RenderAlgorithm::ePBR;
    ASSERT_TRUE(editor.StageRenderingSettings(settings, true));
    ASSERT_TRUE(editor.UpdateRenderingSettings());
    rc::RDGMetricsOptions metrics;
    metrics.logging.enabled     = true;
    metrics.logging.sampleEvery = 1;
    metrics.logging.minInterval = std::chrono::milliseconds(0);
    device.GetRDGMetrics().Configure(metrics);
    rc::RDGDebugSnapshot snapshot;
    ASSERT_EQ(device.GetRendererServer()->DispatchRenderWorkloads(editor.GetViewport().GetRenderView(), nullptr, true,
                                                                  presentation, &snapshot),
              rc::RenderFrameResult::eSubmitted);
    EXPECT_TRUE(snapshot.accepted);
    EXPECT_TRUE(device.GetRDGMetrics().GetLastSnapshot().sceneDispatch);
    const uint64_t firstFrame    = snapshot.frame;
    const uint64_t firstRevision = editor.GetRenderingState().GetAppliedRevision();
    device.NextFrame();
    settings.debug.output = rc::DebugOutput::eDepth;
    ASSERT_TRUE(editor.StageRenderingSettings(settings));
    ASSERT_TRUE(editor.UpdateRenderingSettings());
    EXPECT_GT(editor.GetRenderingState().GetAppliedRevision(), firstRevision);
    ASSERT_EQ(device.GetRendererServer()->DispatchRenderWorkloads(editor.GetViewport().GetRenderView(), nullptr, true,
                                                                  presentation, &snapshot),
              rc::RenderFrameResult::eSubmitted);
    EXPECT_GT(snapshot.frame, firstFrame);
    bool debugPass = false;
    for (const rc::RDGDebugPass& pass : snapshot.passes)
    {
        debugPass |= pass.name == "RenderDebugVisualization";
    }
    EXPECT_TRUE(debugPass);
    const uint64_t capturedFrame = snapshot.frame;
    device.NextFrame();
    ASSERT_EQ(device.GetRendererServer()->DispatchRenderWorkloads(editor.GetViewport().GetRenderView(), nullptr, false,
                                                                  presentation, &snapshot),
              rc::RenderFrameResult::eSubmitted);
    EXPECT_EQ(snapshot.frame, capturedFrame);
    EXPECT_FALSE(device.GetRDGMetrics().GetLastSnapshot().sceneDispatch);
    device.NextFrame();
    device.DestroyViewport(presentation);
}

TEST_P(EditorRendering, PlayWindowReusesSceneAndRestoresAfterPauseResizeAndQuit)
{
    platform::NativeWindow    editorWindow({"Play test editor", true, 400, 300, 0, false});
    EditorRenderingScope      scope(GetParam());
    rc::RenderDevice&         device = *scope.device;
    EditorController&         editor = scope.editor;
    EditorRenderingValidation validation;
    RHIViewport*              editorPresentation = device.CreateViewport(&editorWindow, 400, 300, false);
    ASSERT_NE(editorPresentation, nullptr);
    ASSERT_TRUE(editor.Init());
    ASSERT_TRUE(editor.Load(Fixture("inspection.gltf")));
    ASSERT_TRUE(editor.ResizeViewport(96, 64));
    rc::RenderingSettings settings   = editor.GetRenderingState().GetDraft();
    settings.algorithm               = rc::RenderAlgorithm::ePBR;
    settings.gi.resolution           = 64;
    settings.gi.shadowMapResolution  = 128;
    settings.gi.cone.fieldResolution = 32;
    ASSERT_TRUE(editor.StageRenderingSettings(settings, true));
    ASSERT_TRUE(editor.UpdateRenderingSettings());
    ASSERT_TRUE(Frame(device, editor));
    const rc::RenderScene*          scene      = editor.GetViewport().GetRenderScene();
    RHIBuffer*                      vertices   = scene->GetVertexBuffer();
    RHITexture*                     image      = editor.GetViewport().GetRenderView().color;
    const uint64_t                  generation = editor.GetScene().GetGeneration();
    runtime::RuntimeWindowSettings& run        = editor.GetRunSettings();
    run.windowWidth                            = 320;
    run.windowHeight                           = 240;
    run.renderWidth                            = 96;
    run.renderHeight                           = 64;
    run.followWindow                           = false;
    run.vsync                                  = false;
    settings.gi.shadowMapResolution            = 256;
    ASSERT_TRUE(editor.StageRenderingSettings(settings));
    ASSERT_TRUE(editor.GetRenderingState().NeedsResourceApply());
    ASSERT_TRUE(editor.GetActions().Execute(actions::Play));
    EXPECT_FALSE(editor.GetRenderingState().NeedsResourceApply());
    EXPECT_TRUE(editor.IsPreviewFrozen());
    EXPECT_FALSE(editor.StartOrResumeRuntime());
    EXPECT_FALSE(editor.RequestLoad(Fixture("inspection.gltf")));
    EXPECT_FALSE(editor.GetActions().IsEnabled(actions::FrameAll));
    editor.UpdatePlaySession();
    runtime::RuntimeWindow& play = editor.GetPlaySession();
    ASSERT_NE(play.GetWindow(), nullptr);
    ASSERT_TRUE(play.Render()) << play.GetError();
    ASSERT_EQ(play.GetState(), runtime::RuntimeState::Running);
    EXPECT_EQ(editor.GetViewport().GetRenderScene(), scene);
    EXPECT_EQ(scene->GetVertexBuffer(), vertices);
    EXPECT_EQ(editor.GetViewport().GetRenderView().color, image);
    EXPECT_EQ(editor.GetScene().GetGeneration(), generation);
    ASSERT_TRUE(editor.GetActions().Execute(actions::Pause));
    const uint64_t            frame  = play.GetSceneFrame();
    const double              time   = play.GetSceneSeconds();
    const HeapVector<uint8_t> frozen = ReadScenePixels(device, image);
    play.GetWindow()->SetSize(480, 300);
    platform::NativeWindow::PollEvents();
    for (uint32_t index = 0; index < 4; ++index)
    {
        ASSERT_EQ(device.GetRendererServer()->DispatchRenderWorkloads(editor.GetViewport().GetRenderView(), nullptr, false,
                                                                      editorPresentation),
                  rc::RenderFrameResult::eSubmitted);
        device.NextFrame();
        ASSERT_TRUE(play.Render());
    }
    EXPECT_EQ(play.GetSceneFrame(), frame);
    EXPECT_DOUBLE_EQ(play.GetSceneSeconds(), time);
    EXPECT_EQ(editor.GetViewport().GetRenderView().color, image);
    const HeapVector<uint8_t> paused = ReadScenePixels(device, image);
    EXPECT_TRUE(std::equal(paused.begin(), paused.end(), frozen.begin(), frozen.end()));
    ASSERT_TRUE(editor.GetActions().Execute(actions::Play));
    ASSERT_TRUE(play.Render());
    EXPECT_GT(play.GetSceneFrame(), frame);
    play.GetWindow()->Minimize();
    platform::NativeWindow::PollEvents();
    ASSERT_TRUE(play.GetWindow()->IsMinimized());
    const uint64_t beforeMinimize = play.GetSceneFrame();
    ASSERT_TRUE(play.Render());
    EXPECT_EQ(play.GetSceneFrame(), beforeMinimize);
    play.GetWindow()->Restore();
    platform::NativeWindow::PollEvents();
    ASSERT_TRUE(play.Render());
    EXPECT_GT(play.GetSceneFrame(), beforeMinimize);
    ASSERT_TRUE(editor.GetActions().Execute(actions::Stop));
    editor.UpdatePlaySession();
    EXPECT_EQ(play.GetWindow(), nullptr);
    EXPECT_FALSE(editor.IsPreviewFrozen());
    EXPECT_EQ(editor.GetViewport().GetRenderScene(), scene);
    EXPECT_EQ(scene->GetVertexBuffer(), vertices);
    ASSERT_TRUE(editor.ResizeViewport(160, 96));
    ASSERT_TRUE(Frame(device, editor));
    // Cancel before a native window exists, then open and close through its title bar.
    ASSERT_TRUE(editor.StartOrResumeRuntime());
    ASSERT_TRUE(play.Quit());
    editor.UpdatePlaySession();
    EXPECT_EQ(play.GetWindow(), nullptr);
    run.followWindow = true;
    run.windowWidth  = 333;
    run.windowHeight = 245;
    ASSERT_TRUE(editor.StartOrResumeRuntime());
    editor.UpdatePlaySession();
    ASSERT_TRUE(play.Render());
    const platform::WindowExtent extent = play.GetWindow()->GetFramebufferExtent();
    EXPECT_EQ(editor.GetViewport().GetRenderView().width, extent.width);
    EXPECT_EQ(editor.GetViewport().GetRenderView().height, extent.height);
    for (uint32_t index = 0; index < 32; ++index)
    {
        play.GetWindow()->SetSize(321 + (index % 8) * 13, 241 + (index % 7) * 11);
        platform::NativeWindow::PollEvents();
        ASSERT_TRUE(play.Render()) << play.GetError();
        const platform::WindowExtent resized = play.GetWindow()->GetFramebufferExtent();
        EXPECT_EQ(editor.GetViewport().GetRenderView().width, resized.width);
        EXPECT_EQ(editor.GetViewport().GetRenderView().height, resized.height);
        EXPECT_EQ(scene->GetVertexBuffer(), vertices);
    }
    ASSERT_TRUE(play.Pause());
    const uint64_t            pausedFrame    = play.GetSceneFrame();
    const double              pausedTime     = play.GetSceneSeconds();
    RHITexture*               retainedImage  = editor.GetViewport().GetRenderView().color;
    const HeapVector<uint8_t> retainedPixels = ReadScenePixels(device, retainedImage);
    for (uint32_t index = 0; index < 16; ++index)
    {
        play.GetWindow()->SetSize(350 + (index % 5) * 31, 250 + (index % 4) * 23);
        platform::NativeWindow::PollEvents();
        ASSERT_TRUE(play.Render());
        EXPECT_EQ(editor.GetViewport().GetRenderView().color, retainedImage);
        EXPECT_EQ(play.GetSceneFrame(), pausedFrame);
        EXPECT_DOUBLE_EQ(play.GetSceneSeconds(), pausedTime);
    }
    const HeapVector<uint8_t> resizedPausedPixels = ReadScenePixels(device, retainedImage);
    EXPECT_TRUE(
        std::equal(retainedPixels.begin(), retainedPixels.end(), resizedPausedPixels.begin(), resizedPausedPixels.end()));
    ASSERT_TRUE(play.Resume());
    ASSERT_TRUE(play.Render());
    const platform::WindowExtent resumedExtent = play.GetWindow()->GetFramebufferExtent();
    EXPECT_EQ(editor.GetViewport().GetRenderView().width, resumedExtent.width);
    EXPECT_EQ(editor.GetViewport().GetRenderView().height, resumedExtent.height);
    play.GetWindow()->RequestClose();
    editor.UpdatePlaySession();
    EXPECT_FALSE(editor.IsPreviewFrozen());
    for (uint32_t index = 0; index < 24; ++index)
    {
        ASSERT_TRUE(editor.ResizeViewport(95 + (index % 8) * 17, 65 + (index % 6) * 13));
        ASSERT_TRUE(Frame(device, editor));
        EXPECT_EQ(scene->GetVertexBuffer(), vertices);
    }
    ASSERT_TRUE(editor.StartOrResumeRuntime());
    editor.UpdatePlaySession();
    ASSERT_TRUE(play.Render());
    ASSERT_TRUE(play.Pause());
    play.Shutdown();
    EXPECT_EQ(play.GetWindow(), nullptr);
    EXPECT_EQ(editor.GetViewport().GetRenderScene(), scene);
    EXPECT_TRUE(Frame(device, editor));
}

TEST_P(EditorRendering, PlayDiscardsPausedWheelInput)
{
    EditorRenderingScope scope(GetParam());
    EditorController&    editor = scope.editor;
    ASSERT_TRUE(editor.Init());
    ASSERT_TRUE(editor.Load(Fixture("inspection.gltf")));
    runtime::RuntimeWindowSettings& run = editor.GetRunSettings();
    run.windowWidth                     = 320;
    run.windowHeight                    = 240;
    run.vsync                           = false;
    ASSERT_TRUE(editor.StartOrResumeRuntime());
    editor.UpdatePlaySession();
    runtime::RuntimeWindow& play = editor.GetPlaySession();
    ASSERT_TRUE(play.Render());
    runtime::RuntimeWindowTestAccess::DeliverWheel(play, 1);
    EXPECT_FLOAT_EQ(runtime::RuntimeWindowTestAccess::PendingWheel(play), 1);
    ASSERT_TRUE(play.Pause());
    EXPECT_FLOAT_EQ(runtime::RuntimeWindowTestAccess::PendingWheel(play), 0);
    const Mat4 pausedView = editor.GetCamera().GetCamera().GetViewMatrix();
    runtime::RuntimeWindowTestAccess::DeliverWheel(play, 3);
    EXPECT_FLOAT_EQ(runtime::RuntimeWindowTestAccess::PendingWheel(play), 0);
    ASSERT_TRUE(play.Render());
    EXPECT_EQ(editor.GetCamera().GetCamera().GetViewMatrix(), pausedView);
    ASSERT_TRUE(play.Resume());
    ASSERT_TRUE(play.Render());
    EXPECT_EQ(editor.GetCamera().GetCamera().GetViewMatrix(), pausedView);
    // A new wheel event during active playback must still reach navigation.
    runtime::RuntimeWindowTestAccess::DeliverWheel(play, 2);
    EXPECT_FLOAT_EQ(runtime::RuntimeWindowTestAccess::PendingWheel(play), 2);
}

TEST_P(EditorRendering, PlayDiscardsSpaceTypedWhileDiagnosticsCaptureKeyboard)
{
    EditorRenderingScope scope(GetParam());
    EditorController&    editor = scope.editor;
    ASSERT_TRUE(editor.Init());
    ASSERT_TRUE(editor.Load(Fixture("inspection.gltf")));
    runtime::RuntimeWindowSettings& run = editor.GetRunSettings();
    run.windowWidth                     = 320;
    run.windowHeight                    = 240;
    run.vsync                           = false;
    ASSERT_TRUE(editor.StartOrResumeRuntime());
    editor.UpdatePlaySession();
    runtime::RuntimeWindow& play = editor.GetPlaySession();
    ASSERT_TRUE(play.Render());
    platform::KeyboardMouseInput& keys = platform::KeyboardMouseInput::GetInstance();
    keys.SetUICapture(false, false);
    for (runtime::RuntimeState state : {runtime::RuntimeState::Running, runtime::RuntimeState::Paused})
    {
        ASSERT_EQ(play.GetState(), state);
        play.SetInputCapture(true, true);
        keys.PressKey(platform::Key::Space);
        keys.ReleaseKey(platform::Key::Space);
        runtime::RuntimeWindowTestAccess::ProcessShortcuts(play);
        EXPECT_EQ(play.GetState(), state);
        play.SetInputCapture(false, false);
        runtime::RuntimeWindowTestAccess::ProcessShortcuts(play);
        EXPECT_EQ(play.GetState(), state);
        keys.PressKey(platform::Key::Space);
        keys.ReleaseKey(platform::Key::Space);
        runtime::RuntimeWindowTestAccess::ProcessShortcuts(play);
        EXPECT_NE(play.GetState(), state);
    }
    keys.Reset();
}

TEST_P(EditorRendering, PlayPreservesAnimatedCameraOnReturnAndNavigation)
{
    EditorRenderingScope scope(GetParam());
    EditorController&    editor = scope.editor;
    ASSERT_TRUE(editor.Init());
    ASSERT_TRUE(editor.Load(Fixture("inspection.gltf")));
    const HeapVector<sg::SceneCamera*> cameras = editor.GetScene().Get()->scene->GetComponents<sg::SceneCamera>();
    ASSERT_FALSE(cameras.empty());
    cameras.front()->infiniteFar = true;
    sg::AnimationAsset   animation;
    sg::AnimationSampler sampler;
    sampler.components = 3;
    sampler.times      = {0, 1};
    sampler.values     = {0, 0, 5, 2, 1, 4};
    animation.samplers.push_back(std::move(sampler));
    sg::AnimationChannel channel;
    channel.node = 4; // The fixture's camera node.
    animation.channels.push_back(channel);
    editor.GetScene().Get()->scene->GetAssetData().animations.push_back(std::move(animation));
    ASSERT_TRUE(editor.GetViewport().GetSceneRendering().GetScene()->SetAnimation(0, false));
    runtime::RuntimeWindowSettings& run = editor.GetRunSettings();
    run.windowWidth                     = 320;
    run.windowHeight                    = 240;
    run.vsync                           = false;
    run.fixedStep                       = true;
    ASSERT_TRUE(editor.StartOrResumeRuntime());
    editor.UpdatePlaySession();
    runtime::RuntimeWindow& play = editor.GetPlaySession();
    ASSERT_TRUE(play.Render());
    const Vec3 initialEye = editor.GetCamera().GetCamera().GetPos();
    ASSERT_TRUE(play.Render());
    EXPECT_NE(editor.GetCamera().GetCamera().GetPos(), initialEye);
    const Mat4 animatedView = editor.GetCamera().GetCamera().GetViewMatrix();
    const Vec3 animatedEye  = editor.GetCamera().GetCamera().GetPos();
    ASSERT_TRUE(play.Quit());
    editor.UpdatePlaySession();
    ASSERT_TRUE(editor.ResizeViewport(160, 96));
    EXPECT_TRUE(editor.GetCamera().GetCamera().GetParameters().infiniteFar);
    EXPECT_EQ(editor.GetCamera().GetCamera().GetViewMatrix(), animatedView);
    EXPECT_EQ(editor.GetCamera().GetCamera().GetPos(), animatedEye);
    CameraInput input;
    input.move    = Vec3(0, 1, 0);
    input.seconds = 0.02f;
    editor.GetCamera().Apply(input);
    EXPECT_LT(glm::distance(editor.GetCamera().GetCamera().GetPos(), animatedEye + Vec3(0, 0.02f, 0)), 0.00001f);
    ASSERT_TRUE(Frame(*scope.device, editor));

    // The returned lens must support editor actions as well as navigation.
    const NodeId first{editor.GetScene().GetGeneration(), 1};
    editor.GetSelection().SelectNode(first);
    editor.FrameSelection();
    editor.Pick(Vec2(0.5f));
    EXPECT_EQ(editor.GetSelection().GetNode(), first);
    ResolvePick(*scope.device, editor);
    EXPECT_EQ(editor.GetSelection().GetNode(), first);
    editor.Pick(Vec2(0));
    EXPECT_EQ(editor.GetSelection().GetNode(), first);
    ResolvePick(*scope.device, editor);
    EXPECT_EQ(editor.GetSelection().GetNode(), NodeId{});
    std::string error;
    for (rc::SceneLightType type : {rc::SceneLightType::ePoint, rc::SceneLightType::eSpot, rc::SceneLightType::eDirectional})
    {
        ASSERT_TRUE(editor.AddRenderingLight(type));
        ASSERT_TRUE(rc::ValidateRenderingSettings(editor.GetRenderingState().GetDraft(), error)) << error;
    }
    Vec3 origin;
    Vec3 direction;
    editor.GetCamera().MakeRay(Vec2(0.5f), origin, direction);
    rc::RenderingSettings settings = editor.GetRenderingState().GetDraft();
    settings.cameraLight.enabled   = true;
    settings.cameraLight.position =
        rc::CameraLightPosition(settings.cameraLight, editor.GetCamera().GetCamera().GetPos(), direction);
    settings.cameraLight.followCamera = false;
    ASSERT_TRUE(editor.StageRenderingSettings(settings));
    ASSERT_TRUE(editor.UpdateRenderingSettings());
    editor.FrameAll();
    ASSERT_TRUE(Frame(*scope.device, editor));
}

TEST_P(EditorRendering, PlayFirstFrameMatchesEditorForPBRDepthAndGI)
{
    for (uint32_t variant = 0; variant < 3; ++variant)
    {
        EditorRenderingScope scope(GetParam());
        rc::RenderDevice&    device = *scope.device;
        EditorController&    editor = scope.editor;
        ASSERT_TRUE(editor.Init());
        ASSERT_TRUE(editor.Load(Fixture("inspection.gltf")));
        ASSERT_TRUE(editor.ResizeViewport(96, 64));
        rc::RenderingSettings settings   = editor.GetRenderingState().GetDraft();
        settings.algorithm               = variant == 2 ? rc::RenderAlgorithm::eVoxelGI : rc::RenderAlgorithm::ePBR;
        settings.debug.output            = variant == 1 ? rc::DebugOutput::eDepth : rc::DebugOutput::eFinal;
        settings.gi.resolution           = 64;
        settings.gi.shadowMapResolution  = 128;
        settings.gi.cone.fieldResolution = 32;
        settings.gi.cone.SetAllFeatureProviders(rc::VoxelGISettings::RayProvider::Voxel);
        ASSERT_TRUE(editor.StageRenderingSettings(settings, true));
        ASSERT_TRUE(editor.UpdateRenderingSettings());
        ASSERT_TRUE(FrameWithPreparedGI(device, editor));
        const HeapVector<uint8_t>       expected = ReadScenePixels(device, editor.GetViewport().GetRenderView().color);
        const rc::RenderScene*          scene    = editor.GetViewport().GetRenderScene();
        const rc::VoxelGIRenderer*      gi       = device.GetRendererServer()->RequestVoxelGI();
        runtime::RuntimeWindowSettings& run      = editor.GetRunSettings();
        run.windowWidth                          = 320;
        run.windowHeight                         = 240;
        run.renderWidth                          = 96;
        run.renderHeight                         = 64;
        run.followWindow                         = false;
        run.vsync                                = false;
        ASSERT_TRUE(editor.StartOrResumeRuntime());
        editor.UpdatePlaySession();
        ASSERT_TRUE(editor.GetPlaySession().Render());
        const HeapVector<uint8_t> actual = ReadScenePixels(device, editor.GetViewport().GetRenderView().color);
        ASSERT_EQ(actual.size(), expected.size());
        double totalDifference = 0;
        for (size_t pixel = 0; pixel < actual.size(); ++pixel)
        {
            totalDifference += std::abs(int(expected[pixel]) - int(actual[pixel]));
        }
        EXPECT_LE(totalDifference / actual.size(), variant == 2 ? 2.0 : 0.1) << "variant=" << variant;
        EXPECT_EQ(editor.GetViewport().GetRenderScene(), scene);
        EXPECT_EQ(device.GetRendererServer()->RequestVoxelGI(), gi);
        ASSERT_TRUE(editor.GetPlaySession().Quit());
        editor.UpdatePlaySession();
    }
}


TEST_P(EditorRendering, SceneCompositorRetainsAndLetterboxesWithoutSceneWork)
{
    EditorRenderingScope      scope(GetParam());
    rc::RenderDevice&         device = *scope.device;
    EditorRenderingValidation validation;
    rc::RenderView            source;
    rc::RenderView            destination;
    rc::SceneCompositor       compositor;
    ASSERT_TRUE(rc::ResizeSceneView(device, source, 8, 8));
    ASSERT_TRUE(compositor.Init(device));
    RHITexture* retained = source.color;
    EXPECT_FALSE(rc::ResizeSceneView(device, source, 0, 8));
    EXPECT_EQ(source.color, retained);
    for (uint32_t frame = 0; frame < 4; ++frame)
    {
        const bool wide = frame % 2 == 0;
        ASSERT_TRUE(rc::ResizeSceneView(device, destination, wide ? 16 : 8, wide ? 8 : 16));
        rc::RenderGraph graph("CompositorWithoutSceneWork");
        ASSERT_TRUE(graph.Begin());
        if (frame == 0)
        {
            graph.AddTransferPass("InitializeRetainedImage").ClearTexture(source.color, Color(1, 0, 0, 1));
        }
        ASSERT_TRUE(compositor.BuildRenderGraph(graph, destination, source));
        ASSERT_TRUE(graph.End());
        ASSERT_TRUE(device.ExecuteRenderGraph(graph));
        device.NextFrame();
        const HeapVector<uint8_t> pixels = ReadScenePixels(device, destination.color);
        ASSERT_EQ(pixels.size(), size_t(16 * 8 * 4));
        for (uint32_t y = 0; y < destination.height; ++y)
        {
            for (uint32_t x = 0; x < destination.width; ++x)
            {
                const bool   image = wide ? x >= 4 && x < 12 : y >= 4 && y < 12;
                const size_t pixel = (size_t(y) * destination.width + x) * 4;
                EXPECT_NEAR(pixels[pixel], image ? 255 : 14, 1);
                EXPECT_NEAR(pixels[pixel + 1], image ? 0 : 15, 1);
                EXPECT_NEAR(pixels[pixel + 2], image ? 0 : 19, 1);
            }
        }
        EXPECT_EQ(source.color, retained);
    }
    device.WaitForPreviousFrames();
    rc::DestroySceneView(device, source);
    rc::DestroySceneView(device, destination);
}

TEST_P(EditorRendering, RendererStartsWithExplicitGIAndMarkerSettings)
{
    rc::RenderingSettings initial;
    initial.gi.resolution          = 64;
    initial.gi.shadowMapResolution = 128;
    initial.gi.cone.samples        = 1;
    initial.gi.cone.SetAllFeatureProviders(rc::VoxelGISettings::RayProvider::Voxel);
    initial.gi.cone.indirectIntensity  = 0.375f;
    initial.lightMarkers               = true;
    initial.lightMarkerSize            = 0.047f;
    UniquePtr<rc::RenderDevice> device = CreateDevice(GetParam(), false, &initial);
    const rc::RendererServer&   server = *device->GetRendererServer();
    EXPECT_EQ(server.GetVoxelGISettings().resolution, initial.gi.resolution);
    EXPECT_EQ(server.GetVoxelGISettings().shadowMapResolution, initial.gi.shadowMapResolution);
    EXPECT_EQ(server.RequestVoxelizer()->GetVoxelTexResolution(), initial.gi.resolution);
    EXPECT_EQ(server.RequestVoxelGI()->GetSettings(), initial.gi.cone);
    EXPECT_TRUE(server.RequestDeferredLightingRenderer()->GetLightMarkersEnabled());
    EXPECT_FLOAT_EQ(server.RequestDeferredLightingRenderer()->GetLightMarkerSize(), initial.lightMarkerSize);
    device->WaitForPreviousFrames();
    rc::ShaderProgramManager::GetInstance().Destroy();
    device->Destroy();
}

TEST_P(EditorRendering, PresentationOnlyFramesPreserveFrozenSceneAndRendererState)
{
    EditorRenderingScope scope(GetParam());
    rc::RenderDevice&    device = *scope.device;
    EditorController&    editor = scope.editor;
    ASSERT_TRUE(editor.Init());
    ASSERT_TRUE(editor.Load(Fixture("inspection.gltf")));
    ASSERT_TRUE(editor.ResizeViewport(96, 64));
    rc::RendererServer& server = *device.GetRendererServer();
    for (rc::RenderAlgorithm algorithm : {rc::RenderAlgorithm::ePBR, rc::RenderAlgorithm::eVoxelGI})
    {
        rc::RenderingSettings settings   = editor.GetRenderingState().GetDraft();
        settings.algorithm               = algorithm;
        settings.gi.resolution           = 64;
        settings.gi.shadowMapResolution  = 128;
        settings.gi.cone.fieldResolution = 32;
        settings.gi.cone.SetAllFeatureProviders(rc::VoxelGISettings::RayProvider::Voxel);
        ASSERT_TRUE(editor.StageRenderingSettings(settings, true));
        ASSERT_TRUE(editor.UpdateRenderingSettings()) << editor.GetRenderingState().GetError();
        ASSERT_TRUE(FrameWithPreparedGI(device, editor));
        const rc::DebugOutputDescription before           = server.GetDebugOutputDescription();
        const rc::RenderingStatus        status           = server.GetRenderingStatus();
        rc::HybridGIRenderer*            hybrid           = server.RequestDeferredLightingRenderer()->GetHybridGI();
        const uint32_t                   historyFrame     = hybrid != nullptr ? hybrid->GetFrame() : 0;
        const uint64_t                   geometryRevision = server.RequestVoxelizer()->GetGeometryRevision();
        const HeapVector<uint8_t>        pixels           = ReadScenePixels(device, editor.GetViewport().GetRenderView().color);
        for (uint32_t frame = 0; frame < 12; ++frame)
        {
            ASSERT_EQ(server.DispatchRenderWorkloads(editor.GetViewport().GetRenderView(), nullptr, false),
                      rc::RenderFrameResult::eSubmitted);
            device.NextFrame();
        }
        const rc::DebugOutputDescription after = server.GetDebugOutputDescription();
        EXPECT_EQ(after.output, before.output);
        EXPECT_EQ(after.available, before.available);
        EXPECT_EQ(after.reason, before.reason);
        EXPECT_EQ(after.width, before.width);
        EXPECT_EQ(after.height, before.height);
        EXPECT_EQ(after.interpretation, before.interpretation);
        EXPECT_EQ(server.GetRenderingStatus().effective, status.effective);
        EXPECT_EQ(server.GetRenderingStatus().fallbackReason, status.fallbackReason);
        EXPECT_EQ(server.RequestVoxelizer()->GetGeometryRevision(), geometryRevision);
        if (hybrid != nullptr)
        {
            EXPECT_EQ(hybrid->GetFrame(), historyFrame);
        }
        const HeapVector<uint8_t> frozen = ReadScenePixels(device, editor.GetViewport().GetRenderView().color);
        ASSERT_EQ(frozen.size(), pixels.size());
        EXPECT_TRUE(std::equal(frozen.begin(), frozen.end(), pixels.begin()));
        EXPECT_TRUE(Frame(device, editor));
    }
}

TEST_P(EditorRendering, ApplicationContinuesAfterDeclineAndRecordingFailureButStopsWhenBlocked)
{
    UniquePtr<rc::RenderDevice> device = CreateDevice(GetParam());

    EditorController editor(*device);

    ASSERT_TRUE(editor.Init());

    ASSERT_TRUE(editor.Load(Fixture("inspection.gltf")));

    ASSERT_TRUE(editor.ResizeViewport(320, 240));

    EXPECT_TRUE(Frame(*device, editor));

    EXPECT_TRUE(editor.RecordRenderingResult(rc::RenderFrameResult::eSubmitted));

    for (uint32_t frame = 0; frame < 8; ++frame)
    {
        EXPECT_TRUE(editor.RecordRenderingResult(rc::RenderFrameResult::eDeclined));
    }

    EXPECT_TRUE(Frame(*device, editor));

    EXPECT_TRUE(editor.RecordRenderingResult(rc::RenderFrameResult::eSubmitted));

    EXPECT_TRUE(editor.RecordRenderingResult(rc::RenderFrameResult::eFailed));

    EXPECT_TRUE(Frame(*device, editor));

    for (uint32_t frame = 0; frame < 8; ++frame)
    {
        EXPECT_TRUE(editor.RecordRenderingResult(rc::RenderFrameResult::eDeclined));
    }

    EXPECT_FALSE(editor.RecordRenderingResult(rc::RenderFrameResult::eDeclined));

    EXPECT_FALSE(editor.RecordRenderingResult(rc::RenderFrameResult::eBlocked));

    DestroyDevice(*device, editor);
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
    // Keep the previous Inspector target until the asynchronous ID pass clears it.
    sg::AABB bounds;

    ASSERT_TRUE(editor.GetScene().GetBounds(first, bounds));

    const Vec3 corner = glm::mix(bounds.GetMin(), bounds.GetMax(), Vec3(0.9f, 0.9f, 0.5f));

    const NodeId previous{editor.GetScene().GetGeneration(), 2};
    editor.GetSelection().SelectNode(previous);
    const uint64_t beforePick = editor.GetSelection().GetRevision();
    editor.Pick(Project(editor.GetCamera(), corner));

    EXPECT_EQ(editor.GetSelection().GetNode(), previous);
    EXPECT_EQ(editor.GetSelection().GetRevision(), beforePick);

    ResolvePick(*device, editor);

    EXPECT_EQ(editor.GetSelection().GetNode(), NodeId{});
    EXPECT_EQ(editor.GetSelection().GetRevision(), beforePick + 1);

    // A second click can arrive with the same stamp while the first readback is in flight.
    // Only the latest click may publish, whether it selects a surface or empty space.
    for (bool latestHits : {false, true})
    {
        editor.GetSelection().SelectNode(previous);
        const uint64_t beforeClicks = editor.GetSelection().GetRevision();
        const Vec2     hit(0.5f);
        const Vec2     miss = Project(editor.GetCamera(), corner);
        editor.Pick(latestHits ? miss : hit);
        ASSERT_TRUE(Frame(*device, editor, false));
        EXPECT_TRUE(editor.GetViewport().IsPickPending());
        EXPECT_EQ(editor.GetSelection().GetNode(), previous);
        EXPECT_EQ(editor.GetSelection().GetRevision(), beforeClicks);

        editor.Pick(latestHits ? hit : miss);
        EXPECT_EQ(editor.GetSelection().GetNode(), previous);
        EXPECT_EQ(editor.GetSelection().GetRevision(), beforeClicks);
        ResolvePick(*device, editor);
        EXPECT_EQ(editor.GetSelection().GetNode(), latestHits ? first : NodeId{});
        EXPECT_EQ(editor.GetSelection().GetRevision(), beforeClicks + 1);
    }

    // A pick issued before a resize and a newer selection must not overwrite them.
    editor.Pick(Vec2(0.5f));

    ASSERT_TRUE(Frame(*device, editor, false));

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

TEST_P(EditorRendering, PicksValidateViewAtRecordingAndSurviveLaterNavigation)
{
    UniquePtr<rc::RenderDevice> device = CreateDevice(GetParam());
    EditorController            editor(*device);
    ASSERT_TRUE(editor.Init());
    ASSERT_TRUE(editor.Load(Fixture("inspection.gltf")));
    const NodeId first{editor.GetScene().GetGeneration(), 1};
    const NodeId previous{editor.GetScene().GetGeneration(), 2};

    for (bool recorded : {false, true})
    {
        for (bool resize : {false, true})
        {
            SCOPED_TRACE(testing::Message() << "recorded=" << recorded << " resize=" << resize);
            ASSERT_TRUE(editor.ResizeViewport(320, 240));
            editor.GetSelection().SelectNode(first);
            editor.FrameSelection();
            editor.GetSelection().SelectNode(previous);
            const uint64_t revision = editor.GetSelection().GetRevision();
            editor.Pick(Vec2(0.5f));
            if (recorded)
            {
                ASSERT_TRUE(Frame(*device, editor, false));
            }

            if (resize)
            {
                ASSERT_TRUE(editor.ResizeViewport(411, 267));
            }
            else
            {
                CameraInput input;
                input.dolly = 1;
                editor.GetCamera().Apply(input);
            }

            ResolvePick(*device, editor);
            EXPECT_EQ(editor.GetSelection().GetNode(), recorded ? first : previous);
            EXPECT_EQ(editor.GetSelection().GetRevision(), revision + (recorded ? 1 : 0));
        }
    }

    // A queued click whose view becomes stale must still supersede the older readback.
    editor.GetSelection().SelectNode(first);
    editor.FrameSelection();
    editor.GetSelection().SelectNode(previous);
    const uint64_t beforeClicks = editor.GetSelection().GetRevision();
    editor.Pick(Vec2(0.5f));
    ASSERT_TRUE(Frame(*device, editor, false));
    editor.Pick(Vec2(0));
    CameraInput input;
    input.dolly = 1;
    editor.GetCamera().Apply(input);
    ResolvePick(*device, editor);
    EXPECT_EQ(editor.GetSelection().GetNode(), previous);
    EXPECT_EQ(editor.GetSelection().GetRevision(), beforeClicks);

    // The captured object identity cannot carry over to a replacement scene.
    editor.GetSelection().SelectNode(first);
    editor.FrameSelection();
    editor.GetSelection().SelectNode(previous);
    editor.Pick(Vec2(0.5f));
    ASSERT_TRUE(Frame(*device, editor, false));
    ASSERT_TRUE(editor.Load(Fixture("inspection.gltf")));
    const uint64_t afterLoad = editor.GetSelection().GetRevision();
    ResolvePick(*device, editor);
    EXPECT_EQ(editor.GetSelection().GetNode(), NodeId{});
    EXPECT_EQ(editor.GetSelection().GetRevision(), afterLoad);
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

TEST_P(EditorRendering, MixedFormatsAndCancellationPreserveThePublishedScene)
{
    UniquePtr<rc::RenderDevice> device = CreateDevice(GetParam());
    EditorController            editor(*device);
    ASSERT_TRUE(editor.Init());
    ASSERT_TRUE(editor.ResizeViewport(96, 96));
    ASSERT_TRUE(editor.Load(Fixture("inspection.gltf")));
    const std::string           fbx = std::string(ZEN_EDITOR_SHARED_FIXTURES) + "FBX/maya_cube_7500_binary.fbx";
    const std::string           glb = std::string(ZEN_EDITOR_SHARED_FIXTURES) + "complete_scene.glb";
    const std::filesystem::path internalFile =
        std::filesystem::temp_directory_path()
        / ("zen_editor_internal_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".zenasset");
    const std::string              internal = internalFile.string();
    const asset::SceneImportResult imported = asset::ImportScene({glb});
    ASSERT_TRUE(imported.asset) << imported.error;
    ASSERT_EQ(asset::WriteSceneAssetFile(internal, *imported.asset).status, asset::SceneImportStatus::Success);
    for (SceneLoadStage stage :
         {SceneLoadStage::Queued, SceneLoadStage::Reading, SceneLoadStage::Preparing, SceneLoadStage::Publishing})
    {
        const uint64_t         generation = editor.GetScene().GetGeneration();
        const rc::RenderScene* retained   = editor.GetViewport().GetRenderScene();
        ASSERT_TRUE(editor.RequestLoad(fbx));
        if (stage != SceneLoadStage::Queued)
        {
            ASSERT_TRUE(editor.UpdateSceneLoad());
        }
        if (stage == SceneLoadStage::Preparing || stage == SceneLoadStage::Publishing)
        {
            WaitForImport(editor);
            ASSERT_EQ(editor.GetLoadState().stage, SceneLoadStage::Preparing);
        }
        if (stage == SceneLoadStage::Publishing)
        {
            ASSERT_TRUE(editor.UpdateSceneLoad());
        }
        ASSERT_EQ(editor.GetLoadState().stage, stage);
        ASSERT_TRUE(editor.CancelSceneLoad());
        if (stage == SceneLoadStage::Reading)
        {
            WaitForImport(editor);
        }
        EXPECT_EQ(editor.GetLoadState().stage, SceneLoadStage::Cancelled);
        EXPECT_EQ(editor.GetScene().GetGeneration(), generation);
        EXPECT_EQ(editor.GetViewport().GetRenderScene(), retained);
        EXPECT_TRUE(editor.GetError().empty());
        ASSERT_TRUE(Frame(*device, editor));
        editor.UpdateSceneLoad();
    }
    for (uint32_t cycle = 0; cycle < 2; ++cycle)
    {
        for (const std::string& path : {fbx, glb, internal, Fixture("inspection.gltf")})
        {
            ASSERT_TRUE(editor.RequestLoad(path));
            ASSERT_TRUE(editor.UpdateSceneLoad());
            WaitForImport(editor);
            ASSERT_EQ(editor.GetLoadState().stage, SceneLoadStage::Preparing) << editor.GetError();
            ASSERT_TRUE(editor.UpdateSceneLoad());
            ASSERT_TRUE(Frame(*device, editor));
            ASSERT_TRUE(editor.UpdateSceneLoad());
            EXPECT_EQ(editor.GetLoadState().stage, SceneLoadStage::Complete);
            EXPECT_EQ(editor.GetScene().Get()->path, path);
            if (path == fbx)
            {
                EXPECT_FALSE(editor.GetImportDiagnostics().empty());
            }
            ASSERT_TRUE(Frame(*device, editor));
            editor.UpdateSceneLoad();
        }
    }
    const uint64_t         retainedGeneration = editor.GetScene().GetGeneration();
    const rc::RenderScene* retainedScene      = editor.GetViewport().GetRenderScene();
    {
        std::ofstream corrupt(internalFile, std::ios::binary | std::ios::trunc);
        corrupt << "invalid internal asset";
    }
    ASSERT_TRUE(editor.RequestLoad(internal));
    editor.UpdateSceneLoad();
    WaitForImport(editor);
    EXPECT_EQ(editor.GetLoadState().stage, SceneLoadStage::Failed);
    EXPECT_EQ(editor.GetScene().GetGeneration(), retainedGeneration);
    EXPECT_EQ(editor.GetViewport().GetRenderScene(), retainedScene);
    ASSERT_TRUE(Frame(*device, editor));
    std::error_code error;
    std::filesystem::remove(internalFile, error);
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

    // Vulkan's guaranteed descriptor set minimum: the shader must load on every device.
    ASSERT_TRUE(RHIShaderUtil::ReflectShaderGroupInfo(spirv, RHIGPUInfo{}.maxBoundDescriptorSets, info));

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

    for (uint32_t scenario = 0; scenario < 4; ++scenario)
    {
        std::string error;

        UniquePtr<LoadedScene> candidate = ParseScene(Fixture("textured.gltf"), error);

        ASSERT_TRUE(candidate);

        const HeapVector<sg::Texture*> textures = candidate->scene->GetComponents<sg::Texture>();

        ASSERT_FALSE(textures.empty());

        if (scenario < 3)
        {
            // Allocation succeeds, but the copy cannot cover the declared image.
            textures[0]->bytesData.resize(1);
            if (scenario > 0)
            {
                textures[0]->width = textures[0]->height = 1;
                textures[0]->format = scenario == 1 ? asset::Format::BC4_UNORM_BLOCK : asset::Format::BC5_UNORM_BLOCK;
                // BC4 is truncated; BC5 has a complete block but no normal interpretation.
                textures[0]->bytesData.resize(scenario == 1 ? 7u : 16u);
                textures[0]->interpretation = asset::TextureInterpretation::Default;
            }

            textures[0]->mipBytes.clear();
        }
        else
        {
            sg::ImageBasedLightAsset light;
            light.size = light.mipLevels = 1;
            light.specularFormat         = asset::Format::BC6H_UFLOAT_BLOCK;
            light.irradianceCoefficients = HeapVector<Vec3>(9, Vec3(0));
            SharedPtr<HeapVector<HeapVector<uint8_t>>, MultiThreadCounter> blocks =
                MakeShared<HeapVector<HeapVector<uint8_t>>, MultiThreadCounter>();
            *blocks = HeapVector<HeapVector<uint8_t>>(6, HeapVector<uint8_t>(16, 0));
            blocks->back().pop_back();
            light.specularBlockMipFaces = blocks;
            candidate->scene->GetAssetData().imageBasedLights.push_back(std::move(light));
            candidate->scene->GetAssetData().imageBasedLight = 0;
        }

        EXPECT_FALSE(editor.GetViewport().PrepareScene(*candidate, error));

        EXPECT_FALSE(error.empty());

        candidate.Reset();

        EXPECT_EQ(editor.GetViewport().GetRenderScene(), active);

        EXPECT_EQ(editor.GetScene().GetGeneration(), generation);

        EXPECT_EQ(editor.GetSelection().GetNode(), selection);

        EXPECT_TRUE(Frame(*device, editor));

        EXPECT_TRUE(device->GetCurrentFrameRDG()->GetWarnings().empty());
    }

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

    ASSERT_TRUE(FrameWithPreparedGI(*device, editor));

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

TEST_P(EditorRendering, ImportedLightsReplaceEnvironmentLightingAcrossSceneChanges)
{
    UniquePtr<rc::RenderDevice> device = CreateDevice(GetParam());

    EditorController editor(*device);

    ASSERT_TRUE(editor.Init());

    ASSERT_TRUE(editor.Load(Fixture("textured.gltf")));

    ASSERT_TRUE(editor.GetViewport().SetEnvironmentLighting(2.0f, 45.0f, true, false));

    for (const char* name : {"inspection.gltf", "textured.gltf"})
    {
        ASSERT_TRUE(editor.Load(Fixture(name)));

        const rc::RenderScene* scene = editor.GetViewport().GetRenderScene();

        ASSERT_NE(scene, nullptr);

        const bool imported = scene->HasImportedLights();

        EXPECT_EQ(imported, std::string(name) == "inspection.gltf");

        const rc::SceneUniformData& uniform = *reinterpret_cast<const rc::SceneUniformData*>(scene->GetSceneUniformData());

        // Imported lights replace the environment light; the session's preview values survive.
        EXPECT_FLOAT_EQ(uniform.environment.z, imported ? 0.0f : 1.0f);

        EXPECT_FLOAT_EQ(uniform.environment.x, 2.0f);

        EXPECT_FLOAT_EQ(uniform.environment.y, glm::radians(45.0f));

        EXPECT_FLOAT_EQ(uniform.environment.w, 0.0f);

        EXPECT_EQ(editor.GetRenderingState().GetApplied().environment.lighting, !imported);

        EXPECT_EQ(editor.GetRenderingDefaults().environment.lighting, !imported);

        ASSERT_TRUE(Frame(*device, editor));
    }

    DestroyDevice(*device, editor);
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

TEST_P(EditorRendering, ManualLightPositionsPreviewAndRestoreWithoutChangingRuntimeIdentity)
{
    UniquePtr<rc::RenderDevice> device = CreateDevice(GetParam());
    EditorController            editor(*device);
    ASSERT_TRUE(editor.Init());
    ASSERT_TRUE(editor.Load(Fixture("inspection.gltf")));
    ASSERT_TRUE(editor.ResizeViewport(128, 128));

    for (const rc::SceneLightType type : {rc::SceneLightType::ePoint, rc::SceneLightType::eSpot})
    {
        ASSERT_TRUE(editor.AddRenderingLight(type));
        ASSERT_TRUE(editor.UpdateRenderingSettings());
        rc::RenderingSettings draft = editor.GetRenderingState().GetDraft();
        const Vec3            start = draft.lights.back().light.position;
        const Vec4 clip = editor.GetCamera().GetCamera().GetProjectionMatrix() * editor.GetCamera().GetCamera().GetViewMatrix()
                        * Vec4(start, 1);
        EXPECT_GT(clip.w, 0);
        EXPECT_NEAR(clip.x / clip.w, 0, 1e-4f);
        EXPECT_NEAR(clip.y / clip.w, 0, 1e-4f);
        EXPECT_GT(clip.z / clip.w, 0);
        EXPECT_LT(clip.z / clip.w, 1);

        const rc::LightId runtimeId = editor.GetViewport().GetRenderScene()->GetLights().GetEntries().back().id;
        const uint32_t    grid      = editor.GetViewport().GetSnapshot().settings.resolution;
        draft.gi.resolution         = grid == 64 ? 128 : 64;
        // Repeated drag previews and cancellation also work with pending resource edits.
        for (const Vec3 position : {start + Vec3(0.1f, 0.05f, 0), start + Vec3(0.2f, 0.1f, 0), start})
        {
            draft.lights.back().light.position = position;
            ASSERT_TRUE(editor.StageRenderingSettings(draft));
            ASSERT_TRUE(editor.UpdateRenderingSettings());
            const rc::LightEntry& runtime = editor.GetViewport().GetRenderScene()->GetLights().GetEntries().back();
            EXPECT_EQ(runtime.id, runtimeId);
            EXPECT_EQ(runtime.light.position, position);
            EXPECT_EQ(editor.GetRenderingState().GetApplied().lights.back().light.position, position);
            EXPECT_EQ(editor.GetViewport().GetSnapshot().settings.resolution, grid);
            EXPECT_TRUE(editor.GetRenderingState().NeedsResourceApply());
            ASSERT_TRUE(Frame(*device, editor));
            EXPECT_TRUE(device->GetCurrentFrameRDG()->GetWarnings().empty());
        }
        editor.RevertRenderingSettings();
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

    ASSERT_TRUE(FrameWithPreparedGI(*device, editor));

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
    UniquePtr<rc::RenderDevice> device          = CreateDevice(GetParam());
    const rc::RDGMetricsOptions previousMetrics = device->GetRDGMetrics().GetOptions();

    EditorController editor(*device);

    ASSERT_TRUE(editor.Init());

    ASSERT_TRUE(editor.Load(Fixture("inspection.gltf")));

    editor.GetSelection().SelectNode({editor.GetScene().GetGeneration(), 1});

    editor.FrameSelection();

    ASSERT_TRUE(editor.ResizeViewport(96, 64));

    rc::RenderingSettings settings  = editor.GetRenderingState().GetDraft();

    settings.algorithm              = rc::RenderAlgorithm::ePBR;

    settings.gi.resolution          = 64;

    settings.gi.shadowMapResolution = 128;

    // The rejection view reports per-pixel gather reasons; screen probes have none.
    settings.gi.cone.gather                = rc::VoxelGISettings::Gather::Pixel;

    settings.debug.lightId                 = settings.lights.front().id;

    settings.lights.front().light.position = editor.GetCamera().GetCamera().GetPos();

    settings.debug.face                    = 5;

    HeapVector<uint8_t>       albedo;
    HeapVector<rc::RDGResult> giWarnings;

    sg::AABB selectedBounds;

    ASSERT_TRUE(editor.GetScene().GetBounds(editor.GetSelection().GetNode(), selectedBounds));

    const Vec4 centerView = editor.GetCamera().GetCamera().GetViewMatrix() * Vec4(selectedBounds.GetCenter(), 1);

    for (const rc::DebugOutput output :
         {rc::DebugOutput::eFinal, rc::DebugOutput::eAlbedo, rc::DebugOutput::eNormal, rc::DebugOutput::eDepth,
          rc::DebugOutput::eShadow, rc::DebugOutput::eVoxelSlice, rc::DebugOutput::eVoxels, rc::DebugOutput::eGIRawSky,
          rc::DebugOutput::eGIReconstructedSky, rc::DebugOutput::eGIHistoryLength, rc::DebugOutput::eGIHitMiss,
          rc::DebugOutput::eGIRejection})
    {
        settings.debug.output = output;
        settings.algorithm = output >= rc::DebugOutput::eGIRawSky ? rc::RenderAlgorithm::eVoxelGI : rc::RenderAlgorithm::ePBR;

        if (output == rc::DebugOutput::eGIRawSky)
        {
            editor.GetViewport().SetGPUTimingsEnabled(true);
            // Compare diagnostics against the same view's ordinary GI graph. Existing
            // conditional-history content warnings must not hide a new debug dependency warning.
            settings.debug.output = rc::DebugOutput::eFinal;
            ASSERT_TRUE(editor.StageRenderingSettings(settings, true));
            ASSERT_TRUE(editor.UpdateRenderingSettings());
            ASSERT_TRUE(FrameWithPreparedGI(*device, editor));
            device->GetRDGMetrics().RequestCapture();
            ASSERT_TRUE(Frame(*device, editor));
            giWarnings            = device->GetCurrentFrameRDG()->GetWarnings();
            settings.debug.output = output;
        }

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

        if (output >= rc::DebugOutput::eGIRawSky)
        {
            for (const rc::RDGResult& warning : device->GetCurrentFrameRDG()->GetWarnings())
            {
                bool existing = false;
                for (const rc::RDGResult& baseline : giWarnings)
                {
                    existing |= warning.code == baseline.code && warning.message == baseline.message;
                }
                EXPECT_TRUE(existing) << warning.message;
            }
        }
        else
        {
            EXPECT_TRUE(device->GetCurrentFrameRDG()->GetWarnings().empty());
        }

        const HeapVector<uint8_t> pixels = ReadScenePixels(*device, editor.GetViewport().GetRenderView().color);

        EXPECT_EQ(pixels.size(), 96u * 64 * 4);

        if (output == rc::DebugOutput::eGIHistoryLength || output == rc::DebugOutput::eGIRejection)
        {
            const size_t center = (32 * 96 + 48) * 4;
            if (output == rc::DebugOutput::eGIHistoryLength)
            {
                EXPECT_GT(pixels[center], 0);
                EXPECT_LT(pixels[center], 255);
                EXPECT_EQ(pixels[center], pixels[center + 1]);
                EXPECT_EQ(pixels[center], pixels[center + 2]);
            }
            else
            {
                // Static surface history survives switching diagnostic displays.
                EXPECT_EQ(pixels[center], 0);
                EXPECT_EQ(pixels[center + 1], 255);
                EXPECT_EQ(pixels[center + 2], 0);
            }
        }

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

    const EditorRenderSnapshot timingSnapshot = editor.GetViewport().GetSnapshot();
    EXPECT_TRUE(timingSnapshot.gpuTimingsEnabled);
    EXPECT_FALSE(timingSnapshot.giPassTimings.empty());
    for (const rc::RDGNodeMetrics& timing : timingSnapshot.giPassTimings)
    {
        EXPECT_EQ(timing.gpuStatus, RHIGPUTimingStatus::eAvailable);
        EXPECT_GE(timing.gpuUs, 0.0);
    }
    editor.GetViewport().SetGPUTimingsEnabled(false);
    EXPECT_FALSE(editor.GetViewport().GetSnapshot().gpuTimingsEnabled);
    EXPECT_EQ(device->GetRDGMetrics().GetOptions().gpuTimings, previousMetrics.gpuTimings);
    EXPECT_EQ(device->GetRDGMetrics().GetOptions().logging.enabled, previousMetrics.logging.enabled);

    settings.debug.output  = rc::DebugOutput::eDepth;

    settings.algorithm     = rc::RenderAlgorithm::ePBR;

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

    // Point and line draws still render forward; the G-buffer shades the scene's triangles.
    ASSERT_TRUE(editor.Load(std::string(ZEN_EDITOR_SHARED_FIXTURES) + "all_primitive_modes.gltf"));

    settings              = editor.GetRenderingState().GetDraft();

    settings.debug.output = rc::DebugOutput::eNormal;

    ASSERT_TRUE(editor.StageRenderingSettings(settings));

    ASSERT_TRUE(editor.UpdateRenderingSettings());

    ASSERT_TRUE(Frame(*device, editor));

    EXPECT_TRUE(editor.GetViewport().GetSnapshot().debug.available);

    // An opaque material extension needs forward shading, so this scene has no G-buffer.
    ASSERT_TRUE(editor.Load(Fixture("forward_lit.gltf")));

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

TEST_P(EditorRendering, LightingBackendsSwitchIndependently)
{
    UniquePtr<rc::RenderDevice> device = CreateDevice(GetParam(), true);
    EditorController            editor(*device);
    ASSERT_TRUE(editor.Init());
    ASSERT_TRUE(editor.Load(Fixture("shadow_toggle.gltf")));
    ASSERT_TRUE(editor.ResizeViewport(96, 64));
    rc::RendererServer&   server    = *device->GetRendererServer();
    rc::RenderingSettings settings  = editor.GetRenderingState().GetDraft();
    settings.algorithm              = rc::RenderAlgorithm::eVoxelGI;
    settings.debug.output           = rc::DebugOutput::eFinal;
    settings.gi.resolution          = 64;
    settings.gi.shadowMapResolution = 128;
    settings.gi.cone                = rc::VoxelGISettings{};
    const bool     hardware         = device->GetGPUInfo().rayQuery.IsUsable();
    const uint32_t featureCount     = static_cast<uint32_t>(rc::VoxelGISettings::Feature::Count);
    // All compute, each feature alone on hardware, all hardware, then back to compute.
    for (uint32_t frame = 0; frame < featureCount + 3; ++frame)
    {
        SCOPED_TRACE(frame);
        settings.gi.cone.SetAllFeatureProviders(rc::VoxelGISettings::RayProvider::Voxel);
        if (frame >= 1 && frame <= featureCount)
        {
            settings.gi.cone.SetFeatureProvider(static_cast<rc::VoxelGISettings::Feature>(frame - 1),
                                                rc::VoxelGISettings::RayProvider::Hardware);
        }
        else if (frame == featureCount + 1)
        {
            settings.gi.cone.SetAllFeatureProviders(rc::VoxelGISettings::RayProvider::Hardware);
        }
        ASSERT_TRUE(editor.StageRenderingSettings(settings, true));
        ASSERT_TRUE(editor.UpdateRenderingSettings()) << editor.GetRenderingState().GetError();
        ASSERT_TRUE(Frame(*device, editor));
        const rc::VoxelGIRenderer&                  gi       = *server.RequestVoxelGI();
        const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (gi.IsPreparingField() && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            ASSERT_TRUE(Frame(*device, editor));
        }
        ASSERT_FALSE(gi.IsPreparingField());
        EXPECT_EQ(server.GetRenderOption(), rc::RenderOption::eVoxelGI);
        EXPECT_EQ(gi.GetHardwareFeatureMask(), hardware ? settings.gi.cone.GetHardwareFeatureMask() : 0u);
        EXPECT_EQ(gi.UsesHardwareShadows(), hardware && (frame == featureCount || frame == featureCount + 1));
        EXPECT_TRUE(gi.UsesRayBounce());
        // Compute direct shadows use maps.
        EXPECT_EQ(gi.UsesRayShadows(), gi.UsesHardwareShadows());
        EXPECT_TRUE(gi.UsesReflections());
        // Hardware frames keep the prepared field current for a later Compute selection.
        EXPECT_EQ(gi.UsesFieldTraversal(), !hardware || frame != featureCount + 1);
        ASSERT_NE(gi.GetDistanceField(), nullptr);
        EXPECT_EQ(gi.GetDistanceField()->GetState(), rc::DistanceFieldScene::State::Ready);
    }
    DestroyDevice(*device, editor);
}

bool RenderUntilComputeReady(rc::RenderDevice&          device,
                             EditorController&          editor,
                             const rc::VoxelGISettings& requested,
                             std::ofstream&             csv,
                             const char*                stage,
                             bool                       hardwareWhilePreparing = true,
                             uint32_t*                  framesToReady          = nullptr)
{
    const std::chrono::steady_clock::time_point start    = std::chrono::steady_clock::now();
    const std::chrono::steady_clock::time_point deadline = start + std::chrono::seconds(180);
    rc::VoxelGIRenderer&                        gi       = *device.GetRendererServer()->RequestVoxelGI();
    bool                                        valid    = true;
    bool                                        ready    = false;
    uint32_t                                    frame    = 0;
    while (valid && !ready && std::chrono::steady_clock::now() < deadline)
    {
        const std::chrono::steady_clock::time_point frameStart = std::chrono::steady_clock::now();
        valid                                                  = Frame(device, editor);
        const std::chrono::steady_clock::time_point now        = std::chrono::steady_clock::now();
        const bool                                  pending    = gi.IsPreparingField();
        const uint64_t sceneRevision                           = editor.GetViewport().GetRenderScene()->GetGeometryRevision();
        EXPECT_EQ(gi.GetSettings(), requested);
        if (pending)
        {
            EXPECT_EQ(gi.GetHardwareFeatureMask() != 0, hardwareWhilePreparing);
            EXPECT_EQ(device.GetRendererServer()->GetRenderOption(), rc::RenderOption::eVoxelGI);
            EXPECT_TRUE(device.GetRendererServer()->GetRenderingStatus().fallbackReason.empty());
        }
        ready = !pending && gi.GetHardwareFeatureMask() == 0;
        if (csv.is_open())
        {
            csv << stage << ',' << frame << ',' << std::chrono::duration<double, std::milli>(now - start).count() << ','
                << std::chrono::duration<double, std::milli>(now - frameStart).count() << ',' << pending << ','
                << gi.GetHardwareFeatureMask() << ',' << requested.GetHardwareFeatureMask() << ','
                << gi.GetVisibilityGeneration() << ',' << sceneRevision << ','
                << editor.GetViewport().GetRenderView().color->GetWidth() << ','
                << editor.GetViewport().GetRenderView().color->GetHeight() << ','
                << editor.GetRenderingState().GetDraft().gi.shadowMapResolution << ',' << requested.fieldResolution << '\n';
            csv.flush();
        }
        if (ready)
        {
            EXPECT_TRUE(gi.UsesFieldTraversal());
            EXPECT_FALSE(gi.UsesRayShadows());
            EXPECT_NE(gi.GetVisibilityGeneration(), 0u);
            if (framesToReady != nullptr)
            {
                *framesToReady = frame + 1;
            }
        }
        else
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ++frame;
    }
    EXPECT_TRUE(valid);
    EXPECT_TRUE(ready) << "Compute preparation did not finish within 180 seconds";
    return valid && ready;
}

bool OpenGITimingCsv(std::ofstream& csv, RHIExecutionMode mode, const char* suffix = "")
{
    const char* path = std::getenv("ZEN_EDITOR_GI_SWITCH_CSV");
    if (path != nullptr)
    {
        csv.open(std::string(path) + suffix + (mode == RHIExecutionMode::eInline ? ".inline.csv" : ".threaded.csv"));
        csv << "stage,frame,elapsed_ms,frame_ms,pending,effective_hw_mask,requested_hw_mask,visibility_generation,scene_revision,viewport_width,viewport_height,shadow_map_resolution,field_resolution\n";
        csv.flush();
    }
    return path == nullptr || csv.is_open();
}

TEST_P(EditorRendering, ColdComputeGIShowsBounceBeforeFieldPublication)
{
    EditorRenderingScope scope(GetParam());
    rc::RenderDevice&    device = *scope.device;
    EditorController&    editor = scope.editor;
    ASSERT_TRUE(editor.Init());
    uint64_t brightness[2]{};
    for (uint32_t sample = 0; sample < 2; ++sample)
    {
        // Each sample opens a fresh scene, so both images are the first GI frame.
        ASSERT_TRUE(editor.Load(Fixture("shadow_toggle.gltf")));
        ASSERT_TRUE(editor.ResizeViewport(128, 96));
        rc::RenderingSettings settings  = editor.GetRenderingState().GetDraft();
        settings.algorithm              = rc::RenderAlgorithm::eVoxelGI;
        settings.debug.output           = rc::DebugOutput::eFinal;
        settings.gi.resolution          = 64;
        settings.gi.shadowMapResolution = 128;
        settings.gi.cone.SetAllFeatureProviders(rc::VoxelGISettings::RayProvider::Voxel);
        settings.gi.cone.indirectIntensity = float(sample);
        settings.environment.intensity     = 0;
        settings.environment.skybox        = false;
        settings.lightMarkers              = false;
        settings.lights.clear();
        settings.cameraLight.enabled   = true;
        settings.cameraLight.intensity = 3.0f;
        settings.cameraLight.range     = 4.0f;
        ASSERT_TRUE(editor.StageRenderingSettings(settings, true));
        ASSERT_TRUE(editor.UpdateRenderingSettings());
        ASSERT_TRUE(Frame(device, editor));
        rc::RendererServer&        server = *device.GetRendererServer();
        const rc::VoxelGIRenderer& gi     = *server.RequestVoxelGI();
        ASSERT_TRUE(gi.IsPreparingField());
        EXPECT_FALSE(gi.UsesFieldTraversal());
        EXPECT_FALSE(gi.UsesHardwareQueries());
        EXPECT_EQ(server.GetRenderOption(), rc::RenderOption::eVoxelGI);
        EXPECT_TRUE(server.GetRenderingStatus().fallbackReason.empty());
        EXPECT_GT(gi.GetRadianceGeneration(), 0u);
        const HeapVector<uint8_t> pixels = ReadScenePixels(device, editor.GetViewport().GetRenderView().color);
        for (size_t index = 0; index < pixels.size(); ++index)
        {
            brightness[sample] += index % 4 != 3 ? pixels[index] : 0;
        }
    }
    EXPECT_GT(brightness[1], brightness[0]);
}

TEST_P(EditorRendering, ComputeSceneReplacementKeepsRenderingWhilePreparingGeometry)
{
    EditorRenderingScope scope(GetParam(), true);
    rc::RenderDevice&    device = *scope.device;
    EditorController&    editor = scope.editor;
    ASSERT_TRUE(editor.Init());
    const char* previousPath = std::getenv("ZEN_EDITOR_GI_SWITCH_PREVIOUS_SCENE");
    const char* nextPath     = std::getenv("ZEN_EDITOR_GI_SWITCH_SCENE");
    ASSERT_TRUE(editor.Load(previousPath != nullptr ? std::string(previousPath) : Fixture("shadow_toggle.gltf")));
    ASSERT_TRUE(editor.ResizeViewport(nextPath != nullptr ? 1600 : 96, nextPath != nullptr ? 900 : 64));
    rc::RenderingSettings settings  = editor.GetRenderingState().GetDraft();
    settings.algorithm              = rc::RenderAlgorithm::eVoxelGI;
    settings.debug.output           = rc::DebugOutput::eFinal;
    settings.gi.resolution          = 64;
    settings.gi.shadowMapResolution = nextPath != nullptr ? 1024 : 128;
    settings.gi.cone                = rc::VoxelGISettings{};
    settings.gi.cone.SetAllFeatureProviders(rc::VoxelGISettings::RayProvider::Voxel);
    settings.gi.cone.SetFeatureProvider(rc::VoxelGISettings::Feature::Shadows, rc::VoxelGISettings::RayProvider::Auto);
    settings.gi.cone.samples          = 1;
    settings.gi.cone.adaptiveSampling = false;
    settings.gi.cone.fieldResolution  = 64;
    settings.gi.cone.gather           = rc::VoxelGISettings::Gather::Probes;
    ASSERT_TRUE(editor.StageRenderingSettings(settings, true));
    ASSERT_TRUE(editor.UpdateRenderingSettings());
    rc::RendererServer&  server = *device.GetRendererServer();
    rc::VoxelGIRenderer& gi     = *server.RequestVoxelGI();
    ASSERT_TRUE(Frame(device, editor));
    EXPECT_TRUE(gi.IsPreparingField());
    EXPECT_EQ(server.GetRenderOption(), rc::RenderOption::eVoxelGI);
    EXPECT_FALSE(gi.UsesHardwareQueries());
    EXPECT_EQ(server.RequestDeferredLightingRenderer()->GetHybridGI(), nullptr);
    EXPECT_GT(gi.GetRadianceGeneration(), 0u);
    std::ofstream csv;
    ASSERT_TRUE(OpenGITimingCsv(csv, GetParam(), ".scene-switch"));
    ASSERT_TRUE(RenderUntilComputeReady(device, editor, settings.gi.cone, csv, "initial", false));

    const uint64_t generation = editor.GetScene().GetGeneration();
    ASSERT_TRUE(editor.RequestLoad(nextPath != nullptr ? std::string(nextPath) : Fixture("inspection.gltf")));
    const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(180);
    while (editor.GetLoadState().IsActive() && std::chrono::steady_clock::now() < deadline)
    {
        const SceneLoadStage                        previousStage = editor.GetLoadState().stage;
        const std::chrono::steady_clock::time_point start         = std::chrono::steady_clock::now();
        editor.UpdateSceneLoad();
        const double updateMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        const std::chrono::steady_clock::time_point frameStart = std::chrono::steady_clock::now();
        ASSERT_TRUE(Frame(device, editor));
        if (previousStage != editor.GetLoadState().stage)
        {
            std::printf("Scene switch stage %u -> %u: update %.3f ms, frame %.3f ms, GI pending %u\n",
                        static_cast<uint32_t>(previousStage), static_cast<uint32_t>(editor.GetLoadState().stage), updateMs,
                        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - frameStart).count(),
                        gi.IsPreparingField() ? 1u : 0u);
            std::fflush(stdout);
        }
        if (previousStage == SceneLoadStage::Publishing)
        {
            // The replacement gets its own voxel GI preview while its field builds.
            EXPECT_TRUE(gi.IsPreparingField());
            EXPECT_EQ(gi.GetVisibilityGeneration(), 0u);
            EXPECT_EQ(server.GetRenderOption(), rc::RenderOption::eVoxelGI);
            EXPECT_EQ(server.RequestDeferredLightingRenderer()->GetHybridGI(), nullptr);
            EXPECT_GT(gi.GetRadianceGeneration(), 0u);
        }
        EXPECT_EQ(gi.GetSettings(), settings.gi.cone);
        EXPECT_TRUE(server.GetRenderingStatus().fallbackReason.empty());
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_EQ(editor.GetLoadState().stage, SceneLoadStage::Idle) << editor.GetError();
    ASSERT_EQ(editor.GetScene().GetGeneration(), generation + 1);
    ASSERT_TRUE(RenderUntilComputeReady(device, editor, settings.gi.cone, csv, "replacement", false));
    EXPECT_EQ(server.GetRenderOption(), rc::RenderOption::eVoxelGI);
    EXPECT_TRUE(Frame(device, editor));
    EXPECT_FALSE(gi.IsPreparingField());
}

TEST_P(EditorRendering, ColdHardwareToComputeSwitchKeepsRenderingUntilReady)
{
    UniquePtr<rc::RenderDevice> device = CreateDevice(GetParam(), true);
    EditorController            editor(*device);
    ASSERT_TRUE(editor.Init());
    if (device->GetGPUInfo().rayQuery.IsUsable())
    {
        const char* scenePath = std::getenv("ZEN_EDITOR_GI_SWITCH_SCENE");
        ASSERT_TRUE(editor.Load(scenePath != nullptr ? std::string(scenePath) : Fixture("shadow_toggle.gltf")));
        ASSERT_TRUE(editor.ResizeViewport(scenePath != nullptr ? 1600 : 96, scenePath != nullptr ? 900 : 64));
        rc::RenderingSettings settings  = editor.GetRenderingState().GetDraft();
        settings.algorithm              = rc::RenderAlgorithm::eVoxelGI;
        settings.debug.output           = rc::DebugOutput::eFinal;
        settings.gi.resolution          = 64;
        settings.gi.shadowMapResolution = scenePath != nullptr ? 1024 : 128;
        settings.gi.cone                = rc::VoxelGISettings{};
        settings.gi.cone.SetAllFeatureProviders(rc::VoxelGISettings::RayProvider::Hardware);
        settings.gi.cone.featureProviders[static_cast<size_t>(rc::VoxelGISettings::Feature::Shadows)] =
            rc::VoxelGISettings::RayProvider::Auto;
        settings.gi.cone.rtShadows        = true;
        settings.gi.cone.samples          = 1;
        settings.gi.cone.adaptiveSampling = false;
        settings.gi.cone.fieldResolution  = 64;
        settings.gi.cone.gather           = rc::VoxelGISettings::Gather::Probes;
        ASSERT_TRUE(editor.StageRenderingSettings(settings, true));
        ASSERT_TRUE(editor.UpdateRenderingSettings()) << editor.GetRenderingState().GetError();
        ASSERT_TRUE(Frame(*device, editor));
        rc::VoxelGIRenderer& gi = *device->GetRendererServer()->RequestVoxelGI();
        ASSERT_NE(gi.GetHardwareFeatureMask(), 0u);
        ASSERT_TRUE(gi.UsesHardwareShadows());
        // HW RT needs no field, but the scene's field is prepared in the background meanwhile.
        ASSERT_FALSE(gi.IsPreparingField());
        ASSERT_NE(gi.GetDistanceField(), nullptr);

        std::ofstream csv;
        ASSERT_TRUE(OpenGITimingCsv(csv, GetParam()));
        // Match the editor's global Compute control while retaining inherited map shadows.
        settings.gi.cone.rayProvider = rc::VoxelGISettings::RayProvider::Voxel;
        settings.gi.cone.featureProviders.fill(rc::VoxelGISettings::RayProvider::Voxel);
        settings.gi.cone.featureProviders[static_cast<size_t>(rc::VoxelGISettings::Feature::Shadows)] =
            rc::VoxelGISettings::RayProvider::Auto;
        ASSERT_TRUE(editor.StageRenderingSettings(settings, true));
        ASSERT_TRUE(editor.UpdateRenderingSettings()) << editor.GetRenderingState().GetError();
        ASSERT_TRUE(RenderUntilComputeReady(*device, editor, settings.gi.cone, csv, "cold"));
        const uint64_t generation = gi.GetDistanceField()->GetGeneration();

        settings.gi.cone.SetAllFeatureProviders(rc::VoxelGISettings::RayProvider::Hardware);
        ASSERT_TRUE(editor.StageRenderingSettings(settings, true));
        ASSERT_TRUE(editor.UpdateRenderingSettings());
        ASSERT_TRUE(Frame(*device, editor));
        EXPECT_TRUE(gi.UsesHardwareShadows());
        EXPECT_EQ(gi.GetDistanceField()->GetGeneration(), generation);
        settings.gi.cone.SetAllFeatureProviders(rc::VoxelGISettings::RayProvider::Voxel);
        settings.gi.cone.SetFeatureProvider(rc::VoxelGISettings::Feature::Shadows, rc::VoxelGISettings::RayProvider::Auto);
        ASSERT_TRUE(editor.StageRenderingSettings(settings, true));
        ASSERT_TRUE(editor.UpdateRenderingSettings());
        // A warm switch reuses the published field: no rebuild, effective within two submitted frames.
        uint32_t warmFrames = 0;
        ASSERT_TRUE(RenderUntilComputeReady(*device, editor, settings.gi.cone, csv, "warm", true, &warmFrames));
        EXPECT_LE(warmFrames, 2u);
        EXPECT_EQ(gi.GetDistanceField()->GetGeneration(), generation);
    }
    DestroyDevice(*device, editor);
}

// Reconstructed diffuse sky of one gather configuration, rendered through the debug view.
struct ProbeGatherCase
{
    const char*                      name;
    rc::VoxelGISettings::Gather      gather;
    rc::VoxelGISettings::RayProvider provider;
};

HeapVector<uint8_t> RenderReconstructedSky(rc::RenderDevice&      device,
                                           EditorController&      editor,
                                           rc::RenderingSettings  settings,
                                           const ProbeGatherCase& gatherCase,
                                           uint32_t               frames)
{
    settings.gi.cone.SetAllFeatureProviders(gatherCase.provider);
    settings.gi.cone.gather = gatherCase.gather;
    HeapVector<uint8_t> pixels;
    bool                valid = editor.StageRenderingSettings(settings, true) && editor.UpdateRenderingSettings();
    EXPECT_TRUE(valid) << editor.GetRenderingState().GetError();
    // Compute cases wait for the scene's distance field before the measured frames.
    valid = valid && FrameWithPreparedGI(device, editor);
    for (uint32_t frame = 0; frame < frames && valid; ++frame)
    {
        valid = Frame(device, editor);
        EXPECT_TRUE(valid);
    }
    if (valid)
    {
        const rc::DebugOutputDescription description = editor.GetViewport().GetSnapshot().debug;
        EXPECT_TRUE(description.available) << description.reason;
        pixels = ReadScenePixels(device, editor.GetViewport().GetRenderView().color);
    }
    const char* captureDirectory = std::getenv("ZEN_EDITOR_PROBE_CAPTURE_DIR");
    if (captureDirectory != nullptr && !pixels.empty())
    {
        const rc::RenderView& view = editor.GetViewport().GetRenderView();
        std::ofstream         capture(std::filesystem::path(captureDirectory) / (std::string(gatherCase.name) + ".ppm"),
                                      std::ios::binary);
        capture << "P6\n" << view.width << " " << view.height << "\n255\n";
        for (size_t index = 0; index < pixels.size(); index += 4)
        {
            capture.write(reinterpret_cast<const char*>(pixels.data() + index), 3);
        }
    }
    return pixels;
}

// Screen probes trace the scene with each provider and reconstruct the same diffuse sky as the
// per-pixel gather. The column and sphere make neighbouring probes disagree in plane and normal:
// receivers a reconstruction leaves uncovered show up as dark holes against the per-pixel result.
TEST_P(EditorRendering, ScreenProbeGatherTracksPixelGatherOnCurvedGeometry)
{
    UniquePtr<rc::RenderDevice> device = CreateDevice(GetParam(), true);
    EditorController            editor(*device);
    ASSERT_TRUE(editor.Init());
    ASSERT_TRUE(editor.Load(Fixture("probe_curved.gltf")));
    constexpr uint32_t kWidth = 160, kHeight = 120;
    ASSERT_TRUE(editor.ResizeViewport(kWidth, kHeight));
    editor.GetCamera().GetCamera().SetPose(Vec3(0, -0.75f, 0.55f), Vec3(0, 0, 0.05f));
    rc::RenderingSettings settings = editor.GetRenderingState().GetDraft();
    settings.algorithm             = rc::RenderAlgorithm::eVoxelGI;
    settings.debug.output          = rc::DebugOutput::eGIReconstructedSky;
    settings.lights.clear();
    settings.lightMarkers             = false;
    settings.cameraLight.enabled      = false;
    settings.environment.skybox       = false;
    settings.gi.resolution            = 64;
    settings.gi.shadowMapResolution   = 128;
    settings.gi.cone                  = rc::VoxelGISettings{};
    HeapVector<ProbeGatherCase> cases = {
        {"pixel-field", rc::VoxelGISettings::Gather::Pixel, rc::VoxelGISettings::RayProvider::Voxel},
        {"probes-field", rc::VoxelGISettings::Gather::Probes, rc::VoxelGISettings::RayProvider::Voxel}};
    if (device->GetGPUInfo().rayQuery.IsUsable())
    {
        cases.push_back({"probes-hardware", rc::VoxelGISettings::Gather::Probes, rc::VoxelGISettings::RayProvider::Hardware});
    }
    const HeapVector<uint8_t> reference = RenderReconstructedSky(*device, editor, settings, cases[0], 48);
    EXPECT_EQ(reference.size(), size_t(kWidth) * kHeight * 4);
    for (size_t index = 1; index < cases.size() && reference.size() == size_t(kWidth) * kHeight * 4; ++index)
    {
        SCOPED_TRACE(cases[index].name);
        HeapVector<uint8_t> probes = RenderReconstructedSky(*device, editor, settings, cases[index], 48);
        EXPECT_EQ(probes.size(), reference.size());
        probes.resize(reference.size());
        EXPECT_EQ(device->GetRendererServer()->RequestVoxelGI()->UsesFieldTraversal(),
                  cases[index].provider == rc::VoxelGISettings::RayProvider::Voxel);
        // Receivers with a clearly lit reference; background and deep contact shadow are excluded.
        double   referenceSum = 0.0, probeSum = 0.0, absoluteSum = 0.0;
        uint32_t receivers = 0, holes = 0, clipped = 0;
        for (size_t pixel = 0; pixel < reference.size(); pixel += 4)
        {
            const double expected = (reference[pixel] + reference[pixel + 1] + reference[pixel + 2]) / 3.0;
            const double actual   = (probes[pixel] + probes[pixel + 1] + probes[pixel + 2]) / 3.0;
            if (expected >= 24.0)
            {
                ++receivers;
                referenceSum += expected;
                probeSum     += actual;
                absoluteSum  += std::abs(actual - expected);
                holes        += actual < 0.5 * expected ? 1u : 0u;
                clipped      += expected >= 254.0 ? 1u : 0u;
            }
        }
        // A third of the view is lit receivers; failures keep going so the device is always destroyed.
        EXPECT_GT(receivers, kWidth * kHeight / 3);
        const double                    bias       = (probeSum - referenceSum) / std::max(referenceSum, 1.0);
        const double                    difference = absoluteSum / std::max(referenceSum, 1.0);
        const double                    holeShare  = double(holes) / std::max(receivers, 1u);
        const rc::ScreenProbeStatistics probeCounts =
            device->GetRendererServer()->RequestDeferredLightingRenderer()->GetHybridGI()->GetScreenProbeStatistics();
        std::printf("%s: receivers %u, sRGB mean bias %.4f, mean |difference| %.4f, holes %.4f, clipped %u, "
                    "probes %u uniform, %u of %u requested adaptive\n",
                    cases[index].name, receivers, bias, difference, holeShare, clipped, probeCounts.uniformProbes,
                    probeCounts.adaptiveAllocated, probeCounts.adaptiveRequested);
        EXPECT_LT(clipped, receivers / 10);
        EXPECT_LT(std::abs(bias), 0.04);
        EXPECT_LT(difference, 0.06);
        EXPECT_LT(holeShare, 0.01);
    }
    DestroyDevice(*device, editor);
}

TEST_P(EditorRendering, ShadowPreflightFollowsRayOrMapSelection)
{
    UniquePtr<rc::RenderDevice> device = CreateDevice(GetParam(), true);
    EditorController            editor(*device);
    ASSERT_TRUE(editor.Init());
    ASSERT_TRUE(editor.Load(Fixture("shadow_toggle.gltf")));
    ASSERT_TRUE(editor.ResizeViewport(96, 64));

    rc::RendererServer&   server    = *device->GetRendererServer();
    rc::RenderingSettings settings  = editor.GetRenderingState().GetDraft();
    settings.algorithm              = rc::RenderAlgorithm::eVoxelGI;
    settings.debug.output           = rc::DebugOutput::eFinal;
    settings.gi.resolution          = 64;
    settings.gi.shadowMapResolution = 128;
    settings.gi.cone                = rc::VoxelGISettings{};
    settings.gi.cone.SetAllFeatureProviders(rc::VoxelGISettings::RayProvider::Hardware);

    // An impossible map-array size makes the map preflight fail deterministically,
    // without exhausting physical VRAM. Unavailable hardware shadows need the fallback map array.
    constexpr uint32_t rejectedFaces = (rc::MaxSceneLights + 1) * 6 + 1;
    const bool         hardware      = device->GetGPUInfo().rayQuery.IsUsable();
    EXPECT_EQ(server.ValidateShadowResources(settings, rejectedFaces), hardware);
    ASSERT_TRUE(editor.StageRenderingSettings(settings, true));
    ASSERT_TRUE(editor.UpdateRenderingSettings()) << editor.GetRenderingState().GetError();
    ASSERT_TRUE(Frame(*device, editor));
    ASSERT_EQ(server.RequestVoxelGI()->UsesHardwareShadows(), hardware);
    EXPECT_EQ(server.RequestVoxelGI()->UsesRayShadows(), hardware);
    EXPECT_EQ(server.ValidateShadowResources(settings, rejectedFaces), hardware);

    // A settings edit and a light-count edit both use the viewport's preflight path.
    settings.gi.cone.indirectIntensity = 0.5f;
    rc::RenderingLight light;
    light.id             = rc::MaxSceneLights + 1;
    light.light.type     = rc::SceneLightType::ePoint;
    light.light.position = Vec3(0, 0, 1);
    settings.lights.push_back(light);
    ASSERT_TRUE(editor.StageRenderingSettings(settings));
    ASSERT_TRUE(editor.UpdateRenderingSettings()) << editor.GetRenderingState().GetError();
    ASSERT_TRUE(Frame(*device, editor));
    EXPECT_EQ(server.RequestVoxelGI()->UsesHardwareShadows(), hardware);

    settings.gi.cone.rtShadows = false;
    EXPECT_FALSE(server.ValidateShadowResources(settings, rejectedFaces));
    settings.gi.cone.rtShadows = true;
    settings.gi.cone.SetFeatureProvider(rc::VoxelGISettings::Feature::Shadows, rc::VoxelGISettings::RayProvider::Auto);
    for (rc::VoxelGISettings::RayProvider provider :
         {rc::VoxelGISettings::RayProvider::Voxel, rc::VoxelGISettings::RayProvider::Hardware,
          rc::VoxelGISettings::RayProvider::Auto})
    {
        settings.gi.cone.rayProvider = provider;
        EXPECT_EQ(server.ValidateShadowResources(settings, rejectedFaces),
                  hardware && provider != rc::VoxelGISettings::RayProvider::Voxel);
    }
    // Compute direct shadows use shadow maps, so they need the map array.
    settings.gi.cone.SetAllFeatureProviders(rc::VoxelGISettings::RayProvider::Hardware);
    settings.gi.cone.SetFeatureProvider(rc::VoxelGISettings::Feature::Shadows, rc::VoxelGISettings::RayProvider::Voxel);
    EXPECT_FALSE(server.ValidateShadowResources(settings, rejectedFaces));
    settings.gi.cone.SetAllFeatureProviders(rc::VoxelGISettings::RayProvider::Voxel);
    settings.gi.cone.SetFeatureProvider(rc::VoxelGISettings::Feature::Shadows, rc::VoxelGISettings::RayProvider::Hardware);
    EXPECT_EQ(server.ValidateShadowResources(settings, rejectedFaces), hardware);
    settings.debug.output = rc::DebugOutput::eShadow;
    EXPECT_FALSE(server.ValidateShadowResources(settings, rejectedFaces));
    settings.gi.cone.shadows = false;
    EXPECT_FALSE(server.ValidateShadowResources(settings, rejectedFaces));
    settings.debug.output = rc::DebugOutput::eFinal;
    EXPECT_TRUE(server.ValidateShadowResources(settings, rejectedFaces));
    settings.gi.cone.shadows = true;

    // Scene visibility allocation is checked separately when the frame is prepared.
    server.RequestVoxelGI()->SetRenderScene(nullptr);
    EXPECT_EQ(server.ValidateShadowResources(settings, rejectedFaces), hardware);
    settings.algorithm = rc::RenderAlgorithm::ePBR;
    EXPECT_TRUE(server.ValidateShadowResources(settings, rejectedFaces));
    DestroyDevice(*device, editor);
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

            ASSERT_TRUE(FrameWithPreparedGI(*device, editor));

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

    ASSERT_TRUE(FrameWithPreparedGI(*device, editor));

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

#include "GuideSumTests.inl"

INSTANTIATE_TEST_SUITE_P(Execution, EditorRendering, testing::Values(RHIExecutionMode::eInline, RHIExecutionMode::eThreaded));
} // namespace
} // namespace zen::editor
