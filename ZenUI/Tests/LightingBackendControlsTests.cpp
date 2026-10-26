#include "ImGui/LightingBackendControls.h"
#include "imgui.h"
#include "imgui_internal.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <string>

namespace zen::ui
{
namespace
{
using Feature                  = rc::VoxelGISettings::Feature;
using Provider                 = rc::VoxelGISettings::RayProvider;
using Traversal                = rc::VoxelGISettings::ComputeTraversal;

const char* const kPassNames[] = {"Diffuse GI", "Specular GI", "Sky cache", "Direct shadows"};

size_t CountOf(const std::string& text, const char* needle)
{
    size_t count = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1))
    {
        ++count;
    }
    return count;
}
} // namespace

// Level 1 chooses Compute GI with maps or HW RT; level 2 chooses each pass's tracer.
TEST(GITracingTest, PassTracersMapOntoTheSettings)
{
    rc::VoxelGISettings settings;
    SetGIRayTracing(settings, false);
    for (uint32_t index = 0; index < static_cast<uint32_t>(GIPass::Count); ++index)
    {
        const GIPass pass = static_cast<GIPass>(index);
        EXPECT_EQ(GetGIPassTracer(settings, pass), pass == GIPass::Shadows ? GITracer::ShadowMaps : GITracer::SoftwareBVH)
            << index;
    }

    // Diffuse and specular GI share the Compute traversal; the sky cache has no bricks.
    SetGIPassTracer(settings, GIPass::Diffuse, GITracer::Bricks);
    EXPECT_EQ(settings.computeTraversal, Traversal::Bricks);
    EXPECT_EQ(GetGIPassTracer(settings, GIPass::Specular), GITracer::Bricks);
    EXPECT_EQ(GetGIPassTracer(settings, GIPass::Shadows), GITracer::ShadowMaps);
    SetGIPassTracer(settings, GIPass::SkyCache, GITracer::Bricks);
    EXPECT_EQ(GetGIPassTracer(settings, GIPass::SkyCache), GITracer::SoftwareBVH);
    // Direct shadows need exact geometry: bricks are not offered, and shadows keep maps.
    SetGIPassTracer(settings, GIPass::Shadows, GITracer::Bricks);
    EXPECT_EQ(GetGIPassTracer(settings, GIPass::Shadows), GITracer::ShadowMaps);

    SetGIPassTracer(settings, GIPass::Specular, GITracer::HardwareRT);
    EXPECT_EQ(settings.GetFeatureProvider(Feature::SpecularOcclusion), Provider::Hardware);
    EXPECT_EQ(settings.GetFeatureProvider(Feature::Reflections), Provider::Hardware);
    EXPECT_EQ(GetGIPassTracer(settings, GIPass::Diffuse), GITracer::Bricks);

    // Shadow maps trace no rays and keep the shadow ray provider for a later ray tracer.
    SetGIPassTracer(settings, GIPass::Shadows, GITracer::ShadowMaps);
    EXPECT_TRUE(settings.shadows);
    EXPECT_FALSE(settings.rtShadows);
    EXPECT_EQ(GetGIPassTracer(settings, GIPass::Shadows), GITracer::ShadowMaps);
    SetGIPassTracer(settings, GIPass::Diffuse, GITracer::ShadowMaps);
    EXPECT_EQ(GetGIPassTracer(settings, GIPass::Diffuse), GITracer::Bricks);
    SetGIPassTracer(settings, GIPass::Shadows, GITracer::HardwareRT);
    EXPECT_TRUE(settings.rtShadows);
    EXPECT_EQ(GetGIPassTracer(settings, GIPass::Shadows), GITracer::HardwareRT);
    settings.shadows = false;
    EXPECT_EQ(GetGIPassTracer(settings, GIPass::Shadows), GITracer::Off);
}

