#include "EditorApplication.h"
#include "Editor/ImGui/EditorContext.h"
#include "Editor/ImGui/EditorWorkspace.h"
#include "Editor/ImGui/EditorTheme.h"
#include "Editor/Platform/EditorApplicationIcon.h"
#include "ImGui/UIContext.h"
#include "Platform/NativeWindow.h"
#include "Platform/ConfigLoader.h"
#include "Editor/Model/EditorText.h"
#include "Graphics/RHI/RHIOptions.h"
#include "Graphics/RenderCore/V2/ShaderProgram.h"
#include "AssetLib/TextureLoader.h"
#include <spdlog/sinks/base_sink.h>
#include <chrono>
#include <algorithm>
#include <fstream>

namespace zen::editor
{
namespace
{
class EditorLogSink final : public spdlog::sinks::base_sink<std::mutex>
{
public:
    explicit EditorLogSink(EditorLog& log) : m_log(log) {}

protected:
    void sink_it_(const spdlog::details::log_msg& message) override
    {
        m_log.Append(int(message.level), std::string(message.payload.data(), message.payload.size()));
    }

    void flush_() override {}

private:
    EditorLog& m_log;
};

// Start beside the most recent scene, else in the configured glTF model directory.
std::string GetDialogFolder(const RecentFiles& recent)
{
    HeapVector<std::filesystem::path> candidates;

    if (!recent.Get().empty())
    {
        candidates.push_back(std::filesystem::u8path(recent.Get().front()).parent_path());
    }

    candidates.push_back(std::filesystem::u8path(platform::ConfigLoader::GetInstance().GetGLTFModelBasePath()));

    std::string result;

    for (const std::filesystem::path& candidate : candidates)
    {
        std::error_code error;

        if (result.empty() && !candidate.empty() && std::filesystem::is_directory(candidate, error))
        {
            // Native separators and a trailing separator name a folder rather than a file.
            const std::u8string text = (candidate / "").make_preferred().u8string();

            result                   = std::string(reinterpret_cast<const char*>(text.data()), text.size());
        }
    }

    return result;
}

RHITexture* CreateEditorIconTexture(rc::RenderDevice& device)
{
    const asset::TextureInfo image = asset::TextureLoader::LoadTexture2DFromFile("Editor/zen_engine_menu.png");

    RHITexture* texture            = nullptr;

    if (image.width > 0 && image.height > 0 && !image.data.empty())
    {
        rc::TextureFormat format;

        format.width  = image.width;

        format.height = image.height;

        format.depth  = 1;

        // The UI blends SDR encoded colors, so preserve the PNG's color values.
        format.format = DataFormat::eR8G8B8A8UNORM;

        texture       = device.CreateTextureSampled(format, {.copyUsage = true}, "ZenEditorIcon");

        if (texture != nullptr)
        {
            RHIBufferTextureCopyRegion region{};

            region.textureSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);

            region.textureSubresources.layerCount = 1;

            region.textureSize                    = {image.width, image.height, 1};

            device.UpdateTexture(texture, {&region, 1}, uint32_t(image.data.size()), image.data.data());
        }
    }

