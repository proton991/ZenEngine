#pragma once
#include "Math/Math.h"
#include "Templates/HeapVector.h"
#include <cstdint>
#include <cstddef>

namespace zen::platform
{
class ConfigLoader;
}

namespace zen::rc
{
constexpr uint32_t MaxSceneLights = 32;
using LightId                     = uint64_t;

enum class SceneLightType : uint32_t
{
    eDirectional,
    ePoint,
    eSpot
};

struct SceneLight
{
    SceneLightType type{SceneLightType::ePoint};
    Vec3 position{0.0f};
    Vec3 direction{0.0f, -1.0f, 0.0f};
    Vec3 color{1.0f};
    float intensity{5.0f};
    float range{1000.0f};
    float innerAngleDegrees{20.0f};
    float outerAngleDegrees{35.0f};
    bool enabled{true};
    bool castsShadows{true};
};

// vec4-only layout shared with Common/scene_lighting.glsl (std140).
struct GPULight
{
    Vec4 positionRange{};
    Vec4 directionType{};
    Vec4 colorIntensity{};
    Vec4 coneShadow{};
};

struct SceneUniformData
{
    GPULight lights[MaxSceneLights]{};
    Vec4 viewPos{};
    Vec4 lightInfo{};                         // enabled count; remaining lanes reserved
    Vec4 environment{1.0f, 0.0f, 1.0f, 1.0f}; // intensity, rotation radians, enabled, visible
};
static_assert(sizeof(GPULight) == 64);
static_assert(offsetof(SceneUniformData, viewPos) == MaxSceneLights * 64);
static_assert(sizeof(SceneUniformData) == MaxSceneLights * 64 + 48);

struct LightEntry
{
    LightId id{0};
    SceneLight light;
};

class SceneLights
{
public:
    LightId Add(const SceneLight& light);
    bool Update(LightId id, const SceneLight& light);
    bool Remove(LightId id);
    const SceneLight* Find(LightId id) const;
    void WriteUniforms(SceneUniformData& uniforms) const;
    uint64_t GetRevision() const
    {
        return m_revision;
    }
    uint64_t GetStructureRevision() const
    {
        return m_structureRevision;
    }
    static bool Validate(const SceneLight& light);

private:
    HeapVector<LightEntry> m_lights;
    LightId m_nextId{1};
    uint64_t m_revision{1};
    uint64_t m_structureRevision{1};
};

// Keeps config slots stable even when an invalid light is skipped.
struct ConfiguredLight
{
    uint32_t configIndex{0};
    SceneLight light;
};
HeapVector<ConfiguredLight> LoadSceneLights(const platform::ConfigLoader& config);
} // namespace zen::rc
