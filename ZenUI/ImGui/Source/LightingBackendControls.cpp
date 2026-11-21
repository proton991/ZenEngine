#include "ImGui/LightingBackendControls.h"
#include "imgui.h"
#include <algorithm>
#include <string>

namespace zen::ui
{
namespace
{
using rc::GIDenoiser;
using rc::GIMethod;
using rc::GIShadows;
using rc::GITracing;

const char* const kMethodLabels[]   = {"Per-pixel", "Per-pixel ReSTIR", "Per-pixel + world cache",
                                       "Per-pixel ReSTIR + world cache", "Screen probes + world cache"};
const char* const kTracingLabels[]  = {"Automatic", "Compute (distance field)", "Hardware ray queries", "Hardware RT pipeline"};
const char* const kDenoiserLabels[] = {"Spatiotemporal", "Temporal only", "Spatial only", "Off"};
const char* const kShadowLabels[]   = {"Shadow maps", "Ray traced", "Ray traced with reuse"};
const char* const kTierHeaders[]    = {"Compute", "Ray queries", "RT pipeline"};
const GITracing   kTiers[]          = {GITracing::Compute, GITracing::RayQueries, GITracing::RayTracingPipeline};

struct GIChanges
{
    bool configuration{false};
    bool content{false};
};

bool IsPixelMethod(GIMethod method)
{
    return method != GIMethod::ScreenProbes;
}

bool UsesReSTIR(GIMethod method)
{
    return method == GIMethod::PixelReSTIR || method == GIMethod::PixelReSTIRCache;
}

bool SupportsTier(GITracing tracing, const GIControlsContext& context)
{
    return tracing == GITracing::Automatic || tracing == GITracing::Compute
        || (tracing == GITracing::RayQueries && context.rayQueries)
        || (tracing == GITracing::RayTracingPipeline && context.rayQueries && context.rayTracingPipeline);
}

// The tier the renderer runs: the effective settings carry the resolved provider.
GITracing GetActiveTier(const rc::VoxelGIRuntimeSettings& effective)
{
    const GITracing tracing = rc::GetGITracing(effective.cone);
    return tracing == GITracing::Automatic ? GITracing::Compute : tracing;
}

// Keep the active tier when it supports the method, then try the available hardware tiers.
GITracing GetTierForMethod(GIMethod method, GITracing active, const GIControlsContext& context)
{
    GITracing tier = active;
    if (rc::GetGIMethodUnavailableReason(method, active) != nullptr)
    {
        tier = GITracing::Compute;
        if (SupportsTier(GITracing::RayTracingPipeline, context))
        {
            tier = GITracing::RayTracingPipeline;
        }
        else if (SupportsTier(GITracing::RayQueries, context)
                 && rc::GetGIMethodUnavailableReason(method, GITracing::RayQueries) == nullptr)
        {
            tier = GITracing::RayQueries;
        }
    }
    return tier;
}

void Hint(const char* text)
{
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", text);
    ImGui::PopTextWrapPos();
}

std::string GetSummary(const rc::VoxelGIRuntimeSettings& settings, GITracing active)
{
    const GIMethod method  = rc::GetGIMethod(settings.cone);
    std::string    summary = std::string(GetGIMethodLabel(method)) + " / " + GetGITracingLabel(active);
    if (IsPixelMethod(method))
    {
        const uint32_t divisor  = settings.cone.pixelResolutionDivisor;
        summary                += divisor == 0 ? " / automatic resolution"
                                : divisor == 1 ? " / full resolution"
                                : divisor == 2 ? " / half resolution"
                                               : " / quarter resolution";
    }
    summary += std::string(" / ") + GetGIDenoiserLabel(rc::GetGIDenoiser(settings.cone)) + " denoiser";
    return summary;
}

void DrawSelector(rc::VoxelGIRuntimeSettings&       settings,
                  const rc::VoxelGIRuntimeSettings& shown,
                  GITracing                         active,
                  bool&                             changed)
{
    ImGui::SeparatorText("Configuration");
    const bool optimal = rc::IsOptimalVoxelGIConfiguration(settings);
    if (ImGui::RadioButton("Optimal", optimal) && !optimal)
    {
        rc::ApplyVoxelGIPreset(rc::VoxelGIPreset::Optimal, settings);
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Custom", !optimal) && optimal)
    {
        settings         = shown;
        settings.optimal = false;
        changed          = true;
    }
    if (optimal)
    {
        Hint(("Chosen for this GPU at startup: " + GetSummary(shown, active) + ".").c_str());
    }
}

void DrawMethod(rc::VoxelGIRuntimeSettings& settings, GITracing active, const GIControlsContext& context, GIChanges& changes)
{
    rc::VoxelGISettings& cone   = settings.cone;
    const GIMethod       method = rc::GetGIMethod(cone);
    ImGui::SeparatorText("Method");
    if (ImGui::BeginCombo("Method", GetGIMethodLabel(method)))
    {
        for (uint32_t index = 0; index < static_cast<uint32_t>(GIMethod::Count); ++index)
        {
            const GIMethod    candidate = static_cast<GIMethod>(index);
            const char*       reason    = rc::GetGIMethodUnavailableReason(candidate, active);
            const std::string label     = reason == nullptr ? std::string(GetGIMethodLabel(candidate))
                                                            : std::string(GetGIMethodLabel(candidate)) + " (switches tracing)";
            if (ImGui::Selectable(label.c_str(), candidate == method))
            {
                rc::SetGIMethod(cone, candidate);
                if (reason != nullptr)
                {
                    rc::SetGITracing(cone, GetTierForMethod(candidate, active, context));
                }
                changes.configuration = true;
            }
            if (reason != nullptr && ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("%s", reason);
            }
        }
        ImGui::EndCombo();
    }

    const GITracing tracing = rc::GetGITracing(cone);
    if (ImGui::BeginCombo("Tracing", GetGITracingLabel(tracing)))
    {
        for (uint32_t index = 0; index < static_cast<uint32_t>(GITracing::Count); ++index)
        {
            const GITracing candidate = static_cast<GITracing>(index);
            const bool      supported = SupportsTier(candidate, context);
            if (ImGui::Selectable(GetGITracingLabel(candidate), candidate == tracing,
                                  supported ? ImGuiSelectableFlags_None : ImGuiSelectableFlags_Disabled))
            {
                rc::SetGITracing(cone, candidate);
                changes.configuration = true;
            }
            if (!supported && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            {
                ImGui::SetTooltip("Not supported by this GPU");
            }
        }
        ImGui::EndCombo();
    }
    if (active != GITracing::Compute)
    {
        if (context.hardwareReady)
        {
            ImGui::TextDisabled("Hardware RT is active.");
        }
        else
        {
            ImGui::TextWrapped("Hardware RT is not active: %s", context.reason);
        }
    }
    const char* unavailable = rc::GetGIMethodUnavailableReason(method, active);
    if (unavailable != nullptr)
    {
        ImGui::TextWrapped("%s; the per-pixel trace runs instead.", unavailable);
    }

    if (IsPixelMethod(method))
    {
        const char* resolutions[] = {"Automatic (at least 540 lines)", "Full", "Half per axis", "Quarter per axis"};
        int         resolution    = cone.pixelResolutionDivisor == 0 ? 0
                                  : cone.pixelResolutionDivisor == 4 ? 3
                                                                     : static_cast<int>(cone.pixelResolutionDivisor);
        if (ImGui::Combo("GI resolution", &resolution, resolutions, 4))
        {
            cone.pixelResolutionDivisor = resolution == 0 ? 0u : 1u << (resolution - 1);
            changes.configuration       = true;
        }
    }
    else
    {
        ImGui::TextDisabled("Full-resolution probe gather");
    }
    if (UsesReSTIR(method) && active != GITracing::Compute)
    {
        changes.configuration |= ImGui::Checkbox("Spatial reuse", &cone.restirSpatial);
        if (cone.restirSpatial)
        {
            int neighbors = static_cast<int>(cone.restirSpatialSamples);
            if (ImGui::SliderInt("Reuse neighbors", &neighbors, 1, 4))
            {
                cone.restirSpatialSamples = static_cast<uint32_t>(neighbors);
                changes.configuration     = true;
            }
        }
    }
    if (active == GITracing::Compute)
    {
        const char* cells[] = {"32", "64"};
        int         field   = cone.fieldResolution == 32 ? 0 : 1;
        if (ImGui::Combo("Distance field cells per axis", &field, cells, 2))
        {
            cone.fieldResolution  = 32u << field;
            changes.configuration = true;
        }
        const char* grids[] = {"64", "128", "256"};
        int         grid    = settings.resolution == 64 ? 0 : settings.resolution == 128 ? 1 : 2;
        if (ImGui::Combo("Material voxels per axis", &grid, grids, 3))
        {
            settings.resolution   = 64u << grid;
            changes.configuration = true;
        }
    }
}

// Every method against every tracing tier, with what this GPU runs and the optimal choice.
void DrawCombinations(rc::VoxelGISettings& cone, GITracing active, const GIControlsContext& context, GIChanges& changes)
{
    if (ImGui::TreeNodeEx("All combinations", ImGuiTreeNodeFlags_DefaultOpen))
    {
        const GITracing optimalTier   = rc::GetOptimalGITracing(context.rayQueries, context.rayTracingPipeline);
        const GIMethod  optimalMethod = rc::GetOptimalGIMethod(optimalTier);
        const GIMethod  method        = rc::GetGIMethod(cone);
        if (ImGui::BeginTable("GI combinations", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn("Method", ImGuiTableColumnFlags_WidthStretch, 2.8f);
            for (const char* header : kTierHeaders)
            {
                ImGui::TableSetupColumn(header, ImGuiTableColumnFlags_WidthStretch, 1.0f);
            }
            ImGui::TableHeadersRow();
            for (uint32_t row = 0; row < static_cast<uint32_t>(GIMethod::Count); ++row)
            {
                const GIMethod candidate = static_cast<GIMethod>(row);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(GetGIMethodLabel(candidate));
                for (GITracing tier : kTiers)
                {
                    ImGui::TableNextColumn();
                    const char*       reason   = rc::GetGIMethodUnavailableReason(candidate, tier);
                    const bool        runs     = reason == nullptr && SupportsTier(tier, context);
                    const bool        optimal  = candidate == optimalMethod && tier == optimalTier;
                    const std::string label    = std::string(!runs     ? "-"
                                                             : optimal ? "Optimal"
                                                                       : "Available")
                                               + "##" + std::to_string(row) + "_" + std::to_string(static_cast<uint32_t>(tier));
                    const bool        selected = candidate == method && tier == active;
                    if (ImGui::Selectable(label.c_str(), selected,
                                          runs ? ImGuiSelectableFlags_None : ImGuiSelectableFlags_Disabled))
                    {
                        rc::SetGIMethod(cone, candidate);
                        rc::SetGITracing(cone, tier);
                        changes.configuration = true;
                    }
                    if (!runs && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    {
                        ImGui::SetTooltip("%s", reason != nullptr ? reason : "Not supported by this GPU");
                    }
                }
            }
            ImGui::EndTable();
        }
        Hint("Select a cell to use that method and tracing tier.");
        ImGui::TreePop();
    }
}

void DrawDenoiser(rc::VoxelGISettings& cone, GIChanges& changes)
{
    const GIMethod method   = rc::GetGIMethod(cone);
    int            denoiser = static_cast<int>(rc::GetGIDenoiser(cone));
    ImGui::SeparatorText("Denoiser");
    if (ImGui::Combo("Denoiser", &denoiser, kDenoiserLabels, static_cast<int>(GIDenoiser::Count)))
    {
        rc::SetGIDenoiser(cone, static_cast<GIDenoiser>(denoiser));
        changes.configuration = true;
    }
    Hint(IsPixelMethod(method)
             ? "Pixel denoiser: temporal accumulation, history repair, three a-trous passes and an output stabilizer."
             : "Probe filter: world-anchored temporal accumulation and two spatial passes across probes.");
    if (static_cast<GIDenoiser>(denoiser) != GIDenoiser::Spatiotemporal)
    {
        Hint("Noisier than spatiotemporal; for comparisons.");
    }
    if (IsPixelMethod(method) && cone.temporal && ImGui::TreeNode("Denoiser details"))
    {
        changes.configuration |= ImGui::Checkbox("Bicubic history", &cone.bicubicHistory);
        changes.configuration |= ImGui::Checkbox("Data-driven anti-lag", &cone.dataDrivenAntiLag);
        changes.configuration |= ImGui::Checkbox("Anti-firefly", &cone.antiFirefly);
        changes.configuration |= ImGui::Checkbox("Extra samples for young receivers", &cone.youngReceiverSamples);
        if (cone.pixelResolutionDivisor != 1)
        {
            changes.configuration |= ImGui::Checkbox("Footprint receivers", &cone.footprintReceivers);
        }
        if (method == GIMethod::PixelCache || method == GIMethod::PixelReSTIRCache)
        {
            changes.configuration |= ImGui::Checkbox("Seed new pixels from the world cache", &cone.worldCacheSeed);
            ImGui::SetItemTooltip("Experimental: disoccluded pixels start from their cache cell's indirect light.");
        }
        ImGui::TreePop();
    }
}

void DrawIndirect(rc::VoxelGISettings& cone, GIChanges& changes)
{
    ImGui::SeparatorText("Indirect lighting");
    changes.content |= ImGui::Checkbox("Diffuse GI", &cone.indirectDiffuse);
    if (cone.indirectDiffuse)
    {
        changes.content |= ImGui::SliderFloat("Indirect intensity", &cone.indirectIntensity, 0.0f, 10.0f);
        if (ImGui::TreeNode("Diffuse light sources"))
        {
            changes.content |= ImGui::Checkbox("Analytic lights", &cone.analyticLighting);
            changes.content |= ImGui::Checkbox("Environment", &cone.environmentLighting);
            changes.content |= ImGui::Checkbox("Emissive surfaces", &cone.emissiveLighting);
            ImGui::TreePop();
        }
    }
    changes.content |= ImGui::Checkbox("Specular GI", &cone.specularOcclusion);
    if (cone.specularOcclusion)
    {
        bool reflections = cone.reflections != rc::VoxelGISettings::Reflections::Off;
        if (ImGui::Checkbox("Trace reflections", &reflections))
        {
            cone.reflections = reflections ? rc::VoxelGISettings::Reflections::On : rc::VoxelGISettings::Reflections::Off;
            changes.content  = true;
        }
    }
    if (!cone.indirectDiffuse || !cone.specularOcclusion)
    {
        ImGui::TextWrapped("Disabled terms use the unoccluded environment.");
    }
}

void DrawShadows(rc::VoxelGISettings& cone, GITracing active, GIChanges& changes)
{
    ImGui::SeparatorText("Direct shadows");
    changes.content |= ImGui::Checkbox("Cast shadows", &cone.shadows);
    if (cone.shadows)
    {
        const GIShadows shadows = rc::GetGIShadows(cone);
        if (ImGui::BeginCombo("Shadow method", GetGIShadowsLabel(shadows)))
        {
            for (uint32_t index = 0; index < static_cast<uint32_t>(GIShadows::Count); ++index)
            {
                const GIShadows candidate = static_cast<GIShadows>(index);
                const char*     reason    = candidate == GIShadows::ShadowMaps ? nullptr
                                          : active == GITracing::Compute       ? "Compute uses shadow maps"
                                          : candidate == GIShadows::RayTracedReuse && active == GITracing::RayQueries
                                              ? "Reuse needs the RT pipeline; every receiver is traced"
                                              : nullptr;
                if (ImGui::Selectable(GetGIShadowsLabel(candidate), candidate == shadows))
                {
                    rc::SetGIShadows(cone, candidate);
                    changes.configuration = true;
                }
                if (reason != nullptr && ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("%s", reason);
                }
            }
            ImGui::EndCombo();
        }
        if (shadows != GIShadows::ShadowMaps && active == GITracing::Compute)
        {
            ImGui::TextDisabled("Compute uses shadow maps.");
        }
    }
}
} // namespace

bool DrawGIControls(rc::VoxelGIRuntimeSettings& settings, const GIControlsContext& context)
{
    const rc::VoxelGIRuntimeSettings& effective = context.effective != nullptr ? *context.effective : settings;
    const GITracing                   active    = GetActiveTier(effective);
    // The optimal configuration shows what it resolved to, with the latest content edits.
    rc::VoxelGIRuntimeSettings shown = settings;
    if (rc::IsOptimalVoxelGIConfiguration(settings))
    {
        shown = effective;
        rc::CopyVoxelGIContent(settings, shown);
        shown.optimal = true;
    }
    bool changed = false;
    DrawSelector(settings, shown, active, changed);
    if (!changed)
    {
        GIChanges                  changes;
        rc::VoxelGIRuntimeSettings working = shown;
        DrawMethod(working, active, context, changes);
        DrawCombinations(working.cone, active, context, changes);
        DrawDenoiser(working.cone, changes);
        DrawIndirect(working.cone, changes);
        DrawShadows(working.cone, active, changes);
        if (changes.configuration)
        {
            settings         = working;
            settings.optimal = false;
        }
        else if (changes.content)
        {
            rc::CopyVoxelGIContent(working, settings);
        }
        changed = changes.configuration || changes.content;
    }
    return changed;
}

const char* GetGIMethodLabel(GIMethod method)
{
    return kMethodLabels[std::min(static_cast<uint32_t>(method), static_cast<uint32_t>(GIMethod::Count) - 1)];
}

const char* GetGITracingLabel(GITracing tracing)
{
    return kTracingLabels[std::min(static_cast<uint32_t>(tracing), static_cast<uint32_t>(GITracing::Count) - 1)];
}

const char* GetGIDenoiserLabel(GIDenoiser denoiser)
{
    return kDenoiserLabels[std::min(static_cast<uint32_t>(denoiser), static_cast<uint32_t>(GIDenoiser::Count) - 1)];
}

const char* GetGIShadowsLabel(GIShadows shadows)
{
    return kShadowLabels[std::min(static_cast<uint32_t>(shadows), static_cast<uint32_t>(GIShadows::Count) - 1)];
}

const char* GetGILightingStatus(GILightingState state)
{
    const char* text = nullptr;
    switch (state)
    {
        case GILightingState::Preparing: text = "Preparing GI..."; break;
        case GILightingState::Refining: text = "GI active. Refining lighting..."; break;
        case GILightingState::Ready: text = "GI ready"; break;
        case GILightingState::Unavailable: text = "GI unavailable. Check Rendering for details."; break;
        case GILightingState::Inactive: break;
    }
    return text;
}

} // namespace zen::ui