    return texture;
}

// Explicit diagnostic capture; this wait is never used by ordinary UI drawing/resize.
bool CaptureFrame(rc::RenderDevice& device, RHITexture* source, const std::string& path)
{
    device.FlushRHIThread();

    device.WaitForIdle();

    const uint32_t width  = source->GetWidth();

    const uint32_t height = source->GetHeight();

    rc::TextureFormat format;

    format.width        = width;

    format.height       = height;

    format.depth        = 1;

    format.format       = source->GetFormat();

    RHITexture* sampled = device.CreateTextureSampled(format, {.copyUsage = true}, "EditorCaptureColor");

    RHIBufferCreateInfo info;

    info.size         = width * height * 4;

    info.allocateType = RHIBufferAllocateType::eGPU;

    info.usageFlags.SetFlags(RHIBufferUsageFlagBits::eStorageBuffer, RHIBufferUsageFlagBits::eTransferSrcBuffer);

    RHIBuffer* packed = device.CreateBuffer(info);

    info.allocateType = RHIBufferAllocateType::eCPURead;

    info.usageFlags   = 0;

    info.usageFlags.SetFlag(RHIBufferUsageFlagBits::eTransferDstBuffer);

    RHIBuffer* readback = device.CreateBuffer(info);

    RHISampler* sampler = device.CreateSampler(RHISamplerCreateInfo::CreateLinearRepeat());

    bool valid          = sampled != nullptr && packed != nullptr && readback != nullptr && sampler != nullptr;

    if (valid)
    {
        rc::RenderGraph graph("EditorCapture");

        valid = graph.Begin();

        RHITextureCopyRegion region{};

        region.size = Vec3i(width, height, 1);

        region.srcSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);

        region.dstSubresources = region.srcSubresources;

        graph.AddTransferPass("CopyEditorCapture").CopyTexture(source, sampled, {&region, 1});

        rc::RDGComputePassDesc pack;

        pack.SetShaderProgramName("CaptureFrameSP");

        pack.BindSampledTexture("sourceColor", sampler, sampled->GetDefaultView());

        pack.BindStorageBuffer("packedColor", packed, rc::RDGContentGuarantee::eFullWrite);

        graph.AddComputePass(std::move(pack)).RecordPassCommands([width, height](rc::RDGPassCmdEncoder& encoder) {
            encoder.Dispatch((width + 7) / 8, (height + 7) / 8, 1);
        });

        graph.AddTransferPass("ReadEditorCapture").CopyBuffer(packed, readback, {0, 0, info.size}).NeverCull();

        valid = valid && graph.End() && device.ExecuteRenderGraph(graph);

        device.FlushRHIThread();

        device.WaitForIdle();

        if (valid)
        {
            const uint8_t* pixels = readback->Map();

            std::ofstream file(std::filesystem::u8path(path), std::ios::binary);

            valid = pixels != nullptr && file.good();

            if (valid)
            {
                file << "P6\n" << width << ' ' << height << "\n255\n";

                for (uint32_t index = 0; index < width * height; ++index)
                {
                    file.write(reinterpret_cast<const char*>(pixels + index * 4), 3);
                }

                valid = file.good();
            }

            if (pixels != nullptr)
            {
                readback->Unmap();
            }
        }
    }

    device.DestroyTexture(sampled);

    device.DestroyBuffer(packed);

    device.DestroyBuffer(readback);

    return valid;
}
} // namespace

class EditorApplication::State final : public rc::RenderOverlay
{
public:
    explicit State(EditorOptions options) : m_options(std::move(options)) {}

    bool Initialize()
    {
        // spdlog's sink API uses std::shared_ptr; keep the sink alive until worker teardown.
        m_sink = std::make_shared<EditorLogSink>(m_log);

        spdlog::default_logger()->sinks().push_back(m_sink);

        RHIOptions::GetInstance().SetRayTracingEnabled(false);

        RHIOptions::GetInstance().SetValidationEnabled(true);

        RHIOptions::GetInstance().SetGPUMemoryStats(true);

        // Hidden until initialization completes; a pending maximize applies when shown.
        const platform::WindowConfig config{"ZenEditor", true, 1440, 900, 0, false};

        m_window       = MakeUnique<platform::NativeWindow>(config);

#if defined(ZEN_MACOS)
        if (!InitializeEditorApplicationIcon())
        {
            LOGW("Could not load the ZenEditor application icon");
        }
#endif

        m_windowChrome = MakeUnique<EditorWindowChrome>(*m_window);

        m_windowChrome->Initialize();

        m_window->SetMinimumSize(800, 500);

        if (!m_options.windowed)
        {
            m_window->Maximize();
        }

        m_device = MakeUnique<rc::RenderDevice>(RHIAPIType::eVulkan, 2,
                                                m_options.threaded ? RHIExecutionMode::eThreaded : RHIExecutionMode::eInline);

        int framebufferWidth                     = 0;

        int framebufferHeight                    = 0;

        const platform::WindowExtent framebuffer = m_window->GetFramebufferExtent();

        framebufferWidth                         = int(framebuffer.width);

        framebufferHeight                        = int(framebuffer.height);

        m_present = m_device->CreateViewport(m_window.Get(), uint32_t(std::max(framebufferWidth, 1)),
                                             uint32_t(std::max(framebufferHeight, 1)), true);

        rc::ShaderProgramManager::GetInstance().BuildShaderPrograms(m_device.Get());

        m_device->Init(m_present);

        m_device->InitializeRendererServer();

        m_device->GetRendererServer()->SetRenderOption(m_options.mode);

        float scale = 1;

        scale       = m_window->GetUIScale();

        scale       = m_options.scale > 0 ? m_options.scale : scale;

        bool valid  = m_ui.Init(*m_window, {true, scale, m_options.scale <= 0, 15.0f});

        if (valid)
        {
            ApplyEditorTheme(scale);
        }

        m_renderer   = MakeUnique<ui::ImGuiRenderer>(*m_device);

        valid        = valid && m_renderer->Init(m_ui.GetFonts());

        m_controller = MakeUnique<EditorController>(*m_device);

        valid        = valid && m_controller->Init();

        if (valid)
        {
            m_settings = m_options.settingsDirectory.empty() ? GetDefaultEditorSettingsDirectory()
                                                             : std::filesystem::u8path(m_options.settingsDirectory);

            LoadEditorPreferences(m_settings, m_controller->GetPreferences());

#if defined(ZEN_MACOS)
            const EditorShortcut quitShortcut{platform::Key::Q, uint16_t(platform::KeyModifier::Super)};

            constexpr const char* quitLabel = "Quit ZenEditor";
#else
            const EditorShortcut quitShortcut{};

            constexpr const char* quitLabel = "Exit";
#endif

            m_controller->GetActions().Register(
                {actions::Exit, quitLabel, quitShortcut, "", nullptr, [this]() { m_exitRequested = true; }});

            m_workspace   = MakeUnique<EditorWorkspace>(m_settings);

            m_context     = MakeUnique<EditorContext>(EditorContext{*m_controller, m_log, *m_renderer, *m_windowChrome});

            m_iconTexture = CreateEditorIconTexture(*m_device);

            if (m_iconTexture != nullptr)
            {
                RHISamplerCreateInfo sampler = RHISamplerCreateInfo::CreateLinearRepeat();

                sampler.repeatU = sampler.repeatV = sampler.repeatW = RHISamplerRepeatMode::eClampToEdge;

                m_context->appIcon = m_renderer->GetRenderer().RegisterTexture(m_iconTexture, m_device->CreateSampler(sampler));
            }

            if (m_context->appIcon.value == 0)
            {
                LOGW("Could not load the ZenEditor menu icon");
            }

            m_context->nativeFileDialog = platform::NativeWindow::SupportsFileDialogs();

            m_workspace->Initialize(*m_context);

            if (!m_options.scene.empty())
            {
                m_controller->RequestLoad(m_options.scene);
            }

            // Appear once, at the final size; the first frame presents after the resize.
            if (!m_options.hidden)
            {
                m_window->Show();

                // Showing a Cocoa window alone does not activate an app launched
                // from an IDE or terminal. Bring the ready editor to the front.
                m_window->Focus();
            }

            const platform::WindowExtent shown = m_window->GetFramebufferExtent();

            LOGI("ZenEditor initialized: {} RHI, UI scale {}, framebuffer {}x{}, maximized={}",
                 m_options.threaded ? "threaded" : "inline", scale, shown.width, shown.height, m_window->IsMaximized());
        }

        return valid;
    }

