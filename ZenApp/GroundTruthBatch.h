#pragma once
#include "Runtime/RuntimeLaunchSettings.h"

namespace zen::rc
{
class RenderDevice;
}
namespace zen
{
class RHITexture;
class RHIBuffer;
} // namespace zen

namespace zen::runtime
{
struct GroundTruthFrame
{
    std::string           id;
    RuntimeLaunchSettings setup;
    double                noiseRms[3]{};
    HeapVector<uint64_t>  hybridPixels;
};

bool ReadGroundTruthBatch(const std::string& directory, HeapVector<GroundTruthFrame>& frames, std::string& error);
bool IsRTSSLoaded();
bool CaptureGroundTruthEnvironment(rc::RenderDevice& device, RHITexture* source, const std::string& path);
bool WriteSelectedHybridCapture(RHIBuffer& buffer, const GroundTruthFrame& frame, const std::string& path);
} // namespace zen::runtime