TEST(GITracingTest, DefaultComputeShadowsUseMapsUntilSoftwareIsSelected)
{
    rc::VoxelGIRuntimeSettings settings;
    ASSERT_TRUE(rc::ApplyVoxelGIPreset(rc::VoxelGIPreset::Low, settings));
    EXPECT_EQ(GetGIPassTracer(settings.cone, GIPass::Shadows), GITracer::ShadowMaps);
    SetGIPassTracer(settings.cone, GIPass::Shadows, GITracer::SoftwareBVH);
    EXPECT_EQ(GetGIPassTracer(settings.cone, GIPass::Shadows), GITracer::SoftwareBVH);
    EXPECT_TRUE(settings.cone.UsesRayShadows(false));
    SetGIPassTracer(settings.cone, GIPass::Shadows, GITracer::ShadowMaps);
    SetGIRayTracing(settings.cone, true);
    EXPECT_EQ(GetGIPassTracer(settings.cone, GIPass::Shadows), GITracer::ShadowMaps);
    EXPECT_FALSE(settings.cone.UsesRayShadows(true));
}

TEST(GITracingTest, GlobalComputeUsesMapsAndSoftwareShadowsRemainAnExplicitChoice)
{
    rc::VoxelGISettings settings;
    SetGIRayTracing(settings, true);
    EXPECT_EQ(GetGIPassTracer(settings, GIPass::Shadows), GITracer::HardwareRT);
    SetGIRayTracing(settings, false);
    EXPECT_EQ(settings.rayProvider, Provider::Voxel);
    EXPECT_EQ(settings.featureProviders[static_cast<size_t>(Feature::Shadows)], Provider::Auto);
    EXPECT_EQ(GetGIPassTracer(settings, GIPass::Shadows), GITracer::ShadowMaps);
    EXPECT_FALSE(settings.UsesRayShadows(false));
    EXPECT_FALSE(settings.UsesRayShadows(true));

    SetGIPassTracer(settings, GIPass::Shadows, GITracer::SoftwareBVH);
    EXPECT_EQ(GetGIPassTracer(settings, GIPass::Shadows), GITracer::SoftwareBVH);
    EXPECT_TRUE(settings.UsesRayShadows(false));
    SetGIRayTracing(settings, false);
    EXPECT_EQ(GetGIPassTracer(settings, GIPass::Shadows), GITracer::ShadowMaps);

    SetGIRayTracing(settings, true);
    EXPECT_EQ(GetGIPassTracer(settings, GIPass::Shadows), GITracer::HardwareRT);
    EXPECT_TRUE(settings.UsesRayShadows(true));
    settings.shadows = false;
    SetGIRayTracing(settings, false);
    EXPECT_EQ(GetGIPassTracer(settings, GIPass::Shadows), GITracer::Off);
    SetGIRayTracing(settings, true);
    EXPECT_EQ(GetGIPassTracer(settings, GIPass::Shadows), GITracer::Off);
    EXPECT_FALSE(settings.UsesRayShadows(true));
}

TEST(GITracingTest, DisabledFeaturesNeitherMixNorReenableUnasked)
{
    rc::VoxelGISettings settings;
    SetGIRayTracing(settings, false);
    settings.SetFeatureProvider(Feature::Bounce, Provider::Hardware);
    EXPECT_EQ(GetGIPassTracer(settings, GIPass::Diffuse), GITracer::Mixed);

    // Voxel cone bounce traces no bounce rays, so only the sky rays decide the tracer.
    settings.bounceSource = rc::VoxelGISettings::BounceSource::Cone;
    EXPECT_EQ(GetGIPassTracer(settings, GIPass::Diffuse), GITracer::SoftwareBVH);
    SetGIPassTracer(settings, GIPass::Diffuse, GITracer::Bricks);
    EXPECT_EQ(settings.bounceSource, rc::VoxelGISettings::BounceSource::Cone);

    // A tracing pass keeps its reflection setting; a pass that is off turns on.
    settings.reflections = rc::VoxelGISettings::Reflections::Off;
    SetGIPassTracer(settings, GIPass::Specular, GITracer::HardwareRT);
    EXPECT_EQ(settings.reflections, rc::VoxelGISettings::Reflections::Off);
    settings.specularOcclusion = false;
    EXPECT_EQ(GetGIPassTracer(settings, GIPass::Specular), GITracer::Off);
    SetGIPassTracer(settings, GIPass::Specular, GITracer::Bricks);
    EXPECT_TRUE(settings.specularOcclusion);
    EXPECT_EQ(settings.reflections, rc::VoxelGISettings::Reflections::On);

    // Level 1 changes only providers.
    settings.rtShadows = false;
    SetGIRayTracing(settings, true);
    EXPECT_EQ(settings.GetHardwareFeatureMask(), (1u << static_cast<uint32_t>(Feature::Count)) - 1u);
    EXPECT_FALSE(settings.rtShadows);
    EXPECT_EQ(settings.bounceSource, rc::VoxelGISettings::BounceSource::Cone);
    EXPECT_EQ(settings.computeTraversal, Traversal::Bricks);
}