    bool BuildRenderGraph(rc::RenderGraph& graph, RHIViewport& viewport) override
    {
        if (m_context->sceneVisible)
        {
            m_controller->GetViewport().BuildOverlays(graph);
        }

        // Before the UI pass, which samples the preview image in the same graph.
        m_controller->GetMeshPreview().BuildGraph(graph);

        const ImDrawData* data = m_ui.GetDrawData();

        return data != nullptr && m_renderer->BuildRenderGraph(graph, viewport, *data);
    }

    void SmokeActions(uint32_t frame)
    {
        if (frame == 4)
        {
            m_window->Restore();

            m_window->SetSize(1280, 800);
        }

        if (frame == 6)
        {
            m_window->Minimize();

            platform::NativeWindow::PollEvents();

            m_window->Restore();

            if (m_options.hidden)
            {
                m_window->Hide();
            }
        }

        if (frame == 7)
        {
            m_workspace->SetPanelVisible("SceneViewport", false);
        }

        if (frame == 10)
        {
            m_workspace->SetPanelVisible("SceneViewport", true);
        }

        if (frame == 12)
        {
            m_controller->GetActions().Execute(actions::FrameAll);

            m_controller->Pick(Vec2(0.5f));
        }

        if (frame == 16)
        {
            CameraInput input;

            input.orbit = Vec2(15, -8);

            m_controller->GetCamera().Apply(input);

            // An orientation-sphere drag, which keeps the orbit center in view.
            m_controller->GetCamera().OrbitBy(Vec2(0.7f, 0.35f));
        }

        if (frame == 20)
        {
            m_controller->Pick(Vec2(0.5f));
        }

        if (frame == 24)
        {
            m_controller->GetCamera().SetOrthographic(true);
        }

        if (frame == 36)
        {
            if (m_options.windowed)
            {
                m_window->SetSize(1440, 900);
            }
            else
            {
                m_window->Show();

                m_window->Maximize();

                if (m_options.hidden)
                {
                    m_window->Hide();
                }
            }

            m_workspace->ResetLayout();
        }

        if (frame == 28)
        {
            m_controller->GetCamera().SetOrthographic(false);

            m_workspace->ResetLayout();
        }

        if (frame == 30)
        {
            const HeapVector<SceneAssetItem> assets = m_controller->GetScene().GetAssets().Query("");

            if (!assets.empty())
            {
                m_controller->GetSelection().SelectAsset(assets.back().id);
            }
        }

        // Asset to node selection, followed by browsing references without replacing it.
        if (frame == 33 && !m_controller->GetScene().GetRoots().empty())
        {
            m_controller->GetSelection().SelectNode(m_controller->GetScene().GetRoots()[0]);
        }

        if (frame == 34 || frame == 35)
        {
            const SceneAssetKind kind = frame == 34 ? SceneAssetKind::Material : SceneAssetKind::Texture;

            for (const SceneAssetItem& item : m_controller->GetScene().GetAssets().Query(""))
            {
                if (item.id.kind == kind)
                {
                    m_controller->GetInspector().Open({{}, item.id});

                    break;
                }
            }
        }

        if (frame == 37 || frame == 39)
        {
            m_controller->GetActions().Execute(actions::InspectorBack);
        }

        if (frame == 38)
        {
            m_controller->GetActions().Execute(actions::InspectorForward);
        }
    }

