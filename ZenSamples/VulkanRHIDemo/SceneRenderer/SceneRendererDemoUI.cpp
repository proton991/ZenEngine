#include "SceneRendererDemo.h"
#include "Graphics/RenderCore/V2/Renderer/RendererServer.h"
#include "Graphics/RenderCore/V2/Renderer/DeferredLightingRenderer.h"

namespace zen
{
ui::RuntimeSceneSettings SceneRendererDemo::GetRuntimeSceneSettings() const
{
    ui::RuntimeSceneSettings settings;

    settings.cameraPosition = m_camera->GetPos();

    const rc::SceneUniformData& uniforms =
        *reinterpret_cast<const rc::SceneUniformData*>(m_renderScene->GetSceneUniformData());

    settings.environmentIntensity = uniforms.environment.x;

    settings.environmentRotation = glm::degrees(uniforms.environment.y);

    settings.environmentEnabled = uniforms.environment.z != 0;

    settings.skyboxVisible = uniforms.environment.w != 0;

    const rc::DeferredLightingRenderer& lighting =
        *m_renderDevice->GetRendererServer()->RequestDeferredLightingRenderer();

    settings.markersEnabled = lighting.GetLightMarkersEnabled();

    settings.markerSize = lighting.GetLightMarkerSize();

    settings.lightCount = m_configLightCount;

    for (uint32_t index = 0; index < settings.lightCount; ++index)
    {
        const rc::SceneLight* light = m_renderScene->GetLights().Find(m_configLightIds[index]);

        if (light != nullptr)
        {
            settings.lights[index] = *light;
        }
        else
        {
            settings.lights[index].enabled = false;
        }
    }

    settings.animationEnabled = m_dynamicLight != 0;

    settings.animatedLight = m_animatedLightIndex;

    settings.orbitCenter = m_orbitCenter;

    settings.orbitRadius = m_orbitRadius;

    settings.orbitSpeed = m_orbitSpeedDegrees;

    return settings;
}

bool SceneRendererDemo::ApplyRuntimeSceneSettings(const ui::RuntimeSceneSettings& previous,
                                                  const ui::RuntimeSceneSettings& next)
{
    bool applied = ui::ValidateRuntimeSceneSettings(next);

    if (applied)
    {
        rc::SceneLights& lights = m_renderScene->GetLights();

        for (uint32_t index = next.lightCount; index < m_configLightCount; ++index)
        {
            if (lights.Find(m_configLightIds[index]) != nullptr)
            {
                lights.Remove(m_configLightIds[index]);
            }

            m_configLightIds[index] = 0;
        }

        for (uint32_t index = 0; index < next.lightCount; ++index)
        {
            const rc::SceneLight* current = lights.Find(m_configLightIds[index]);

            rc::SceneLight light = next.lights[index];

            if (current != nullptr)
            {
                light = ui::MergeRuntimeLightEdit(*current, previous.lights[index], light);

                const bool changed = light.type != current->type ||
                    light.position != current->position || light.direction != current->direction ||
                    light.color != current->color || light.intensity != current->intensity ||
                    light.range != current->range ||
                    light.innerAngleDegrees != current->innerAngleDegrees ||
                    light.outerAngleDegrees != current->outerAngleDegrees ||
                    light.enabled != current->enabled ||
                    light.castsShadows != current->castsShadows;

                if (changed)
                {
                    applied &= lights.Update(m_configLightIds[index], light);
                }
            }
            else
            {
                m_configLightIds[index] = lights.Add(light);

                applied &= m_configLightIds[index] != 0;
            }
        }

        m_configLightCount = next.lightCount;

        m_animatedLightIndex = next.animatedLight;

        m_dynamicLight = next.animationEnabled ? m_configLightIds[next.animatedLight] : 0;

        m_orbitCenter = next.orbitCenter;

        m_orbitRadius = next.orbitRadius;

        m_orbitSpeedDegrees = next.orbitSpeed;

        if (previous.cameraPosition != next.cameraPosition)
        {
            m_camera->SetPosition(next.cameraPosition);
        }

        applied &= m_renderScene->SetEnvironmentLighting(
            next.environmentIntensity, next.environmentRotation, next.environmentEnabled,
            next.skyboxVisible);

        applied &=
            m_renderDevice->GetRendererServer()->RequestDeferredLightingRenderer()->SetLightMarkers(
                next.markersEnabled, next.markerSize);
    }

    return applied;
}
} // namespace zen