// Diffuse and specular GI switch off on their own, to tell their contributions apart.
TEST(GITracingTest, DiffuseAndSpecularSwitchOffIndependently)
{
    rc::VoxelGISettings settings;
    SetGIRayTracing(settings, false);
    SetGIPassTracer(settings, GIPass::Diffuse, GITracer::Bricks);
    SetGIPassTracer(settings, GIPass::Diffuse, GITracer::Off);
    EXPECT_FALSE(settings.indirectDiffuse);
    EXPECT_TRUE(settings.specularOcclusion);
    EXPECT_EQ(GetGIPassTracer(settings, GIPass::Diffuse), GITracer::Off);
    EXPECT_EQ(GetGIPassTracer(settings, GIPass::Specular), GITracer::Bricks);

    SetGIPassTracer(settings, GIPass::Specular, GITracer::Off);
    EXPECT_FALSE(settings.specularOcclusion);
    EXPECT_EQ(GetGIPassTracer(settings, GIPass::Specular), GITracer::Off);

    // Choosing a tracer switches a pass back on.
    SetGIPassTracer(settings, GIPass::Diffuse, GITracer::Bricks);
    EXPECT_TRUE(settings.indirectDiffuse);
    EXPECT_EQ(GetGIPassTracer(settings, GIPass::Diffuse), GITracer::Bricks);
    EXPECT_EQ(GetGIPassTracer(settings, GIPass::Specular), GITracer::Off);

    // The sky cache and direct shadows offer no Off.
    SetGIPassTracer(settings, GIPass::SkyCache, GITracer::Off);
    SetGIPassTracer(settings, GIPass::Shadows, GITracer::Off);
    EXPECT_EQ(GetGIPassTracer(settings, GIPass::SkyCache), GITracer::SoftwareBVH);
    EXPECT_EQ(GetGIPassTracer(settings, GIPass::Shadows), GITracer::ShadowMaps);
}

// The GI tracing panel shown by the editor and the runtime UI. Drawn headlessly; ImGui logs the drawn text.
class LightingBackendControlsTest : public testing::Test
{
protected:
    void SetUp() override
    {
        context               = ImGui::CreateContext();
        ImGuiIO& io           = ImGui::GetIO();
        io.IniFilename        = nullptr;
        io.DisplaySize        = ImVec2(900, 1400);
        io.DeltaTime          = 1.0f / 60.0f;
        unsigned char* pixels = nullptr;
        int            width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    }

    void TearDown() override
    {
        ImGui::DestroyContext(context);
    }

    std::string Draw(rc::VoxelGISettings& settings, bool hardwareReady, const char* reason = "ray queries unsupported")
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(900, 1400));
        ImGui::Begin("GI tracing test");
        ImGui::LogToBuffer();
        EXPECT_FALSE(DrawLightingBackendControls(settings, hardwareReady, reason));
        EXPECT_FALSE(DrawGIGatherControls(settings));
        const std::string text = ImGui::GetCurrentContext()->LogBuffer.c_str();
        ImGui::LogFinish();
        ImGui::End();
        ImGui::Render();
        std::printf("%s\n", text.c_str());
        return text;
    }

    ImGuiContext* context{nullptr};
};

