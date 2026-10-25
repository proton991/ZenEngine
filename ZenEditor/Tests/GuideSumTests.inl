// The independent fixture generator uses Python Fraction and rational searches,
// rather than reproducing the shader's carry and rounding implementation.
class GuideSumProbe
{
public:
    explicit GuideSumProbe(rc::RenderDevice& device) : m_device(device) {}
    ~GuideSumProbe()
    {
        m_device.WaitForIdle();
        m_device.DestroyBuffer(m_samples);
        m_device.DestroyBuffer(m_results);
        m_device.DestroyBuffer(m_readback);
    }
    void Run(const HeapVector<uint32_t>& samples, uint32_t cases, HeapVector<uint32_t>& values)
    {
        m_samples = m_device.CreateStorageBuffer(static_cast<uint32_t>(samples.size() * sizeof(uint32_t)),
                                                 reinterpret_cast<const uint8_t*>(samples.data()), "guide_sum_samples");
        RHIBufferCreateInfo info;
        info.size         = cases * 4u * 11u * sizeof(uint32_t);
        info.allocateType = RHIBufferAllocateType::eGPU;
        info.usageFlags.SetFlags(RHIBufferUsageFlagBits::eStorageBuffer, RHIBufferUsageFlagBits::eTransferSrcBuffer);
        m_results         = m_device.CreateBuffer(info);
        info.allocateType = RHIBufferAllocateType::eCPURead;
        info.usageFlags.Clear();
        info.usageFlags.SetFlag(RHIBufferUsageFlagBits::eTransferDstBuffer);
        m_readback = m_device.CreateBuffer(info);
        ASSERT_NE(m_samples, nullptr);
        ASSERT_NE(m_results, nullptr);
        ASSERT_NE(m_readback, nullptr);
        rc::ShaderProgramManager::GetInstance().StoreProgram(
            ZEN_NEW() rc::ComputeFileSP(&m_device, "GuideSumCheckSP", "VoxelGI/Calibration/guide_sum_check.comp.spv"));
        rc::RenderGraph graph("GuideSumCheck");
        ASSERT_TRUE(graph.Begin());
        rc::RDGComputePassDesc pass;
        pass.SetShaderProgramName("GuideSumCheckSP");
        pass.BindStorageBuffer("Samples", m_samples);
        pass.BindStorageBuffer("Results", m_results, rc::RDGContentGuarantee::eFullWrite);
        graph.AddComputePass(std::move(pass)).RecordPassCommands([cases](rc::RDGPassCmdEncoder& encoder) {
            encoder.Dispatch(cases * 4u, 1, 1);
        });
        graph.AddTransferPass("guide_sum_readback").CopyBuffer(m_results, m_readback, {0, 0, info.size});
        ASSERT_TRUE(graph.End()) << graph.GetResult().message;
        ASSERT_TRUE(m_device.ExecuteRenderGraph(graph));
        m_device.FlushRHIThread();
        m_device.WaitForIdle();
        const uint8_t* mapped = m_readback->Map();
        ASSERT_NE(mapped, nullptr);
        values.resize(cases * 4u * 11u);
        std::memcpy(values.data(), mapped, static_cast<size_t>(info.size));
        m_readback->Unmap();
    }

private:
    rc::RenderDevice& m_device;
    RHIBuffer*        m_samples{nullptr};
    RHIBuffer*        m_results{nullptr};
    RHIBuffer*        m_readback{nullptr};
};

TEST_P(EditorRendering, GuideSumMatchesExactRationalOracleAcrossArrivalOrders)
{
    std::ifstream input(Fixture("guide_sum_cases.bin"), std::ios::binary);
    ASSERT_TRUE(input.is_open());
    std::error_code error;
    const uintmax_t size = std::filesystem::file_size(Fixture("guide_sum_cases.bin"), error);
    ASSERT_FALSE(error) << error.message();
    ASSERT_LE(size, 16u * 1024u * 1024u);
    HeapVector<uint8_t> bytes(static_cast<size_t>(size));
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    ASSERT_TRUE(input.good());
    ASSERT_GE(bytes.size(), 16u);
    ASSERT_EQ(bytes.size() % 4u, 0u);
    HeapVector<uint32_t> words(bytes.size() / 4u);
    for (size_t index = 0; index < words.size(); ++index)
    {
        words[index] = uint32_t(bytes[index * 4u]) | uint32_t(bytes[index * 4u + 1u]) << 8u
                     | uint32_t(bytes[index * 4u + 2u]) << 16u | uint32_t(bytes[index * 4u + 3u]) << 24u;
    }
    ASSERT_EQ(words[0], 0x3153475au);
    const uint32_t cases = words[1];
    ASSERT_GT(cases, 200u);
    ASSERT_LT(cases, 4096u);
    ASSERT_EQ(words[2], 4096u);
    ASSERT_EQ(words[3], 11u);
    ASSERT_EQ(words.size(), 4u + cases * (11u + 4096u));
    const HeapVector<uint32_t> samples(words.data() + 4u + cases * 11u, cases * 4096u);
    EditorRenderingScope       scope(GetParam());
    EditorRenderingValidation  validation;
    GuideSumProbe              probe(*scope.device);
    HeapVector<uint32_t>       values;
    ASSERT_NO_FATAL_FAILURE(probe.Run(samples, cases, values));
    for (uint32_t testCase = 0; testCase < cases; ++testCase)
    {
        for (uint32_t permutation = 0; permutation < 4; ++permutation)
        {
            for (uint32_t word = 0; word < 11; ++word)
            {
                ASSERT_EQ(values[(testCase * 4u + permutation) * 11u + word], words[4u + testCase * 11u + word])
                    << "case " << testCase << " order " << permutation << " word " << word;
            }
        }
    }
    testing::Test::RecordProperty("exact_sum_cases", cases);
    testing::Test::RecordProperty("arrival_orders", 4);
}
