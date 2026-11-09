#pragma once
#include "Runtime/RuntimeLaunchSettings.h"

namespace zen::rc
{
class RenderDevice;
}
namespace zen
{
class RHITexture;
}

namespace zen::runtime
{
struct GroundTruthFrame
{
    std::string           id;
    RuntimeLaunchSettings setup;
    double                noiseRms[3]{};
};

bool ReadGroundTruthBatch(const std::string& directory, HeapVector<GroundTruthFrame>& frames, std::string& error);
bool IsRTSSLoaded();
bool CaptureGroundTruthEnvironment(rc::RenderDevice& device, RHITexture* source, const std::string& path);
} // namespace zen::runtime
