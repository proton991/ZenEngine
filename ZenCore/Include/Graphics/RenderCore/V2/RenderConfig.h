#pragma once

namespace zen::rc
{
struct RenderConfig
{
    static RenderConfig& GetInstance()
    {
        static RenderConfig instance;
        return instance;
    }
    // Depth bias (and slope) are used to avoid shadowing artifacts
    // Constant depth bias factor (always applied)
    float depthBiasConstant = 1.25f;
    // Slope depth bias factor, applied depending on polygon's slope
    float depthBiasSlope = 1.75f;
    // Size of shadow map
    uint32_t shadowMapSize = 2048;

    uint32_t offScreenFbSize = 2048;

    DataFormat shadowDepthFormat{DataFormat::eD16UNORM};
};
} // namespace zen::rc