#include "Graphics/RenderCore/V2/RenderGraph/RDGPassCompiler.h"
#include "Graphics/RHI/RHICommandList.h"
#include "Graphics/RHI/DynamicRHI.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#if defined(_WIN32)
#    include <malloc.h>
#endif

namespace
{
thread_local bool     countAllocations = false;
thread_local uint64_t allocations      = 0;

void* Allocate(size_t size, size_t alignment = 0)
{
    void* memory = nullptr;

    size         = std::max(size, size_t(1));

    if (alignment == 0)
    {
        memory = std::malloc(size);
    }
    else
    {
#if defined(_WIN32)
        memory = _aligned_malloc(size, alignment);
#else
        memory = std::aligned_alloc(alignment, (size + alignment - 1) / alignment * alignment);
#endif
    }

    if (memory == nullptr)
    {
        std::abort();
    }

    allocations += countAllocations ? 1 : 0;

    return memory;
}

void FreeAligned(void* memory)
{
#if defined(_WIN32)
    _aligned_free(memory);
#else
    std::free(memory);
#endif
}

// Recording only consults device limits. Any backend work invalidates this CPU benchmark.
class MockRHI final : public zen::DynamicRHI
{
public:
    void Init() override
    {
        std::abort();
    }
    void Destroy() override
    {
        std::abort();
    }
    void BeginFrame() override
    {
        std::abort();
    }
    zen::IRHICommandContext* GetCommandContext(zen::RHICommandContextType) override
    {
        std::abort();
    }
    zen::IRHICommandContext* GetTransferCommandContext() override
    {
        std::abort();
    }
    zen::RHIAPIType GetAPIType() override
    {
        std::abort();
    }
    zen::NameID GetName() override
    {
        std::abort();
    }
    DataFormat GetSupportedDepthFormat() override
    {
        std::abort();
    }
    zen::RHIViewport* CreateViewport(void*, uint32_t, uint32_t, bool) override
    {
        std::abort();
    }
    void DestroyViewport(zen::RHIViewport*) override
    {
        std::abort();
    }
    zen::RHIShader* CreateShader(const zen::RHIShaderCreateInfo&) override
    {
        std::abort();
    }
    void DestroyShader(zen::RHIShader*) override
    {
        std::abort();
    }
    zen::RHIPipeline* CreatePipeline(const zen::RHIComputePipelineCreateInfo&) override
    {
        std::abort();
    }
    zen::RHIPipeline* CreatePipeline(const zen::RHIGfxPipelineCreateInfo&) override
    {
        std::abort();
    }
    void DestroyPipeline(zen::RHIPipeline*) override
    {
        std::abort();
    }
    zen::RHISampler* CreateSampler(const zen::RHISamplerCreateInfo&) override
    {
        std::abort();
    }
    void DestroySampler(zen::RHISampler*) override
    {
        std::abort();
    }
    zen::RHITexture* CreateTexture(const zen::RHITextureCreateInfo&) override
    {
        std::abort();
    }
    zen::RHITextureView* CreateTextureView(zen::RHITexture*, const zen::RHITextureViewCreateInfo&) override
    {
        std::abort();
    }
    void DestroyTexture(zen::RHITexture*) override
    {
        std::abort();
    }
    zen::RHIBuffer* CreateBuffer(const zen::RHIBufferCreateInfo&) override
    {
        std::abort();
    }
    void DestroyBuffer(zen::RHIBuffer*) override
    {
        std::abort();
    }
    zen::RHIStatus FinalizeCommandLists(zen::VectorView<zen::RHICommandList*>,
                                        zen::HeapVector<zen::RHIPlatformCommandList*>&) override
    {
        std::abort();
    }
    void SubmitPlatformCommandLists(zen::VectorView<zen::RHIPlatformCommandList*>) override
    {
        std::abort();
    }
    zen::RHISubmissionResult FlushAllGPUCommands() override
    {
        std::abort();
    }
    uint64_t GetLastSubmittedSerial(zen::RHICommandContextType) const override
    {
        std::abort();
    }
    uint64_t QueryLastCompletedSerial(zen::RHICommandContextType) override
    {
        std::abort();
    }
    bool WaitForCompletion(zen::RHICommandContextType, uint64_t, uint64_t) override
    {
        std::abort();
    }
    void WaitDeviceIdle() override
    {
        std::abort();
    }
    zen::RHITextureCopyCapabilities GetTextureCopyCapabilities(DataFormat) const override
    {
        std::abort();
    }
    zen::RHIQueueCopyCapabilities GetQueueCopyCapabilities(zen::RHICommandContextType) const override
    {
        std::abort();
    }

