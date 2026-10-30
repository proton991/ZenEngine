#include "Graphics/RenderCore/V2/ScenePresentation.h"
#include "Graphics/RenderCore/V2/RenderDevice.h"
#include <filesystem>
#include <fstream>

namespace zen::rc
{
// Explicit diagnostic capture; this wait is never used by ordinary UI drawing/resize.
bool CaptureSceneFrame(rc::RenderDevice& device, RHITexture* source, const std::string& path)
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

    RHITexture* sampled = device.CreateTextureSampled(format, {.copyUsage = true}, "SceneCaptureColor");

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
        rc::RenderGraph graph("SceneCapture");

        valid = graph.Begin();

        RHITextureCopyRegion region{};

        region.size = Vec3i(width, height, 1);

        region.srcSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);

        region.dstSubresources = region.srcSubresources;

        graph.AddTransferPass("CopySceneCapture").CopyTexture(source, sampled, {&region, 1});

        rc::RDGComputePassDesc pack;

        pack.SetShaderProgramName("CaptureFrameSP");

        pack.BindSampledTexture("sourceColor", sampler, sampled->GetDefaultView());

        pack.BindStorageBuffer("packedColor", packed, rc::RDGContentGuarantee::eFullWrite);

        graph.AddComputePass(std::move(pack)).RecordPassCommands([width, height](rc::RDGPassCmdEncoder& encoder) {
            encoder.Dispatch((width + 7) / 8, (height + 7) / 8, 1);
        });

        graph.AddTransferPass("ReadSceneCapture").CopyBuffer(packed, readback, {0, 0, info.size}).NeverCull();

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
} // namespace zen::rc
