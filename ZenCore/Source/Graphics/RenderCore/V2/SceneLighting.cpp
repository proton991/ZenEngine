#include "Graphics/RenderCore/V2/SceneLighting.h"
#include "Graphics/RenderCore/V2/RenderScene.h"
#include "Platform/ConfigLoader.h"
#include <cmath>

namespace zen::rc
{
namespace
{
bool FiniteVector(const Vec3& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}
} // namespace

bool RenderScene::SetEnvironmentLighting(float intensity,
                                         float rotationDegrees,
                                         bool enabled,
                                         bool visible)
{
    const bool valid = std::isfinite(intensity) && intensity >= 0.0f &&
        std::isfinite(rotationDegrees) && std::isfinite(intensity * m_authoredEnvironmentIntensity);
    if (valid)
    {
        m_environmentIntensity = intensity;

        const Vec4 environment(intensity * m_authoredEnvironmentIntensity,
                               glm::radians(rotationDegrees), enabled ? 1.0f : 0.0f,
                               visible ? 1.0f : 0.0f);
        // Background visibility does not change the irradiance seen by scene surfaces.
        if (Vec3(environment) != Vec3(m_sceneUniformData.environment))
        {
            ++m_environmentRevision;
        }
        m_sceneUniformData.environment = environment;
    }
    return valid;
}

bool SceneLights::Validate(const SceneLight& light)
{
    const glm::dvec3 direction(light.direction);
    return light.type <= SceneLightType::eSpot && FiniteVector(light.position) &&
        FiniteVector(light.direction) && FiniteVector(light.color) &&
        glm::all(glm::greaterThanEqual(light.color, Vec3(0.0f))) &&
        std::isfinite(light.intensity) && light.intensity >= 0.0f && std::isfinite(light.range) &&
        light.range >= 0.0f && glm::dot(direction, direction) > 1e-12 &&
        std::isfinite(light.innerAngleDegrees) && std::isfinite(light.outerAngleDegrees) &&
        light.innerAngleDegrees >= 0.0f && light.innerAngleDegrees < light.outerAngleDegrees &&
        light.outerAngleDegrees <= 90.0f;
}

LightId SceneLights::Add(const SceneLight& light)
{
    LightId result = 0;
    if (Validate(light) && m_lights.size() < MaxSceneLights && m_nextId != 0)
    {
        result = m_nextId++;
        m_lights.push_back({result, light});
        ++m_revision;
        ++m_structureRevision;
    }
    else
    {
        LOGW("Cannot add scene light: invalid parameters or light capacity exhausted");
    }
    return result;
}

bool SceneLights::Update(LightId id, const SceneLight& light)
{
    bool updated = false;
    if (Validate(light))
    {
        for (LightEntry& entry : m_lights)
        {
            if (entry.id == id)
            {
                if (entry.light.enabled != light.enabled || entry.light.type != light.type ||
                    entry.light.castsShadows != light.castsShadows)
                {
                    ++m_structureRevision;
                }
                entry.light = light;
                ++m_revision;
                updated = true;
                break;
            }
        }
    }
    return updated;
}

bool SceneLights::Remove(LightId id)
{
    bool removed = false;
    for (HeapVector<LightEntry>::iterator entry = m_lights.begin(); entry != m_lights.end();
         ++entry)
    {
        if (entry->id == id)
        {
            m_lights.erase(entry);
            ++m_revision;
            ++m_structureRevision;
            removed = true;
            break;
        }
    }
    return removed;
}

const SceneLight* SceneLights::Find(LightId id) const
{
    const SceneLight* result = nullptr;
    for (const LightEntry& entry : m_lights)
    {
        if (entry.id == id)
        {
            result = &entry.light;
            break;
        }
    }
    return result;
}

void SceneLights::WriteUniforms(SceneUniformData& uniforms) const
{
    uint32_t count = 0;
    for (const LightEntry& entry : m_lights)
    {
        const SceneLight& light = entry.light;
        if (light.enabled)
        {
            GPULight& gpu     = uniforms.lights[count++];
            gpu.positionRange = Vec4(light.position, light.range);
            // Square finite float components in double precision to avoid normalization overflow.
            gpu.directionType  = Vec4(Vec3(glm::normalize(glm::dvec3(light.direction))),
                                      static_cast<float>(light.type));
            gpu.colorIntensity = Vec4(light.color, light.intensity);
            gpu.coneShadow     = Vec4(std::cos(glm::radians(light.innerAngleDegrees)),
                                      std::cos(glm::radians(light.outerAngleDegrees)),
                                  light.castsShadows ? 1.0f : 0.0f, 0.0f);
        }
    }
    for (uint32_t i = count; i < MaxSceneLights; ++i)
    {
        uniforms.lights[i] = {};
    }
    uniforms.lightInfo = Vec4(static_cast<float>(count), 0.0f, 0.0f, 0.0f);
}

HeapVector<SceneLight> BuildSceneLights(const sg::Scene& scene)
{
    HeapVector<SceneLight> lights;
    const std::vector<sg::Light*> imported = scene.GetComponents<sg::Light>();
    if (!imported.empty())
    {
        HashMap<const sg::Light*, bool> visibility;

        for (const UniquePtr<sg::Node>& node : scene.GetNodes())
        {
            if (node->HasComponent<sg::Light>())
            {
                visibility[node->GetComponent<sg::Light>()] = node->IsVisible();
            }
        }

        for (const sg::Light* source : imported)
        {
            const sg::LightProperties& properties = source->GetProperties();

            SceneLight light;

            light.type              = static_cast<SceneLightType>(source->GetType());
            light.position          = properties.position;
            light.direction         = Vec3(properties.direction);
            light.color             = Vec3(properties.color);
            light.intensity         = properties.intensity;
            light.range             = properties.range;
            light.innerAngleDegrees = glm::degrees(properties.innerConeAngle);
            light.outerAngleDegrees = glm::degrees(properties.outerConeAngle);

            const HashMap<const sg::Light*, bool>::const_iterator owner = visibility.find(source);

            light.enabled = owner == visibility.end() || owner->second;

            lights.push_back(light);
        }
    }
    else
    {
        const sg::AABB& bounds = scene.GetAABB();
        const Vec3 min         = bounds.GetMin();
        const Vec3 max         = bounds.GetMax();
        if (FiniteVector(min) && FiniteVector(max) && glm::all(glm::lessThanEqual(min, max)))
        {
            const Vec3 center  = min * 0.5f + max * 0.5f;
            const float span   = std::max(bounds.GetMaxExtent(), 0.01f);
            const float margin = std::max(span * 0.15f, 0.01f);
            for (uint32_t face = 0; face < 6; ++face)
            {
                const uint32_t axis = face / 2;
                const bool positive = (face & 1) != 0;
                SceneLight light;
                light.position = center;
                // Outside the complete world AABB guarantees no geometry contains the light.
                light.position[axis] = positive ? max[axis] + margin : min[axis] - margin;
                light.direction      = glm::normalize(center - light.position);
                const float distance = glm::distance(center, light.position);
                light.intensity      = distance * distance * 3.0f;
                light.range          = 0.0f;
                lights.push_back(light);
            }
        }
    }

    return lights;
}

HeapVector<ConfiguredLight> LoadSceneLights(const platform::ConfigLoader& config)
{
    HeapVector<ConfiguredLight> lights;
    uint32_t count            = 4;
    const bool explicitLights = config.HasKey("light_count");
    if (!config.ReadNumber("light_count", count) || count > MaxSceneLights)
    {
        LOGW("Invalid light_count; expected 0..{}", MaxSceneLights);
        count = 0;
    }
    for (uint32_t i = 0; i < count; ++i)
    {
        SceneLight light;
        bool valid = true;
        if (explicitLights)
        {
            const std::string key  = "light." + std::to_string(i) + ".";
            const std::string type = config.GetString(key + "type", "point");
            valid                  = type == "point" || type == "directional" || type == "spot";
            light.type             = type == "directional" ?
                            SceneLightType::eDirectional :
                            (type == "spot" ? SceneLightType::eSpot : SceneLightType::ePoint);
            valid &= config.ReadVec3(key + "position", light.position);
            valid &= config.ReadVec3(key + "direction", light.direction);
            valid &= config.ReadVec3(key + "color", light.color);
            valid &= config.ReadNumber(key + "intensity", light.intensity);
            valid &= config.ReadNumber(key + "range", light.range);
            valid &= config.ReadNumber(key + "inner_angle_degrees", light.innerAngleDegrees);
            valid &= config.ReadNumber(key + "outer_angle_degrees", light.outerAngleDegrees);
            valid &= config.ReadBool(key + "enabled", light.enabled);
            valid &= config.ReadBool(key + "casts_shadows", light.castsShadows);
        }
        else
        {
            light.position = Vec3((i & 1u) ? 1.0f : -1.0f, 1.0f, (i & 2u) ? 1.0f : -1.0f);
        }
        if (valid && SceneLights::Validate(light))
        {
            lights.push_back({i, light});
        }
        else
        {
            LOGW("Invalid light.{} configuration; skipping light", i);
        }
    }
    return lights;
}
} // namespace zen::rc