    const zen::RHIGPUInfo& QueryGPUInfo() const override
    {
        return m_info;
    }

private:
    zen::RHIGPUInfo m_info;
};

class MockBuffer final : public zen::RHIBuffer
{
public:
    MockBuffer() : RHIBuffer({.size = 64}) {}

    uint8_t* Map() override
    {
        return nullptr;
    }

    void Unmap() override {}

    bool SetTexelFormat(DataFormat) override
    {
        return false;
    }

private:
    void Init() override {}

    // Stack-owned, with references released before scope exit.
    void Destroy() override {}
};

class MockShader final : public zen::RHIShader
{
public:
    MockShader() : RHIShader({}) {}

private:
    void Init() override {}

    void Destroy() override {}
};

class MockPipeline final : public zen::RHIPipeline
{
public:
    explicit MockPipeline(zen::RHIShader* shader) : RHIPipeline(zen::RHIGfxPipelineCreateInfo{.pShader = shader}) {}

    explicit MockPipeline(const zen::RHIComputePipelineCreateInfo& info) : RHIPipeline(info) {}

private:
    void Init() override {}

    void Destroy() override {}
};

bool Benchmark(bool requireNoAllocations)
{
    using Clock = std::chrono::steady_clock;

    MockShader shader;

    MockPipeline pipeline(&shader);

    MockBuffer index;

    zen::rc::RDGGraphicsPass pass;

    pass.pPipeline                   = &pipeline;

    pass.geometryBuffer.pIndexBuffer = &index;

    zen::RHICommandList commands;

    std::array<double, 20> samples{};

    uint64_t measuredAllocations = 0;

    bool valid                   = true;

    // Grow the command arena and retained-resource tables before measuring encoder checks.
    for (uint32_t run = 0; valid && run < 23; ++run)
    {
        commands.Reset();

        zen::rc::RDGPassCmdEncoder encoder(&commands, &pass);

        allocations                   = 0;

        countAllocations              = run >= 3;

        const Clock::time_point start = Clock::now();

        for (uint32_t draw = 0; draw < 10000; ++draw)
        {
            encoder.SetPushConstants(draw);

            encoder.DrawIndexed(3, 1, 0, 0, 0);
        }

        const Clock::time_point end = Clock::now();

        countAllocations            = false;

        valid                       = bool(encoder.GetResult()) && commands.GetCommandCount() == 20000;

        if (run >= 3)
        {
            samples[run - 3]     = std::chrono::duration<double, std::nano>(end - start).count() / 10000;

            measuredAllocations += allocations;
        }
    }

    if (valid)
    {
        std::sort(samples.begin(), samples.end());

        std::printf("PRODUCTION_ENCODER draws=10000 samples=20 ns_per_pair=%.3f allocations_per_pair=%.6f\n", samples[10],
                    double(measuredAllocations) / 200000);

        valid = !requireNoAllocations || measuredAllocations == 0;
    }
    else
    {
        std::fprintf(stderr, "Encoder benchmark did not record the expected valid commands\n");
    }

    commands.Reset();

    index.ReleaseReference();

    pipeline.ReleaseReference();

    shader.ReleaseReference();

    return valid;
}

enum class EncoderOperation
{
    eDraw,
    eDrawIndexed,
    eDrawIndexedIndirect,
    eDispatch,
    eDispatchIndirect,
    ePushConstants
};

void RecordOperation(EncoderOperation operation, zen::rc::RDGPassCmdEncoder& encoder, zen::RHIBuffer* indirect)
{
    switch (operation)
    {
        case EncoderOperation::eDraw: encoder.Draw(3, 1); break;
        case EncoderOperation::eDrawIndexed: encoder.DrawIndexed(3, 1, 0, 0, 0); break;
        case EncoderOperation::eDrawIndexedIndirect: encoder.DrawIndexedIndirect(indirect, 0, 1, 20); break;
        case EncoderOperation::eDispatch: encoder.Dispatch(1, 1, 1); break;
        case EncoderOperation::eDispatchIndirect: encoder.DispatchIndirect(indirect, 0); break;
        case EncoderOperation::ePushConstants: encoder.SetPushConstants(uint32_t(1)); break;
    }
}

bool BenchmarkOperations(bool requireNoAllocations)
{
    using Clock = std::chrono::steady_clock;

    MockShader shader;

    MockPipeline pipeline(&shader);

    MockPipeline computePipeline(zen::RHIComputePipelineCreateInfo{&shader});

    MockBuffer buffer;

    zen::rc::RDGGraphicsPass graphics;

    graphics.pPipeline                   = &pipeline;

    graphics.geometryBuffer.pIndexBuffer = &buffer;

    zen::rc::RDGComputePass compute;

    compute.pPipeline = &computePipeline;

    zen::rc::RDGPassDescBase desc;

    desc.independentDispatches = true;

    desc.indirectBuffers.push_back(&buffer);

    zen::RHICommandList commands;

    const char* names[] = {"Draw", "DrawIndexed", "DrawIndexedIndirect", "Dispatch", "DispatchIndirect", "SetPushConstants"};

    bool valid          = true;

    for (uint32_t index = 0; index < std::size(names); ++index)
    {
        const EncoderOperation operation = static_cast<EncoderOperation>(index);

        zen::rc::RDGShaderPass* pass =
            operation == EncoderOperation::eDispatch || operation == EncoderOperation::eDispatchIndirect
                ? static_cast<zen::rc::RDGShaderPass*>(&compute)
                : &graphics;

        std::array<double, 20> samples{};

        uint64_t measuredAllocations = 0;

        bool operationValid          = true;

        for (uint32_t run = 0; operationValid && run < 23; ++run)
        {
            commands.Reset();

            zen::rc::RDGPassCmdEncoder encoder(&commands, pass, nullptr, &desc);

            allocations                   = 0;

            countAllocations              = run >= 3;

            const Clock::time_point start = Clock::now();

            for (uint32_t command = 0; command < 10000; ++command)
            {
                RecordOperation(operation, encoder, &buffer);
            }

            const Clock::time_point end = Clock::now();

            countAllocations            = false;

            operationValid              = bool(encoder.GetResult()) && commands.GetCommandCount() == 10000;

            if (run >= 3)
            {
                samples[run - 3]     = std::chrono::duration<double, std::nano>(end - start).count() / 10000;

                measuredAllocations += allocations;
            }
        }

        std::sort(samples.begin(), samples.end());

        std::printf(
            "PRODUCTION_ENCODER_COMMAND name=%s commands=10000 samples=20 ns_per_call=%.3f allocations_per_call=%.6f valid=%u\n",
            names[index], samples[10], double(measuredAllocations) / 200000, uint32_t(operationValid));

        valid &= operationValid && (!requireNoAllocations || measuredAllocations == 0);
    }

    commands.Reset();

    buffer.ReleaseReference();

    pipeline.ReleaseReference();

    computePipeline.ReleaseReference();

    shader.ReleaseReference();

    return valid;
}
} // namespace

