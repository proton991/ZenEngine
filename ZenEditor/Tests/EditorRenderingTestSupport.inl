// Shared native fixture and readback helpers; include inside the test namespace.
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

bool Frame(rc::RenderDevice& device, EditorController& editor, bool processPicks = true)
{
    EditorViewport& viewport = editor.GetViewport();

    bool valid =
        device.GetRendererServer()->DispatchRenderWorkloads(viewport.GetRenderView()) == rc::RenderFrameResult::eSubmitted;

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

    if (processPicks)
    {
        editor.ProcessPicks();
    }

    return valid;
}

// Image and GI-state checks need the completed cold preparation. Responsiveness
// tests use Frame directly so they can inspect the initial voxel GI preview.
bool FrameWithPreparedGI(rc::RenderDevice& device, EditorController& editor)
{
    const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    bool                                        valid    = Frame(device, editor);
    const rc::VoxelGIRenderer&                  gi       = *device.GetRendererServer()->RequestVoxelGI();
    while (valid && gi.IsPreparingField() && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        valid = Frame(device, editor);
    }
    return valid && !gi.IsPreparingField();
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
UniquePtr<rc::RenderDevice> CreateDevice(RHIExecutionMode             mode,
                                         bool                         rayTracing      = false,
                                         const rc::RenderingSettings* initialSettings = nullptr)
{
    RHIOptions::GetInstance().SetRayTracingDisabled(!rayTracing);

    RHIOptions::GetInstance().SetValidationEnabled(true);

    UniquePtr<rc::RenderDevice> device = MakeUnique<rc::RenderDevice>(RHIAPIType::eVulkan, 2, mode);

    device->Init(nullptr);

    rc::ShaderProgramManager::GetInstance().BuildShaderPrograms(device.Get());

    EXPECT_EQ(device->GetRendererServer(), nullptr);

    device->InitializeRendererServer(initialSettings);

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

class EditorRenderingScope
{
public:
    explicit EditorRenderingScope(RHIExecutionMode mode, bool rayTracing = false) :
        device(CreateDevice(mode, rayTracing)), editor(*device)
    {}
    ~EditorRenderingScope()
    {
        DestroyDevice(*device, editor);
    }

    UniquePtr<rc::RenderDevice> device;
    EditorController            editor;
};

class EditorRenderingValidation
{
public:
    EditorRenderingValidation()
    {
        VkDebugUtilsMessengerCreateInfoEXT info{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        info.messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
        info.pfnUserCallback = Validation;
        EXPECT_EQ(vkCreateDebugUtilsMessengerEXT(GVulkanRHI->GetInstance(), &info, nullptr, &m_messenger), VK_SUCCESS);
    }
    ~EditorRenderingValidation()
    {
        vkDestroyDebugUtilsMessengerEXT(GVulkanRHI->GetInstance(), m_messenger, nullptr);
    }

private:
    static VKAPI_ATTR VkBool32 VKAPI_CALL Validation(VkDebugUtilsMessageSeverityFlagBitsEXT,
                                                     VkDebugUtilsMessageTypeFlagsEXT,
                                                     const VkDebugUtilsMessengerCallbackDataEXT* data,
                                                     void*)
    {
        ADD_FAILURE() << data->pMessage;
        return VK_FALSE;
    }
    VkDebugUtilsMessengerEXT m_messenger{VK_NULL_HANDLE};
};
