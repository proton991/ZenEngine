#include "ImGui/LightingBackendControls.h"
#include "imgui.h"
#include <iterator>

namespace zen::ui
{
bool DrawGIPresetControls(rc::VoxelGIRuntimeSettings& settings, const char* selectionReason)
{
    bool                    changed  = false;
    const rc::VoxelGIPreset selected = rc::GetVoxelGIPreset(settings);
    if (ImGui::BeginCombo("GI preset", rc::GetVoxelGIPresetName(selected)))
    {
        for (uint32_t index = 1; index <= static_cast<uint32_t>(rc::VoxelGIPreset::Ultra); ++index)
        {
            const rc::VoxelGIPreset preset = static_cast<rc::VoxelGIPreset>(index);
            if (ImGui::Selectable(rc::GetVoxelGIPresetName(preset), selected == preset))
            {
                changed = rc::ApplyVoxelGIPreset(preset, settings);
            }
        }
        ImGui::EndCombo();
    }
    ImGui::TextWrapped(
        "Low: 64 cubed, specular occlusion only. Medium: 64 cubed with reflections. High: 128 cubed. Ultra: 256 cubed. All use per-pixel rays; GI rays not on HW RT trace bricks.");
    ImGui::TextWrapped("Selection: %s. GI tracing can override the preset preference.", selectionReason);
    return changed;
}

namespace
{
using Feature   = rc::VoxelGISettings::Feature;
using Provider  = rc::VoxelGISettings::RayProvider;
using Traversal = rc::VoxelGISettings::ComputeTraversal;

// The ray features each pass traces, in GIPass order. The sky cache and shadows trace one feature.
struct GIPassRow
{
    const char* name;
    const char* rays;
    Feature     first;
    Feature     second;
};
const GIPassRow kGIPassRows[] = {
    {"Diffuse GI", "Sky light and bounce rays", Feature::Sky, Feature::Bounce},
    {"Specular GI", "Occlusion and reflection rays", Feature::SpecularOcclusion, Feature::Reflections},
    {"Sky cache", "Sky visibility, on scene change", Feature::SkyCache, Feature::SkyCache},
    {"Direct shadows", "Shadow rays toward lights", Feature::Shadows, Feature::Shadows}};
static_assert(std::size(kGIPassRows) == static_cast<size_t>(GIPass::Count));

// Indexed by GITracer.
const char* const kTracerNames[] = {"HW RT", "Compute: Bricks", "Compute: Software BVH", "Shadow maps", "Off", "Mixed"};

bool IsComputeTracer(GITracer tracer)
{
    return tracer == GITracer::Bricks || tracer == GITracer::SoftwareBVH;
}

// Bricks serve only GI gather rays: the sky cache traces triangles, and direct shadows need exact
// geometry (brick shadows were blocky and leaky for little gain). Only direct shadows can use
// shadow maps. Diffuse and specular GI can be switched off to tell their contributions apart.
bool Offers(GIPass pass, GITracer tracer)
{
    return tracer == GITracer::HardwareRT || tracer == GITracer::SoftwareBVH
        || (tracer == GITracer::Bricks && (pass == GIPass::Diffuse || pass == GIPass::Specular))
        || (tracer == GITracer::ShadowMaps && pass == GIPass::Shadows)
        || (tracer == GITracer::Off && (pass == GIPass::Diffuse || pass == GIPass::Specular));
}
} // namespace

GITracer GetGIPassTracer(const rc::VoxelGISettings& settings, GIPass pass)
{
    const GIPassRow& row = kGIPassRows[static_cast<uint32_t>(pass)];
    // A disabled feature traces no rays, so its provider does not make the pass Mixed.
    bool      firstOn   = true;
    bool      secondOn  = false;
    Traversal traversal = settings.computeTraversal;
    switch (pass)
    {
        case GIPass::Diffuse:
            firstOn  = settings.indirectDiffuse;
            secondOn = settings.indirectDiffuse && settings.bounceSource != rc::VoxelGISettings::BounceSource::Cone;
            break;
        case GIPass::Specular:
            firstOn  = settings.specularOcclusion;
            secondOn = settings.specularOcclusion && settings.reflections != rc::VoxelGISettings::Reflections::Off;
            break;
        // The sky cache and direct shadows trace triangles under Compute.
        case GIPass::SkyCache: traversal = Traversal::Triangles; break;
        case GIPass::Shadows:
            firstOn   = settings.shadows && settings.rtShadows;
            traversal = Traversal::Triangles;
            break;
        default: break;
    }
    const Provider provider = settings.GetFeatureProvider(row.first);
    GITracer       tracer   = provider == Provider::Hardware ? GITracer::HardwareRT
                            : traversal == Traversal::Bricks ? GITracer::Bricks
                                                             : GITracer::SoftwareBVH;
    if (secondOn && settings.GetFeatureProvider(row.second) != provider)
    {
        tracer = GITracer::Mixed;
    }
    if (!firstOn)
    {
        tracer = pass == GIPass::Shadows && settings.shadows ? GITracer::ShadowMaps : GITracer::Off;
    }
    return tracer;
}

void SetGIPassTracer(rc::VoxelGISettings& settings, GIPass pass, GITracer tracer)
{
    const GIPassRow& row = kGIPassRows[static_cast<uint32_t>(pass)];
    if (tracer == GITracer::ShadowMaps && pass == GIPass::Shadows)
    {
        settings.shadows   = true;
        settings.rtShadows = false;
    }
    else if (tracer == GITracer::Off && Offers(pass, tracer))
    {
        // Reflections need specular occlusion rays, so specular GI stops with them.
        settings.indirectDiffuse   = pass == GIPass::Diffuse ? false : settings.indirectDiffuse;
        settings.specularOcclusion = pass == GIPass::Specular ? false : settings.specularOcclusion;
    }
    else if ((tracer == GITracer::HardwareRT || IsComputeTracer(tracer)) && Offers(pass, tracer))
    {
        // SetFeatureProvider also enables the feature. A pass that already traces keeps its
        // bounce source and reflection setting; the voxel cone bounce is a GI attribute.
        const rc::VoxelGISettings::BounceSource bounce      = settings.bounceSource;
        const rc::VoxelGISettings::Reflections  reflections = settings.reflections;
        const bool                              specular    = settings.specularOcclusion;
        const Provider provider = tracer == GITracer::HardwareRT ? Provider::Hardware : Provider::Voxel;
        settings.SetFeatureProvider(row.first, provider);
        settings.SetFeatureProvider(row.second, provider);
        settings.bounceSource = bounce;
        if (specular)
        {
            settings.reflections = reflections;
        }
        if (pass == GIPass::Diffuse)
        {
            settings.indirectDiffuse = true;
        }
        if (IsComputeTracer(tracer) && (pass == GIPass::Diffuse || pass == GIPass::Specular))
        {
            settings.computeTraversal = tracer == GITracer::Bricks ? Traversal::Bricks : Traversal::Triangles;
        }
    }
}

void SetGIRayTracing(rc::VoxelGISettings& settings, bool hardware)
{
    // Only providers change: passes keep their enables, Compute tracers and shadow-map choice.
    settings.rayProvider = hardware ? Provider::Hardware : Provider::Voxel;
    settings.featureProviders.fill(settings.rayProvider);
}

bool DrawLightingBackendControls(rc::VoxelGISettings& settings, bool hardwareReady, const char* reason)
{
    bool changed = false;
    if (ImGui::CollapsingHeader("GI tracing", ImGuiTreeNodeFlags_DefaultOpen))
    {
        // Pass names can repeat other settings' labels in the same window.
        ImGui::PushID("GITracing");
        GITracer tracers[static_cast<size_t>(GIPass::Count)];
        bool     compute  = true;
        bool     hardware = true;
        bool     bricks   = false;
        for (uint32_t index = 0; index < static_cast<uint32_t>(GIPass::Count); ++index)
        {
            const GITracer tracer  = GetGIPassTracer(settings, static_cast<GIPass>(index));
            tracers[index]         = tracer;
            compute               &= tracer != GITracer::HardwareRT && tracer != GITracer::Mixed;
            hardware              &= !IsComputeTracer(tracer) && tracer != GITracer::Mixed;
            bricks                |= tracer == GITracer::Bricks;
        }

        const char* choices[] = {"Compute", "HW RT"};
        if (ImGui::BeginCombo("Ray tracing", compute ? choices[0] : hardware ? choices[1] : "Per pass"))
        {
            for (int index = 0; index < 2; ++index)
            {
                if (ImGui::Selectable(choices[index], index == 0 ? compute : hardware))
                {
                    SetGIRayTracing(settings, index == 1);
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::TextWrapped(
            "Compute traces rays in compute shaders on any GPU. HW RT uses the GPU's ray-tracing hardware. Both light the scene the same way. This choice sets every pass below.");
        if (!compute)
        {
            if (hardwareReady)
            {
                ImGui::TextDisabled("HW RT is active.");
            }
            else
            {
                ImGui::TextWrapped("HW RT is unavailable (%s). Passes set to HW RT run on Compute.", reason);
            }
        }

        const ImGuiTableFlags flags = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp;
        if (ImGui::BeginTable("GIPasses", 2, flags))
        {
            ImGui::TableSetupColumn("Pass");
            ImGui::TableSetupColumn("Tracer");
            ImGui::TableHeadersRow();
            for (uint32_t index = 0; index < static_cast<uint32_t>(GIPass::Count); ++index)
            {
                const GIPass   pass    = static_cast<GIPass>(index);
                const GITracer current = tracers[index];
                ImGui::PushID(static_cast<int>(index));
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(kGIPassRows[index].name);
                ImGui::TextDisabled("%s", kGIPassRows[index].rays);
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-FLT_MIN);
                const char* preview = current == GITracer::HardwareRT && !hardwareReady
                                        ? "HW RT (unavailable)"
                                        : kTracerNames[static_cast<uint32_t>(current)];
                if (ImGui::BeginCombo("##tracer", preview))
                {
                    for (uint32_t option = 0; option <= static_cast<uint32_t>(GITracer::Off); ++option)
                    {
                        const GITracer tracer = static_cast<GITracer>(option);
                        if (Offers(pass, tracer) && ImGui::Selectable(kTracerNames[option], tracer == current))
                        {
                            SetGIPassTracer(settings, pass, tracer);
                            changed = true;
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("Bricks are fast and approximate; Software BVH is exact and slow.%s",
                            IsComputeTracer(tracers[static_cast<size_t>(GIPass::Diffuse)])
                                    && IsComputeTracer(tracers[static_cast<size_t>(GIPass::Specular)])
                                ? " Diffuse and specular GI share one Compute tracer."
                                : "");
        ImGui::TextDisabled("Probe placement, filtering, denoising and voxel light injection always run on Compute.");
        if (tracers[static_cast<size_t>(GIPass::Diffuse)] == GITracer::Off
            || tracers[static_cast<size_t>(GIPass::Specular)] == GITracer::Off)
        {
            ImGui::TextDisabled(
                "Off lights that term from the unoccluded environment, as without GI. Diffuse GI keeps tracing while off.");
        }
        ImGui::PopTextWrapPos();
        if (bricks)
        {
            const char* resolutionChoices[] = {"64", "128", "256"};
            int         resolution          = settings.brickResolution == 64 ? 0 : settings.brickResolution == 128 ? 1 : 2;
            if (ImGui::Combo("Brick cells per axis", &resolution, resolutionChoices, 3))
            {
                settings.brickResolution = 64u << resolution;
                changed                  = true;
            }
        }
        ImGui::PopID();
    }
    return changed;
}

bool DrawGIGatherControls(rc::VoxelGISettings& settings)
{
    bool        changed   = false;
    const char* gathers[] = {"Per-pixel rays", "Screen probes"};
    int         gather    = static_cast<int>(settings.gather);
    if (ImGui::Combo("Gather", &gather, gathers, 2))
    {
        settings.gather = static_cast<rc::VoxelGISettings::Gather>(gather);
        changed         = true;
    }
    if (settings.gather == rc::VoxelGISettings::Gather::Probes)
    {
        const char* spacingChoices[] = {"8 pixels", "16 pixels"};
        int         spacing          = settings.probeSpacing == 8 ? 0 : 1;
        if (ImGui::Combo("Probe spacing", &spacing, spacingChoices, 2))
        {
            settings.probeSpacing = spacing == 0 ? 8u : 16u;
            changed               = true;
        }
        ImGui::TextWrapped(
            "Each probe traces 64 rays and shares them with nearby pixels. 8 pixels keeps more surface detail. Reference accumulation and ray-count captures use per-pixel rays.");
    }
    else
    {
        ImGui::TextWrapped("Every pixel traces its own rays, at several times the cost of screen probes.");
    }
    return changed;
}
} // namespace zen::ui