// This executable alone replaces global allocation. Engine/custom allocators are unchanged.
void* operator new(size_t size)
{
    return Allocate(size);
}

void* operator new[](size_t size)
{
    return Allocate(size);
}

void* operator new(size_t size, std::align_val_t alignment)
{
    return Allocate(size, size_t(alignment));
}

void* operator new[](size_t size, std::align_val_t alignment)
{
    return Allocate(size, size_t(alignment));
}

void operator delete(void* memory) noexcept
{
    std::free(memory);
}

void operator delete[](void* memory) noexcept
{
    std::free(memory);
}

void operator delete(void* memory, size_t) noexcept
{
    std::free(memory);
}

void operator delete[](void* memory, size_t) noexcept
{
    std::free(memory);
}

void operator delete(void* memory, std::align_val_t) noexcept
{
    FreeAligned(memory);
}

void operator delete[](void* memory, std::align_val_t) noexcept
{
    FreeAligned(memory);
}

void operator delete(void* memory, size_t, std::align_val_t) noexcept
{
    FreeAligned(memory);
}

void operator delete[](void* memory, size_t, std::align_val_t) noexcept
{
    FreeAligned(memory);
}

int main(int argc, char** argv)
{
    zen::GetRHIThread().Start(zen::RHIExecutionMode::eInline);

    MockRHI rhi;

    GDynamicRHI                      = &rhi;

    const bool requireNoAllocations  = argc == 2 && std::strcmp(argv[1], "--expect-no-allocations") == 0;

    bool valid                       = Benchmark(requireNoAllocations);

    valid                           &= BenchmarkOperations(requireNoAllocations);

    GDynamicRHI                      = nullptr;

    zen::GetRHIThread().Stop();

    return valid ? 0 : 1;
}