TEST_F(LightingBackendControlsTest, ComputeListsEveryPassWithItsTracer)
{
    rc::VoxelGISettings settings;
    SetGIRayTracing(settings, false);
    settings.computeTraversal = Traversal::Bricks;
    settings.gather           = rc::VoxelGISettings::Gather::Probes;
    const std::string text    = Draw(settings, false);
    for (const char* name : kPassNames)
    {
        const std::string row = std::string("| ") + name;
        EXPECT_EQ(CountOf(text, row.c_str()), 1u) << name;
    }
    // Tracer cells log their combo preview as "{ text } |".
    EXPECT_EQ(CountOf(text, "{ Compute: Bricks } |"), 2u);
    EXPECT_EQ(CountOf(text, "{ Compute: Software BVH } |"), 1u);
    EXPECT_EQ(CountOf(text, "{ Shadow maps } |"), 1u);
    EXPECT_EQ(CountOf(text, "{ Software BVH } |"), 0u);
    EXPECT_NE(text.find("share one Compute tracer"), std::string::npos);
    EXPECT_NE(text.find("Brick cells per axis"), std::string::npos);
    EXPECT_NE(text.find("Probe spacing"), std::string::npos);
    EXPECT_EQ(text.find("HW RT is"), std::string::npos);
}

TEST_F(LightingBackendControlsTest, SoftwareShadowsAreLabeledAfterAnExplicitPassSelection)
{
    rc::VoxelGISettings settings;
    SetGIRayTracing(settings, false);
    SetGIPassTracer(settings, GIPass::Shadows, GITracer::SoftwareBVH);
    const std::string text = Draw(settings, false);
    EXPECT_EQ(CountOf(text, "{ Software BVH } |"), 1u);
    EXPECT_EQ(CountOf(text, "{ Shadow maps } |"), 0u);
}

// Compute GI preparation is reported over the scene view, not in the panel.
TEST_F(LightingBackendControlsTest, ComputePreparationIsLeftToTheSceneView)
{
    rc::VoxelGISettings settings;
    SetGIRayTracing(settings, false);
    std::string text = Draw(settings, true, "software_bvh_preparing");
    EXPECT_EQ(text.find("Preparing Compute GI"), std::string::npos);
    EXPECT_EQ(text.find("software_bvh_preparing"), std::string::npos);
    EXPECT_EQ(GetGIPassTracer(settings, GIPass::Shadows), GITracer::ShadowMaps);
    // A pass still on HW RT gives no false unavailability reason while Compute prepares.
    SetGIPassTracer(settings, GIPass::Specular, GITracer::HardwareRT);
    text = Draw(settings, false, "software_bvh_preparing");
    EXPECT_EQ(text.find("HW RT is unavailable"), std::string::npos);
    EXPECT_EQ(text.find("software_bvh_preparing"), std::string::npos);
}

