#include "Graphics/RenderCore/V2/RenderDevice.h"
#include "Graphics/RenderCore/V2/SpatialTextureSampling.h"
#include "Graphics/RenderCore/V2/ShaderProgram.h"
#include <cmath>

namespace zen::rc
{
namespace
{
constexpr uint32_t QueryCount  = 4 * 256 * 3;
constexpr uint32_t OutputBytes = QueryCount * 3 * sizeof(Vec4);

void DispatchSpatialQualification(RDGPassCmdEncoder& encoder)
{
    encoder.Dispatch(QueryCount / 64, 1, 1);
}

bool MeasureSpatialTextureSampling(RenderDevice& device, SpatialTextureRounding& rounding)
{
    ShaderProgramManager& programs = ShaderProgramManager::GetInstance();
    ShaderProgram*        program  = programs.RequestShaderProgram("SpatialSamplingQualificationSP");
    if (program == nullptr)
    {
        const HashMap<uint32_t, RHIShaderSpecializationValue> constants{{0u, device.GetGPUInfo().subTexelPrecisionBits}};
        program = ZEN_NEW() ComputeFileSP(&device, "SpatialSamplingQualificationSP",
                                          "SceneRenderer/atlas_sampling_calibration.comp.spv", constants);
        if (program->GetShader() != nullptr)
        {
            programs.StoreProgram(program);
        }
        else
        {
            ZEN_DELETE(program);
            program = nullptr;
        }
    }
    bool        valid    = program != nullptr && program->GetShader() != nullptr;
    RHITexture* texture  = nullptr;
    RHIBuffer*  output   = nullptr;
    RHIBuffer*  readback = nullptr;
    RHISampler* sampler  = nullptr;
    if (valid)
    {
        RHITextureCreateInfo info;
        info.type   = RHITextureType::e2D;
        info.format = DataFormat::eR8G8B8A8UNORM;
        info.width = info.height = 2;
        info.usageFlags.SetFlags(RHITextureUsageFlagBits::eSampled, RHITextureUsageFlagBits::eTransferDst);
        info.tag = "spatial_sampling_qualification";
        texture  = device.CreateTexture(info);
        RHIBufferCreateInfo buffer;
        buffer.size         = OutputBytes;
        buffer.allocateType = RHIBufferAllocateType::eGPU;
        buffer.usageFlags.SetFlags(RHIBufferUsageFlagBits::eStorageBuffer, RHIBufferUsageFlagBits::eTransferSrcBuffer);
        output              = device.CreateBuffer(buffer);
        buffer.allocateType = RHIBufferAllocateType::eCPURead;
        buffer.usageFlags.Clear();
        buffer.usageFlags.SetFlag(RHIBufferUsageFlagBits::eTransferDstBuffer);
        readback                      = device.CreateBuffer(buffer);
        RHISamplerCreateInfo sampling = RHISamplerCreateInfo::CreateLinearRepeat();
        sampling.repeatU = sampling.repeatV = RHISamplerRepeatMode::eClampToEdge;
        sampling.maxLod                     = 0;
        sampler                             = device.CreateSampler(sampling);
        valid = texture != nullptr && texture->GetDefaultView() != nullptr && output != nullptr && readback != nullptr
             && sampler != nullptr;
    }
    RenderGraph graph("spatial_sampling_qualification");
    if (valid)
    {
        // Independent red/green ramps distinguish X/Y coordinate rounding.
        const uint8_t              pixels[] = {0, 0, 0, 255, 255, 0, 255, 255, 0, 255, 255, 255, 255, 255, 0, 255};
        RHIBufferTextureCopyRegion region;
        region.textureSize = {2, 2, 1};
        region.textureSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
        region.textureSubresources.layerCount = 1;
        device.UpdateTexture(texture, {&region, 1}, sizeof(pixels), pixels);
        valid = graph.Begin();
        if (valid)
        {
            RDGComputePassDesc pass;
            pass.SetShaderProgramName("SpatialSamplingQualificationSP");
            pass.BindSampledTexture("calibrationTexture", sampler, texture->GetDefaultView());
            pass.BindStorageBuffer("Results", output, RDGContentGuarantee::eFullWrite);
            graph.AddComputePass(std::move(pass)).RecordPassCommands(DispatchSpatialQualification);
            graph.AddTransferPass("spatial_qualification_readback").CopyBuffer(output, readback, {0, 0, OutputBytes});
            valid = graph.End() && device.ExecuteRenderGraph(graph);
        }
        if (valid)
        {
            // Qualification needs CPU-visible results before publishing material
            // parameters. Wait for this graphics submission, not device-wide idle.
            const uint64_t      serial = device.GetSubmittedSerials().Get(RHICommandContextType::eGraphics);
            const RHIWaitResult waited = GDynamicRHI->WaitForCompletionChecked(RHICommandContextType::eGraphics, serial);
            valid                      = serial != 0 && waited.outcome == RHIWaitOutcome::eCompleted;
        }
        if (valid)
        {
            const uint8_t* mapped = readback->Map();
            valid                 = mapped != nullptr;
            if (valid)
            {
                rounding = ClassifySpatialTextureSampling({reinterpret_cast<const Vec4*>(mapped), QueryCount * 3});
                readback->Unmap();
                valid = rounding != SpatialTextureRounding::eUnqualified;
            }
        }
    }
    graph.Reset();
    device.DestroyBuffer(readback);
    device.DestroyBuffer(output);
    device.DestroyTexture(texture);
    return valid;
}
} // namespace

SpatialTextureRounding ClassifySpatialTextureSampling(VectorView<const Vec4> observations)
{
    bool halfUp     = !observations.empty() && observations.size() % 3 == 0;
    bool tiesToEven = halfUp;
    for (size_t index = 0; (halfUp || tiesToEven) && index < observations.size(); index += 3)
    {
        for (uint32_t channel = 0; channel < 2; ++channel)
        {
            const float native  = observations[index][channel];
            halfUp             &= std::isfinite(native) && native == observations[index + 1][channel];
            tiesToEven         &= std::isfinite(native) && native == observations[index + 2][channel];
        }
    }
    return halfUp == tiesToEven ? SpatialTextureRounding::eUnqualified
         : halfUp               ? SpatialTextureRounding::eHalfUp
                                : SpatialTextureRounding::eTiesToEven;
}

bool RenderDevice::QualifySpatialTextureSampling()
{
    bool valid = m_spatialTextureRounding != SpatialTextureRounding::eUnqualified;
    if (!valid && GetGPUInfo().subTexelPrecisionBits > 0 && GetGPUInfo().subTexelPrecisionBits <= 23
        && CanReconfigureResources())
    {
        SpatialTextureRounding measured = SpatialTextureRounding::eUnqualified;
        valid                           = MeasureSpatialTextureSampling(*this, measured);
        if (valid)
        {
            m_spatialTextureRounding = measured;
            LOGI("Spatial texture sampling qualified: {}",
                 measured == SpatialTextureRounding::eHalfUp ? "half-up" : "ties-to-even");
        }
    }
    if (!valid)
    {
        LOGE("Spatial texture sampler qualification failed; scene publication requires a supported native rounding rule");
    }
    return valid;
}

Vec4 RenderDevice::GetTextureSamplingParameters() const
{
    return Vec4(std::exp2(static_cast<float>(GetGPUInfo().subTexelPrecisionBits)), static_cast<float>(m_spatialTextureRounding),
                0, 0);
}
} // namespace zen::rc
