#include "GroundTruthBatch.h"
#include "RuntimeJson.h"
#include <algorithm>
#include <fstream>
#include "Graphics/RenderCore/V2/RenderDevice.h"
#if defined(_WIN32)
#    include <Windows.h>
#endif

namespace zen::runtime
{
bool CaptureGroundTruthEnvironment(rc::RenderDevice& device, RHITexture* source, const std::string& path)
{
    const uint32_t      size = source->GetWidth();
    RHIBufferCreateInfo info;
    info.size         = uint64_t(size) * size * 6 * sizeof(Vec4);
    info.allocateType = RHIBufferAllocateType::eGPU;
    info.usageFlags.SetFlags(RHIBufferUsageFlagBits::eStorageBuffer, RHIBufferUsageFlagBits::eTransferSrcBuffer);
    RHIBuffer* output = device.CreateBuffer(info);
    info.allocateType = RHIBufferAllocateType::eCPURead;
    info.usageFlags   = 0;
    info.usageFlags.SetFlag(RHIBufferUsageFlagBits::eTransferDstBuffer);
    RHIBuffer*  readback = device.CreateBuffer(info);
    RHISampler* sampler  = device.CreateSampler(RHISamplerCreateInfo::CreateLinearRepeat());
    bool        valid    = output != nullptr && readback != nullptr && sampler != nullptr;
    if (valid)
    {
        rc::RenderGraph graph("GroundTruthEnvironment");
        valid = graph.Begin();
        rc::RDGComputePassDesc pass;
        pass.SetShaderProgramName("CaptureHybridEnvironmentSP");
        pass.BindSampledTexture("sourceEnvironment", sampler, source->GetDefaultView());
        pass.BindStorageBuffer("EnvironmentCapture", output, rc::RDGContentGuarantee::eFullWrite);
        graph.AddComputePass(std::move(pass)).RecordPassCommands([size](rc::RDGPassCmdEncoder& encoder) {
            encoder.Dispatch((size + 7) / 8, (size + 7) / 8, 6);
        });
        graph.AddTransferPass("ReadGroundTruthEnvironment").CopyBuffer(output, readback, {0, 0, info.size}).NeverCull();
        valid = valid && graph.End() && device.ExecuteRenderGraph(graph);
        device.FlushRHIThread();
        device.WaitForIdle();
        valid               = valid && device.PollFrameSubmissions();
        const uint8_t* data = valid ? readback->Map() : nullptr;
        valid               = valid && data != nullptr;
        if (data != nullptr)
        {
            std::ofstream file(std::filesystem::u8path(path), std::ios::binary);
            file.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(info.size));
            file.close();
            valid = valid && file.good();
            readback->Unmap();
        }
    }
    device.DestroyBuffer(output);
    device.DestroyBuffer(readback);
    return valid;
}

bool IsRTSSLoaded()
{
    bool loaded = false;
#if defined(_WIN32)
    loaded = GetModuleHandleW(L"RTSSHooks64.dll") != nullptr || GetModuleHandleW(L"RTSSHooks.dll") != nullptr;
#endif
    return loaded;
}

namespace
{
constexpr uint64_t kMaxDatasetManifestBytes = 64ull * 1024 * 1024;

// Batch frames share one window, device, package and scene preparation; only the camera,
// environment and rendering settings change between them.
bool IsCompatibleBatchSetup(const RuntimeLaunchSettings& setup, const RuntimeLaunchSettings& first)
{
    return setup.packagePath == first.packagePath && setup.sourcePath == first.sourcePath
        && setup.processingProfile == first.processingProfile && setup.engineConfiguration == first.engineConfiguration
        && setup.configurationDirectory == first.configurationDirectory && setup.defaultEnvironment == first.defaultEnvironment
        && setup.run == first.run && setup.rendering.gi.asyncCompute == first.rendering.gi.asyncCompute
        && setup.rendering.normalizationCenter == first.rendering.normalizationCenter
        && setup.rendering.normalizationScale == first.rendering.normalizationScale && !setup.animationEnabled
        && !setup.rendering.normalMaps && !setup.run.followWindow && !setup.run.fullscreen
        && setup.rendering.algorithm == rc::RenderAlgorithm::eVoxelGI;
}
} // namespace

bool ReadGroundTruthBatch(const std::string& directory, HeapVector<GroundTruthFrame>& frames, std::string& error)
{
    simdjson::dom::parser       parser;
    simdjson::padded_string     json;
    simdjson::dom::element      document;
    simdjson::dom::array        entries;
    const std::filesystem::path root = std::filesystem::u8path(directory);
    bool valid                       = LoadJsonDocument(root / "dataset.json", kMaxDatasetManifestBytes, parser, json, document)
                                    && !document["frames"].get_array().get(entries);
    if (valid)
    {
        for (simdjson::dom::element entry : entries)
        {
            std::string_view id;
            GroundTruthFrame frame;
            valid = valid && !entry["id"].get_string().get(id);
            if (valid)
            {
                frame.id = id;
                valid    = !id.empty() && id != "." && id != ".."
                        && id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-")
                               == std::string_view::npos;
            }
            if (valid)
            {
                valid = ReadRuntimeLaunchSettings(root / "frames" / frame.id / "scene.zenrender", frame.setup, error);
                const char* terms[] = {"D_direct", "D_sky", "D_bounce"};
                for (uint32_t term = 0; term < 3 && valid; ++term)
                {
                    valid =
                        !entry["noise_rms"][terms[term]].get_double().get(frame.noiseRms[term]) && frame.noiseRms[term] >= 0;
                }
            }
            if (valid)
            {
                const RuntimeLaunchSettings& first = frames.empty() ? frame.setup : frames.front().setup;
                valid                              = IsCompatibleBatchSetup(frame.setup, first)
                                                  && std::none_of(frames.begin(), frames.end(),
                                                                  [&frame](const GroundTruthFrame& old) { return old.id == frame.id; });
            }
            if (valid)
            {
                frames.push_back(std::move(frame));
            }
            else
            {
                break;
            }
        }
    }
    valid = valid && !frames.empty();
    if (!valid)
    {
        frames.clear();
        if (error.empty())
        {
            error = "Invalid ground-truth dataset or incompatible batch setups";
        }
    }
    return valid;
}
} // namespace zen::runtime
