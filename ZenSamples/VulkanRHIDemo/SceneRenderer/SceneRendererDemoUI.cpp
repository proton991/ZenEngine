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

    settings.lightCount = m_editableLightCount;

    settings.boundsPresetLights = m_boundsPresetLights;

    settings.modelLightCount = m_modelLightCount;

    settings.lights = m_editableLightDefaults;

    for (uint32_t index = 0; index < settings.lightCount; ++index)
    {
        const rc::SceneLight* light = m_renderScene->GetLights().Find(m_editableLightIds[index]);

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

    // The model's animated lights cannot be removed by the editable-light panel.
    // Check real ownership before modifying anything, even if draft metadata changed.
    applied = applied && next.lightCount <= rc::MaxSceneLights - m_modelLightCount;

    if (applied)
    {
        rc::SceneLights& lights = m_renderScene->GetLights();

        for (uint32_t index = next.lightCount; index < m_editableLightCount; ++index)
        {
            const rc::SceneLight* current = lights.Find(m_editableLightIds[index]);

            if (current != nullptr)
            {
                m_editableLightDefaults[index] = *current;

                lights.Remove(m_editableLightIds[index]);
            }

            m_editableLightIds[index] = 0;
        }

        for (uint32_t index = 0; index < next.lightCount; ++index)
        {
            const rc::SceneLight* current = lights.Find(m_editableLightIds[index]);

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
                    applied &= lights.Update(m_editableLightIds[index], light);
                }
            }
            else
            {
                m_editableLightIds[index] = lights.Add(light);

                applied &= m_editableLightIds[index] != 0;
            }
        }

        m_editableLightCount = next.lightCount;

        m_animatedLightIndex = next.animatedLight;

        m_dynamicLight = next.animationEnabled ? m_editableLightIds[next.animatedLight] : 0;

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
