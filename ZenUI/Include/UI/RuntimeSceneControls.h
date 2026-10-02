#pragma once

#include "Graphics/RenderCore/V2/SceneLighting.h"
#include "AssetLib/GLTFModelCatalog.h"
#include <array>

namespace zen::ui
{
struct RuntimeModelState
{
    std::string                              basePath;
    HeapVector<asset::GLTFModelCatalogEntry> models;
    std::string                              currentPath;
    std::string                              pendingPath;
    std::string                              error;
    uint64_t                                 revision{0};
};

struct RuntimeSceneSettings
{
    Vec3                                           cameraPosition{0.0f};
    float                                          environmentIntensity{1.0f};
    float                                          environmentRotation{0.0f};
    bool                                           environmentEnabled{true};
    bool                                           skyboxVisible{true};
    bool                                           markersEnabled{false};
    float                                          markerSize{0.02f};
    uint32_t                                       lightCount{0};
    bool                                           boundsPresetLights{false};
    uint32_t                                       modelLightCount{0};
    std::array<rc::SceneLight, rc::MaxSceneLights> lights{};
    bool                                           animationEnabled{false};
    uint32_t                                       animatedLight{0};
    Vec3                                           orbitCenter{0.0f, 1.0f, 0.0f};
    float                                          orbitRadius{1.0f};
    float                                          orbitSpeed{45.0f};
};

bool ValidateRuntimeSceneSettings(const RuntimeSceneSettings& settings);

rc::SceneLight MergeRuntimeLightEdit(const rc::SceneLight& current,
                                     const rc::SceneLight& previous,
                                     const rc::SceneLight& requested);

// The application owns scene/camera/animation lifetime. The panel owns only a draft.
// Apply compares against the original draft so unrelated edits preserve animated values.
class RuntimeSceneControls
{
public:
    virtual ~RuntimeSceneControls()                                                                                = default;

    virtual RuntimeSceneSettings GetRuntimeSceneSettings() const                                                   = 0;

    virtual bool ApplyRuntimeSceneSettings(const RuntimeSceneSettings& previous, const RuntimeSceneSettings& next) = 0;

    virtual const RuntimeModelState& GetRuntimeModelState() const;

    virtual void RefreshRuntimeModels();

    virtual bool RequestRuntimeModel(const std::string& path);
};
} // namespace zen::ui
