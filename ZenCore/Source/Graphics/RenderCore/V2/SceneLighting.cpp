#include "Graphics/RenderCore/V2/SceneLighting.h"
#include "Graphics/RenderCore/V2/RenderScene.h"
#include "Platform/ConfigLoader.h"
#include <cmath>
#include <limits>

namespace zen::rc
{
namespace
{
bool FiniteVector(const Vec3& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}
} // namespace

bool ValidateCameraLight(const CameraLightSettings& settings)
{
    return FiniteVector(settings.color) && glm::all(glm::greaterThanEqual(settings.color, Vec3(0.0f)))
        && std::isfinite(settings.intensity) && settings.intensity >= 0.0f && std::isfinite(settings.range)
        && std::isfinite(settings.radius) && settings.radius > 0.0f && settings.range > settings.radius
        && std::isfinite(settings.followDistance) && settings.followDistance > settings.radius
        && FiniteVector(settings.position);
}

Vec3 CameraLightPosition(const CameraLightSettings& settings, const Vec3& eye, const Vec3& forward)
{
    return settings.followCamera ? eye + forward * settings.followDistance : settings.position;
}

uint32_t CameraLightShadowFaces(const CameraLightSettings& settings)
{
    return settings.enabled && settings.intensity > 0.0f ? 6u : 0u;
}

bool EqualSceneLight(const SceneLight& left, const SceneLight& right)
{
    return left.type == right.type && left.position == right.position && left.direction == right.direction
        && left.color == right.color && left.intensity == right.intensity && left.range == right.range
        && left.innerAngleDegrees == right.innerAngleDegrees && left.outerAngleDegrees == right.outerAngleDegrees
        && left.enabled == right.enabled && left.castsShadows == right.castsShadows;
}

bool RenderScene::SetEnvironmentLighting(float intensity, float rotationDegrees, bool enabled, bool visible)
{
    const bool valid = ValidateEnvironmentLighting(intensity, rotationDegrees);

    if (valid)
    {
        m_environmentIntensity = intensity;

        const Vec4 environment(intensity * m_authoredEnvironmentIntensity, glm::radians(rotationDegrees), enabled ? 1.0f : 0.0f,
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

bool RenderScene::ValidateEnvironmentLighting(float intensity, float rotationDegrees) const
{
    return std::isfinite(intensity) && intensity >= 0.0f && std::isfinite(rotationDegrees)
        && std::isfinite(intensity * m_authoredEnvironmentIntensity);
}

bool SceneLights::Validate(const SceneLight& light)
{
    const glm::dvec3 direction(light.direction);

    return light.type <= SceneLightType::eSpot && FiniteVector(light.position) && FiniteVector(light.direction)
        && FiniteVector(light.color) && glm::all(glm::greaterThanEqual(light.color, Vec3(0.0f)))
        && std::isfinite(light.intensity) && light.intensity >= 0.0f && std::isfinite(light.range) && light.range >= 0.0f
        && glm::dot(direction, direction) > 1e-12 && std::isfinite(light.innerAngleDegrees)
        && std::isfinite(light.outerAngleDegrees) && light.innerAngleDegrees >= 0.0f
        && light.innerAngleDegrees < light.outerAngleDegrees && light.outerAngleDegrees <= 90.0f;
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
                if (entry.light.enabled != light.enabled || entry.light.type != light.type
                    || entry.light.castsShadows != light.castsShadows
                    || (entry.light.intensity > 0.0f) != (light.intensity > 0.0f))
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

    for (HeapVector<LightEntry>::iterator entry = m_lights.begin(); entry != m_lights.end(); ++entry)
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
            gpu.directionType  = Vec4(Vec3(glm::normalize(glm::dvec3(light.direction))), static_cast<float>(light.type));

            gpu.colorIntensity = Vec4(light.color, light.intensity);

            gpu.coneShadow     = Vec4(std::cos(glm::radians(light.innerAngleDegrees)),
                                      std::cos(glm::radians(light.outerAngleDegrees)), light.castsShadows ? 1.0f : 0.0f, 0.0f);
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

    const zen::HeapVector<sg::Light*> imported = scene.GetComponents<sg::Light>();

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

            light.type                                                  = static_cast<SceneLightType>(source->GetType());

            light.position                                              = properties.position;

            light.direction                                             = Vec3(properties.direction);

            light.color                                                 = Vec3(properties.color);

            light.intensity                                             = properties.intensity;

            light.range                                                 = properties.range;

            light.innerAngleDegrees                                     = glm::degrees(properties.innerConeAngle);

            light.outerAngleDegrees                                     = glm::degrees(properties.outerConeAngle);

            const HashMap<const sg::Light*, bool>::const_iterator owner = visibility.find(source);

            light.enabled                                               = owner == visibility.end() || owner->second;

            lights.push_back(light);
        }
    }

    return lights;
}


HeapVector<SceneLight> BuildBoundsLightPreset(const sg::AABB& bounds, bool corners)
{
    HeapVector<SceneLight> lights;

    const Vec3 minimum = bounds.GetMin();

    const Vec3 maximum = bounds.GetMax();

    if (FiniteVector(minimum) && FiniteVector(maximum) && glm::all(glm::lessThanEqual(minimum, maximum)))
    {
        const Vec3 center  = minimum * 0.5f + maximum * 0.5f;

        const float margin = std::max(bounds.GetMaxExtent() * 0.15f, 0.01f);

        for (uint32_t index = 0; index < (corners ? 8u : 6u); ++index)
        {
            SceneLight light;

            light.position = center;

            if (corners)
            {
                Vec3 diagonal;

                for (uint32_t axis = 0; axis < 3; ++axis)
                {
                    const bool positive  = (index & (1u << axis)) != 0;

                    light.position[axis] = positive ? maximum[axis] : minimum[axis];

                    diagonal[axis]       = positive ? 1.0f : -1.0f;
                }

                const Vec3 offset  = light.position - center;

                light.position    += glm::normalize(glm::length(offset) > 1e-6f ? offset : diagonal) * margin;
            }
            else
            {
                const uint32_t axis  = index / 2;

                light.position[axis] = (index & 1) != 0 ? maximum[axis] + margin : minimum[axis] - margin;
            }

            light.direction      = glm::normalize(center - light.position);

            const float distance = glm::distance(center, light.position);

            light.intensity      = distance * distance * 3.0f;

            light.range          = 0.0f;

            lights.push_back(light);
        }
    }

    return lights;
}

uint32_t CountShadowFaces(const HeapVector<SceneLight>& lights)
{
    uint32_t faces = 0;

    for (const SceneLight& light : lights)
    {
        if (light.enabled && light.castsShadows && light.intensity > 0.0f)
        {
            faces += light.type == SceneLightType::ePoint ? 6 : 1;
        }
    }

    return faces;
}

uint64_t EstimateShadowBytes(uint32_t resolution, uint32_t faces)
{
    return uint64_t(resolution) * resolution * 4 * std::max(2u, faces);
}

bool ValidateShadowMemory(uint32_t resolution, uint32_t faces, uint64_t availableBytes)
{
    return resolution >= 128 && resolution <= 2048 && faces <= (MaxSceneLights + 1) * 6
        && EstimateShadowBytes(resolution, faces) + uint64_t(resolution) * resolution * 4 <= availableBytes;
}

bool SceneLights::Replace(HeapVector<LightEntry>& entries)
{
    bool valid      = entries.size() <= MaxSceneLights;

    uint64_t newIds = 0;

    for (size_t index = 0; valid && index < entries.size(); ++index)
    {
        valid   = Validate(entries[index].light) && (entries[index].id == 0 || Find(entries[index].id) != nullptr);

        newIds += entries[index].id == 0 ? 1 : 0;

        for (size_t previous = 0; valid && previous < index; ++previous)
        {
            valid = entries[index].id == 0 || entries[index].id != entries[previous].id;
        }
    }

    valid = valid && (newIds == 0 || (m_nextId != 0 && newIds - 1 <= std::numeric_limits<LightId>::max() - m_nextId));

    if (valid)
    {
        for (LightEntry& entry : entries)
        {
            if (entry.id == 0)
            {
                entry.id = m_nextId++;
            }
        }

        m_lights = entries;

        ++m_revision;

        ++m_structureRevision;
    }

    return valid;
}

bool RenderScene::ReplaceLights(HeapVector<LightEntry>& entries)
{
    const bool valid = m_lights.Replace(entries);

    if (valid)
    {
        m_importedLightIds.clear();
    }

    return valid;
}

bool RenderScene::SetCameraLight(const CameraLightSettings& settings)
{
    const bool valid = ValidateCameraLight(settings);

    if (valid)
    {
        m_cameraLight = settings;
    }

    return valid;
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
            const std::string key   = "light." + std::to_string(i) + ".";

            const std::string type  = config.GetString(key + "type", "point");

            valid                   = type == "point" || type == "directional" || type == "spot";

            light.type              = type == "directional" ? SceneLightType::eDirectional
                                                            : (type == "spot" ? SceneLightType::eSpot : SceneLightType::ePoint);

            valid                  &= config.ReadVec3(key + "position", light.position);

            valid                  &= config.ReadVec3(key + "direction", light.direction);

            valid                  &= config.ReadVec3(key + "color", light.color);

            valid                  &= config.ReadNumber(key + "intensity", light.intensity);

            valid                  &= config.ReadNumber(key + "range", light.range);

            valid                  &= config.ReadNumber(key + "inner_angle_degrees", light.innerAngleDegrees);

            valid                  &= config.ReadNumber(key + "outer_angle_degrees", light.outerAngleDegrees);

            valid                  &= config.ReadBool(key + "enabled", light.enabled);

            valid                  &= config.ReadBool(key + "casts_shadows", light.castsShadows);
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