TEST_F(LightingBackendControlsTest, PreparationStatusDrawsAtTheTopCenterClearOfCornerWidgets)
{
    EXPECT_EQ(GetGIPreparationStatus(false, true), nullptr);
    EXPECT_STREQ(GetGIPreparationStatus(true, true), "Preparing Compute GI. HW RT stays active until it is ready.");
    EXPECT_STREQ(GetGIPreparationStatus(true, false), "Preparing Compute GI. The view shows PBR until it is ready.");

    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(900, 1400));
    ImGui::Begin("Scene view test");
    ImDrawList&     draw = *ImGui::GetWindowDrawList();
    const ImVec2    viewMin(100, 50);
    const ImVec2    viewMax(800, 450);
    constexpr float inset = 120.0f;
    int             first = draw.VtxBuffer.Size;
    EXPECT_TRUE(DrawGIPreparationStatus(draw, true, false, viewMin, viewMax, inset));
    ASSERT_GT(draw.VtxBuffer.Size, first);
    ImVec2 low(FLT_MAX, FLT_MAX);
    ImVec2 high(-FLT_MAX, -FLT_MAX);
    for (int vertex = first; vertex < draw.VtxBuffer.Size; ++vertex)
    {
        low  = ImVec2(std::min(low.x, draw.VtxBuffer[vertex].pos.x), std::min(low.y, draw.VtxBuffer[vertex].pos.y));
        high = ImVec2(std::max(high.x, draw.VtxBuffer[vertex].pos.x), std::max(high.y, draw.VtxBuffer[vertex].pos.y));
    }
    EXPECT_GE(low.x, viewMin.x + inset);
    EXPECT_LE(high.x, viewMax.x - inset);
    EXPECT_NEAR((low.x + high.x) * 0.5f, (viewMin.x + viewMax.x) * 0.5f, 1.0f);
    EXPECT_GT(low.y, viewMin.y);
    EXPECT_LT(high.y, (viewMin.y + viewMax.y) * 0.5f);

    first = draw.VtxBuffer.Size;
    EXPECT_FALSE(DrawGIPreparationStatus(draw, false, false, viewMin, viewMax, inset));
    // A view too narrow beside its corner widgets keeps its scene clear.
    EXPECT_FALSE(DrawGIPreparationStatus(draw, true, true, viewMin, ImVec2(viewMin.x + 2 * inset + 40, viewMax.y), inset));
    EXPECT_EQ(draw.VtxBuffer.Size, first);
    ImGui::End();
    ImGui::Render();
}

TEST_F(LightingBackendControlsTest, HardwareReportsWhetherItIsActive)
{
    rc::VoxelGISettings settings;
    SetGIRayTracing(settings, true);
    std::string text = Draw(settings, true);
    EXPECT_NE(text.find("HW RT is active"), std::string::npos);
    EXPECT_EQ(CountOf(text, "{ HW RT } |"), 4u);
    EXPECT_EQ(text.find("Brick cells per axis"), std::string::npos);
    EXPECT_EQ(text.find("Probe spacing"), std::string::npos);
    text = Draw(settings, false);
    EXPECT_NE(text.find("ray queries unsupported"), std::string::npos);
    EXPECT_EQ(CountOf(text, "{ HW RT (unavailable) } |"), 3u);
    EXPECT_EQ(CountOf(text, "{ Shadow maps (HW RT unavailable) } |"), 1u);
    EXPECT_TRUE(settings.rtShadows);
    EXPECT_EQ(settings.GetFeatureProvider(Feature::Shadows), Provider::Hardware);
}

TEST_F(LightingBackendControlsTest, OffPassesSayWhatReplacesThem)
{
    rc::VoxelGISettings settings;
    SetGIRayTracing(settings, false);
    settings.computeTraversal = Traversal::Bricks;
    std::string text          = Draw(settings, false);
    EXPECT_EQ(text.find("unoccluded environment"), std::string::npos);
    SetGIPassTracer(settings, GIPass::Diffuse, GITracer::Off);
    text = Draw(settings, false);
    EXPECT_EQ(CountOf(text, "{ Off } |"), 1u);
    EXPECT_EQ(CountOf(text, "{ Compute: Bricks } |"), 1u);
    EXPECT_NE(text.find("unoccluded environment"), std::string::npos);
    EXPECT_EQ(text.find("share one Compute tracer"), std::string::npos);
}

TEST_F(LightingBackendControlsTest, ShowsPerPassWhenPassesDiffer)
{
    rc::VoxelGISettings settings;
    SetGIRayTracing(settings, false);
    SetGIPassTracer(settings, GIPass::Diffuse, GITracer::HardwareRT);
    SetGIPassTracer(settings, GIPass::Shadows, GITracer::ShadowMaps);
    const std::string text = Draw(settings, true);
    EXPECT_NE(text.find("Per pass"), std::string::npos);
    EXPECT_EQ(CountOf(text, "{ HW RT } |"), 1u);
    EXPECT_EQ(CountOf(text, "{ Shadow maps } |"), 1u);
    EXPECT_EQ(text.find("share one Compute tracer"), std::string::npos);
}
} // namespace zen::ui