    // Platform picker requests from the previous frame; a chosen path loads this frame.
    void ProcessFileDialog()
    {
        if (m_context->nativeFileDialog && m_fileDialog == FileDialogTarget::None)
        {
            if (m_controller->TakeFileOpenRequest())
            {
                if (m_window->ShowOpenFileDialog({"glTF scenes", "gltf;glb"},
                                                 GetDialogFolder(m_controller->GetPreferences().recentFiles)))
                {
                    m_fileDialog = FileDialogTarget::Scene;
                }
            }
            else if (m_controller->TakeEnvironmentFileOpenRequest())
            {
                if (m_window->ShowOpenFileDialog({"Environment textures", "hdr;ktx;dds"}, ZEN_TEXTURE_PATH))
                {
                    m_fileDialog = FileDialogTarget::Environment;
                }
            }
        }

        std::string path;

        if (m_window->TakeFileDialogResult(path))
        {
            if (!path.empty())
            {
                if (m_fileDialog == FileDialogTarget::Environment)
                {
                    m_controller->RequestEnvironmentTexture(path);
                }
                else if (m_fileDialog == FileDialogTarget::Scene)
                {
                    m_controller->RequestLoad(path);
                }
            }

            m_fileDialog = FileDialogTarget::None;
        }
    }

    int Run()
    {
        bool valid                                     = Initialize();

        uint32_t frame                                 = 0;

        std::chrono::steady_clock::time_point previous = std::chrono::steady_clock::now();

        while (valid && !m_window->ShouldClose() && !m_exitRequested && (m_options.frames == 0 || frame < m_options.frames))
        {
            m_windowChrome->ProcessPendingAction();

            if (m_options.smokeTest && !m_controller->GetLoadState().IsActive())
            {
                SmokeActions(frame);
            }

            m_window->Update(false);

            m_context->focused = m_window->IsFocused();

            m_workspace->ProcessMenuCommands(*m_context);

            ProcessFileDialog();

            if (m_exitRequested)
            {
                break;
            }

            int width                                = 0;

            int height                               = 0;

            const platform::WindowExtent framebuffer = m_window->GetFramebufferExtent();

            width                                    = int(framebuffer.width);

            height                                   = int(framebuffer.height);

            if (width == 0 || height == 0)
            {
                platform::NativeWindow::WaitEvents(0.05);

                previous = std::chrono::steady_clock::now();
            }
            else
            {
                // Window coordinates and framebuffer pixels can differ on HiDPI monitors.
                if (m_present->GetWidth() != uint32_t(width) || m_present->GetHeight() != uint32_t(height))
                {
                    m_device->ProcessViewportResize(uint32_t(width), uint32_t(height));
                }

                const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();

                const float seconds                             = std::chrono::duration<float>(now - previous).count();

                previous                                        = now;

                m_context->seconds                              = std::min(seconds, 0.1f);

                m_context->frameMs = m_context->frameMs == 0 ? seconds * 1000.0f : m_context->frameMs * 0.9f + seconds * 100.0f;

                // Advance outside UI recording. GPU stages can block; restart timing
                // after a transition so navigation does not jump when opening finishes.
                if (m_controller->UpdateSceneLoad())
                {
                    previous = std::chrono::steady_clock::now();
                }

                if (!m_options.environment.empty() && !m_controller->GetLoadState().IsActive()
                    && m_controller->GetViewport().HasScene())
                {
                    m_controller->RequestEnvironmentTexture(std::move(m_options.environment));

                    m_options.environment.clear();
                }

                if (m_options.debugSpecified && m_controller->GetViewport().HasScene()
                    && !m_controller->GetLoadState().IsActive())
                {
                    rc::RenderingSettings settings = m_controller->GetRenderingState().GetDraft();

                    settings.debug.output          = m_options.debugOutput;

                    settings.debug.maximum         = m_options.debugOutput == rc::DebugOutput::eDepth ? 10.0f : 1.0f;

                    settings.debug.slice           = settings.gi.resolution / 2;

                    m_controller->StageRenderingSettings(settings);

                    m_options.debugSpecified = false;
                }

                m_controller->UpdateRenderingSettings();

                if (m_controller->UpdateEnvironment())
                {
                    previous = std::chrono::steady_clock::now();
                }

                m_controller->ProcessPicks();

                m_ui.BeginFrame(m_context->seconds, m_present->GetWidth(), m_present->GetHeight());

                m_workspace->Draw(*m_context);

                m_ui.EndFrame();

                EditorViewport& viewport = m_controller->GetViewport();

                valid = m_device->GetRendererServer()->DispatchRenderWorkloads(viewport.GetRenderView(), this,
                                                                               m_context->sceneVisible);

                viewport.OnSubmitted(valid);

                m_controller->RecordRenderingResult(valid);

                if (valid && !m_options.capture.empty() && m_options.frames != 0 && frame + 1 == m_options.frames)
                {
                    valid = CaptureFrame(*m_device, m_present->GetColorBackBuffer(), m_options.capture);
                }

                if (valid && !m_options.sceneCapture.empty() && m_options.frames != 0 && frame + 1 == m_options.frames)
                {
                    valid = viewport.HasScene() && viewport.GetRenderView().color != nullptr
                         && CaptureFrame(*m_device, viewport.GetRenderView().color, m_options.sceneCapture);
                }

                m_device->NextFrame();

                // Smoke actions exercise the loaded scene, after asynchronous startup.
                if (!m_options.smokeTest || !m_controller->GetLoadState().IsActive())
                {
                    ++frame;
                }
            }
        }

        Shutdown();

        return valid ? 0 : 1;
    }

    void Shutdown()
    {
        if (m_device)
        {
            m_device->WaitForPreviousFrames();

            if (m_workspace)
            {
                m_workspace->Save(*m_context);

                if (!SaveEditorPreferences(m_settings, m_controller->GetPreferences()))
                {
                    LOGW("Could not save editor preferences to {}", PathToUtf8(m_settings));
                }
            }

            m_workspace.Reset();

            m_context.Reset();

            if (m_renderer)
            {
                m_renderer->Destroy();
            }

            m_renderer.Reset();

            m_device->DestroyTexture(m_iconTexture);

            m_iconTexture = nullptr;

            m_ui.Destroy();

            if (m_controller)
            {
                m_controller->Destroy();
            }

            m_controller.Reset();

            rc::ShaderProgramManager::GetInstance().Destroy();

            m_device->Destroy();

            m_device.Reset();
        }

        if (m_window)
        {
            m_window->Hide();
        }

        m_windowChrome.Reset();

        m_window.Reset();

        if (m_sink)
        {
            std::vector<spdlog::sink_ptr>& sinks = spdlog::default_logger()->sinks();

            sinks.erase(std::remove(sinks.begin(), sinks.end(), m_sink), sinks.end());

            m_sink.reset();
        }
    }

private:
    enum class FileDialogTarget
    {
        None,
        Scene,
        Environment
    };

    FileDialogTarget                  m_fileDialog{FileDialogTarget::None};
    EditorOptions                     m_options;
    EditorLog                         m_log;
    std::shared_ptr<EditorLogSink>    m_sink;
    UniquePtr<platform::NativeWindow> m_window;
    UniquePtr<EditorWindowChrome>     m_windowChrome;
    UniquePtr<rc::RenderDevice>       m_device;
    RHIViewport*                      m_present{nullptr};
    RHITexture*                       m_iconTexture{nullptr};
    ui::UIContext                     m_ui;
    UniquePtr<ui::ImGuiRenderer>      m_renderer;
    UniquePtr<EditorController>       m_controller;
    UniquePtr<EditorContext>          m_context;
    UniquePtr<EditorWorkspace>        m_workspace;
    std::filesystem::path             m_settings;
    bool                              m_exitRequested{false};
};

EditorApplication::EditorApplication(EditorOptions options) : m_state(MakeUnique<State>(std::move(options))) {}

EditorApplication::~EditorApplication() = default;

int EditorApplication::Run()
{
    return m_state->Run();
}
} // namespace zen::editor
