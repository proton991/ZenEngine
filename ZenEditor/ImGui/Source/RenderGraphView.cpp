#include "Editor/ImGui/RenderGraphView.h"
#include "Editor/ImGui/EditorTheme.h"
#include "RenderGraphDrawing.h"
#include <filesystem>
#include <spdlog/fmt/fmt.h>

namespace zen::editor
{
using namespace graph_drawing;
namespace
{
// Filled chips, as in the mockup: tinted for state and queue, neutral otherwise.
void StatusChip(ImFont* font, const char* label, ImU32 color, float tint, const char* tooltip = nullptr)
{
    const ImVec2 position = ImGui::GetCursorScreenPos();
    const float  height   = TextSize(font, label).y + 4;
    const ImVec2 size =
        Chip(*ImGui::GetWindowDrawList(), font,
             ImVec2(position.x, std::round(position.y + (ImGui::GetFrameHeight() - height) * 0.5f)), label, color, tint);
    ImGui::Dummy(ImVec2(size.x, ImGui::GetFrameHeight()));
    if (tooltip != nullptr && ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("%s", tooltip);
    }
}

const char* FormatName(DataFormat format)
{
    const char* name = "Undefined";
    switch (format)
    {
        case DataFormat::eR8UNORM: name = "R8"; break;
        case DataFormat::eR8UInt: name = "R8U"; break;
        case DataFormat::eR8G8B8SRGB: name = "RGB8 sRGB"; break;
        case DataFormat::eR8G8B8UNORM: name = "RGB8"; break;
        case DataFormat::eR8G8B8A8UInt: name = "RGBA8U"; break;
        case DataFormat::eR8G8B8A8SRGB: name = "RGBA8 sRGB"; break;
        case DataFormat::eR8G8B8A8UNORM: name = "RGBA8"; break;
        case DataFormat::eB8G8R8A8UNORM: name = "BGRA8"; break;
        case DataFormat::eB8G8R8A8SRGB: name = "BGRA8 sRGB"; break;
        case DataFormat::eR16UInt: name = "R16U"; break;
        case DataFormat::eR16SInt: name = "R16I"; break;
        case DataFormat::eR16SFloat: name = "R16F"; break;
        case DataFormat::eR16G16UNORM: name = "RG16"; break;
        case DataFormat::eR16G16UInt: name = "RG16U"; break;
        case DataFormat::eR16G16SInt: name = "RG16I"; break;
        case DataFormat::eR16G16SFloat: name = "RG16F"; break;
        case DataFormat::eR16G16B16UInt: name = "RGB16U"; break;
        case DataFormat::eR16G16B16SInt: name = "RGB16I"; break;
        case DataFormat::eR16G16B16SFloat: name = "RGB16F"; break;
        case DataFormat::eR16G16B16A16UInt: name = "RGBA16U"; break;
        case DataFormat::eR16G16B16A16SInt: name = "RGBA16I"; break;
        case DataFormat::eR16G16B16A16SFloat: name = "RGBA16F"; break;
        case DataFormat::eR32UInt: name = "R32U"; break;
        case DataFormat::eR32SInt: name = "R32I"; break;
        case DataFormat::eR32SFloat: name = "R32F"; break;
        case DataFormat::eR32G32UInt: name = "RG32U"; break;
        case DataFormat::eR32G32SInt: name = "RG32I"; break;
        case DataFormat::eR32G32SFloat: name = "RG32F"; break;
        case DataFormat::eR32G32B32UInt: name = "RGB32U"; break;
        case DataFormat::eR32G32B32SInt: name = "RGB32I"; break;
        case DataFormat::eR32G32B32SFloat: name = "RGB32F"; break;
        case DataFormat::eR32G32B32A32UInt: name = "RGBA32U"; break;
        case DataFormat::eR32G32B32A32SInt: name = "RGBA32I"; break;
        case DataFormat::eR32G32B32A32SFloat: name = "RGBA32F"; break;
        case DataFormat::eR64UInt: name = "R64U"; break;
        case DataFormat::eR64SInt: name = "R64I"; break;
        case DataFormat::eR64SFloat: name = "R64F"; break;
        case DataFormat::eR64G64UInt: name = "RG64U"; break;
        case DataFormat::eR64G64SInt: name = "RG64I"; break;
        case DataFormat::eR64G64SFloat: name = "RG64F"; break;
        case DataFormat::eR64G64B64UInt: name = "RGB64U"; break;
        case DataFormat::eR64G64B64SInt: name = "RGB64I"; break;
        case DataFormat::eR64G64B64SFloat: name = "RGB64F"; break;
        case DataFormat::eR64G64B64A64UInt: name = "RGBA64U"; break;
        case DataFormat::eR64G64B64A64SInt: name = "RGBA64I"; break;
        case DataFormat::eR64G64B64A64SFloat: name = "RGBA64F"; break;
        case DataFormat::eD16UNORM: name = "D16"; break;
        case DataFormat::eD32SFloat: name = "D32F"; break;
        case DataFormat::eS8UInt: name = "S8U"; break;
        case DataFormat::eD16UNORMS8UInt: name = "D16S8U"; break;
        case DataFormat::eD24UNORMS8UInt: name = "D24S8U"; break;
        case DataFormat::eD32SFloatS8UInt: name = "D32FS8U"; break;
        case DataFormat::eBC4UNORMBlock: name = "BC4"; break;
        case DataFormat::eBC4SNORMBlock: name = "BC4 SNORM"; break;
        case DataFormat::eBC5UNORMBlock: name = "BC5"; break;
        case DataFormat::eBC5SNORMBlock: name = "BC5 SNORM"; break;
        case DataFormat::eBC6HUFloatBlock: name = "BC6HUF"; break;
        case DataFormat::eBC6HSFloatBlock: name = "BC6HF"; break;
        case DataFormat::eBC7UNORMBlock: name = "BC7"; break;
        case DataFormat::eBC7SRGBBlock: name = "BC7SRGB"; break;
        default: break;
    }
    return name;
}

void ResourceTooltip(const rc::RDGDebugResource& resource, const rc::RDGAccess& access)
{
    ImGui::BeginTooltip();
    ImGui::Text("%s (#%d)", resource.name.c_str(), resource.id);
    ImGui::TextUnformatted(resource.lifecycle == rc::RDGResourceLifeCycle::eImported     ? "Imported"
                           : resource.lifecycle == rc::RDGResourceLifeCycle::ePersistent ? "Persistent"
                                                                                         : "Transient");
    if (resource.type == rc::RDGResourceType::eTexture)
    {
        ImGui::Text("%u x %u x %u, %s", resource.texture.width, resource.texture.height, resource.texture.depth,
                    FormatName(resource.texture.format));
        ImGui::Text("Mips %u + %u; layers %u + %u", access.textureSubResourceRange.baseMipLevel,
                    access.textureSubResourceRange.levelCount, access.textureSubResourceRange.baseArrayLayer,
                    access.textureSubResourceRange.layerCount);
    }
    else
    {
        ImGui::Text("%llu bytes (whole buffer)", static_cast<unsigned long long>(resource.bufferSize));
    }
    if (access.finalStateOnly)
    {
        ImGui::TextUnformatted("Final state only");
    }
    ImGui::EndTooltip();
}
} // namespace

void RenderGraphNavigation::Start(ImVec2 destination, float scale)
{
    startPan   = pan;
    startZoom  = zoom;
    targetPan  = destination;
    targetZoom = scale;
    elapsed    = 0;
}

void RenderGraphNavigation::Advance(float seconds)
{
    if (IsActive())
    {
        elapsed           = std::min(elapsed + std::max(seconds, 0.0f), RenderGraphStyle::NavigationSeconds);
        const float t     = elapsed / RenderGraphStyle::NavigationSeconds;
        const float eased = 1 - (1 - t) * (1 - t) * (1 - t);
        pan  = ImVec2(startPan.x + (targetPan.x - startPan.x) * eased, startPan.y + (targetPan.y - startPan.y) * eased);
        zoom = startZoom + (targetZoom - startZoom) * eased;
    }
}

void RenderGraphNavigation::Stop()
{
    elapsed = RenderGraphStyle::NavigationSeconds;
}

void RenderGraphNavigation::Jump(ImVec2 destination, float scale)
{
    pan = startPan = targetPan = destination;
    zoom = startZoom = targetZoom = scale;
    elapsed                       = RenderGraphStyle::NavigationSeconds;
}

void RenderGraphNavigation::Offset(ImVec2 delta)
{
    pan.x       += delta.x;
    pan.y       += delta.y;
    startPan.x  += delta.x;
    startPan.y  += delta.y;
    targetPan.x += delta.x;
    targetPan.y += delta.y;
}

bool RenderGraphNavigation::IsActive() const
{
    return elapsed < RenderGraphStyle::NavigationSeconds;
}

void RenderGraphView::InitFonts(float scale)
{
    std::error_code error;
    if (std::filesystem::is_regular_file(ZEN_EDITOR_FONT_PATH, error))
    {
        ImFontConfig         font;
        static const ImWchar ranges[] = {0x20, 0xff, 0x2026, 0x2026, 0x2190, 0x2192, 0};
        font.GlyphRanges              = ranges;
        font.RasterizerDensity        = ImGui::GetIO().DisplayFramebufferScale.x;
        m_titleFont                   = ImGui::GetIO().Fonts->AddFontFromFileTTF(ZEN_EDITOR_FONT_PATH, 13 * scale, &font);
        m_detailFont                  = ImGui::GetIO().Fonts->AddFontFromFileTTF(ZEN_EDITOR_FONT_PATH, 11 * scale, &font);
    }
}

bool RenderGraphView::IsAnimating() const
{
    return m_navigation.IsActive();
}

void RenderGraphView::RequestSelection(const std::string& name)
{
    m_requestedSelection = name;
}

bool RenderGraphView::DependencyRow(const std::string& name, const char* reason, ImU32 reasonColor, bool input, bool enabled)
{
    // One clickable row per connection: direction arrow, the other pass, the reason,
    // and a chevron. The arrows are drawn because the editor font has no arrow glyphs.
    ImDrawList&  draw   = *ImGui::GetWindowDrawList();
    const float  ui     = EditorScale();
    const float  width  = ImGui::GetContentRegionAvail().x;
    const float  height = std::round(ImGui::GetFrameHeight() * 0.95f);
    const ImVec2 a      = ImGui::GetCursorScreenPos();
    ImGui::BeginDisabled(!enabled);
    const bool clicked = ImGui::InvisibleButton("##dependency", ImVec2(width, height));
    ImGui::EndDisabled();
    const bool   hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
    const ImVec2 b(a.x + width, a.y + height);
    const float  middle = std::round((a.y + b.y) * 0.5f);
    const float  alpha  = enabled ? 1.0f : 0.5f;
    draw.AddRectFilled(a, b, Alpha(hovered && enabled ? RenderGraphStyle::RowHovered : RenderGraphStyle::Row, alpha), 4 * ui);
    const float arrowLeft  = a.x + 8 * ui;
    const float arrowRight = arrowLeft + 11 * ui;
    const float head       = 3.5f * ui;
    const ImU32 arrow      = Alpha(RenderGraphStyle::Secondary, alpha);
    draw.AddLine(ImVec2(arrowLeft, middle), ImVec2(arrowRight, middle), arrow, 1.5f * ui);
    const float tip  = input ? arrowLeft : arrowRight;
    const float tail = input ? tip + head : tip - head;
    draw.AddTriangleFilled(ImVec2(tip, middle), ImVec2(tail, middle - head), ImVec2(tail, middle + head), arrow);
    const float       textLeft   = arrowRight + 8 * ui;
    const float       reasonLeft = a.x + std::round(width * 0.56f);
    const float       chevron    = b.x - 12 * ui;
    const ImVec2      nameSize   = ImGui::CalcTextSize("Ag");
    const std::string shown      = Shorten(nullptr, name, reasonLeft - textLeft - 6 * ui);
    const std::string why        = Shorten(nullptr, reason, chevron - reasonLeft - 6 * ui);
    draw.AddText(ImVec2(textLeft, std::round(middle - nameSize.y * 0.5f)), Alpha(RenderGraphStyle::Text, alpha), shown.c_str());
    draw.AddText(ImVec2(reasonLeft, std::round(middle - nameSize.y * 0.5f)), Alpha(reasonColor, alpha), why.c_str());
    draw.AddLine(ImVec2(chevron, middle - 4 * ui), ImVec2(chevron + 4 * ui, middle), arrow, 1.5f * ui);
    draw.AddLine(ImVec2(chevron + 4 * ui, middle), ImVec2(chevron, middle + 4 * ui), arrow, 1.5f * ui);
    ImGui::Dummy(ImVec2(0, 2 * ui));
    return clicked && enabled;
}

void RenderGraphView::SelectPass(int32_t id)
{
    m_selected    = id;
    m_selectedKey = id >= 0 ? m_layout.keys[id] : std::string{};
}

void RenderGraphView::DrawToolbar(RenderGraphCaptureState& state)
{
    const rc::RDGDebugSnapshot& snapshot = state.GetSnapshot();
    const float                 scale    = EditorScale();
    const float                 width    = ImGui::GetContentRegionAvail().x;
    const float                 startX   = ImGui::GetCursorPosX();
    const float                 gap      = ImGui::GetStyle().ItemSpacing.x;
    const bool                  compact  = width < 1050 * scale;
    ImGui::PushStyleColor(ImGuiCol_Button,
                          state.IsFrozen() ? GetEditorPalette().treeSelection : ImGui::GetStyleColorVec4(ImGuiCol_Button));
    const bool freeze = compact ? EditorIconButton("Freeze", EditorIcon::Pause, true, "Freeze capture")
                                : EditorToolButton("Freeze", EditorIcon::Pause, true, "Keep the current captured graph");
    ImGui::PopStyleColor();
    if (freeze)
    {
        state.SetFrozen(!state.IsFrozen());
    }
    ImGui::SameLine();
    const bool refresh = compact
                           ? EditorIconButton("Refresh", EditorIcon::Refresh, true, "Capture the next available scene frame")
                           : EditorToolButton("Refresh", EditorIcon::Refresh);
    if (refresh)
    {
        state.Refresh();
    }
    ImGui::SameLine();
    const bool fit = compact ? EditorIconButton("Fit", EditorIcon::Fit, true, "Rebuild and fit the graph")
                             : EditorToolButton("Fit", EditorIcon::Fit);
    if (fit)
    {
        if (snapshot.accepted)
        {
            m_layout.Update(snapshot, m_showCulled, true, m_hideHost);
        }
        m_fit     = true;
        m_autoFit = true;
    }
    ImGui::SameLine();
    bool filtersChanged = false;
    if (!compact)
    {
        filtersChanged = ImGui::Checkbox("Culled passes", &m_showCulled);
        ImGui::SameLine();
    }
    if (ImGui::Button("View"))
    {
        ImGui::OpenPopup("GraphOptions");
    }
    if (ImGui::BeginPopup("GraphOptions"))
    {
        filtersChanged |= ImGui::Checkbox("Culled passes", &m_showCulled);
        filtersChanged |= ImGui::Checkbox("Hide host passes", &m_hideHost);
        ImGui::Checkbox("Trace upstream and downstream", &m_trace);
        ImGui::EndPopup();
    }
    if (filtersChanged && snapshot.accepted)
    {
        m_layoutChanged |= m_layout.Update(snapshot, m_showCulled, false, m_hideHost);
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(std::clamp(width * 0.17f, 80 * scale, 190 * scale));
    if (ImGui::InputTextWithHint("##FindPass", "Find pass", m_search.InputBuf, sizeof(m_search.InputBuf)))
    {
        m_search.Build();
        if (m_search.IsActive())
        {
            for (const rc::RDGDebugPass& pass : snapshot.passes)
            {
                if (m_layout.positions[pass.id].x >= 0 && m_search.PassFilter(pass.name.c_str()))
                {
                    SelectPass(pass.id);
                    m_focusSelection = true;
                    break;
                }
            }
        }
    }
    const float                     controlsEnd = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x;
    const RenderGraphCaptureSource& source      = state.GetCapturedSource();
    const char*                     status      = state.IsStale() ? "Stale" : state.IsFrozen() ? "Frozen" : "Live";
    const ImU32                     statusColor = state.IsStale()  ? ImGui::GetColorU32(GetEditorPalette().warning)
                                                : state.IsFrozen() ? RenderGraphStyle::Brand
                                                                   : RenderGraphStyle::Queues[1];
    const char*                     mode        = snapshot.mode == rc::RDGDebugExecutionMode::Grouped ? "Grouped" : "Graphics";
    const std::string               tooltip =
        fmt::format("{} | build {}\nAccepted for submission\n{}", snapshot.graph, snapshot.generation, state.GetAlgorithm());
    const float padding     = std::round(ImGui::GetFontSize() * 0.55f) * 2;
    const float stateWidth  = ImGui::CalcTextSize(status).x + padding;
    const float sourceWidth = ImGui::CalcTextSize(source.play ? "Play" : "Editor").x + padding;
    const float extraWidth =
        ImGui::CalcTextSize(state.GetAlgorithm().c_str()).x + ImGui::CalcTextSize(mode).x + padding * 2 + 2 * gap;
    const float helpWidth  = ImGui::GetFrameHeight();
    const float remaining  = startX + width - controlsEnd - gap;
    const bool  fullStatus = snapshot.accepted && remaining > stateWidth + sourceWidth + extraWidth + helpWidth + 3 * gap;
    const bool  showSource = snapshot.accepted && remaining > stateWidth + sourceWidth + helpWidth + 2 * gap;
    const float chipsWidth =
        stateWidth + helpWidth + gap + (showSource ? sourceWidth + gap : 0) + (fullStatus ? extraWidth : 0);
    ImGui::SameLine(std::max(controlsEnd + gap, startX + width - chipsWidth));
    if (showSource)
    {
        StatusChip(nullptr, source.play ? "Play" : "Editor", RenderGraphStyle::Text, 0, tooltip.c_str());
        ImGui::SameLine();
    }
    StatusChip(nullptr, status, statusColor, 0.2f, tooltip.c_str());
    if (fullStatus)
    {
        ImGui::SameLine();
        StatusChip(nullptr, state.GetAlgorithm().c_str(), RenderGraphStyle::Text, 0, tooltip.c_str());
        ImGui::SameLine();
        StatusChip(nullptr, mode, RenderGraphStyle::Text, 0, "Submission mode");
    }
    ImGui::SameLine();
    if (ImGui::Button("?", ImVec2(helpWidth, 0)))
    {
        ImGui::OpenPopup("GraphHelp");
    }
    if (ImGui::BeginPopup("GraphHelp"))
    {
        ImGui::TextUnformatted("Wheel to zoom. Middle/right drag to pan.");
        ImGui::TextUnformatted("Click a pass to inspect. Click the minimap to navigate.");
        ImGui::Separator();
        ImGui::TextUnformatted("Solid edges: resources. Dashed edges: cross-queue waits.");
        ImGui::TextUnformatted("Dashed cards: host or culled passes. Badge: execution order.");
        ImGui::TextUnformatted("Trace highlights all upstream and downstream work.");
        for (uint32_t lane = 0; lane < 4; ++lane)
        {
            StatusChip(nullptr, LaneName(lane), RenderGraphStyle::Queues[lane], 0.2f);
            ImGui::SameLine();
        }
        ImGui::NewLine();
        ImGui::TextWrapped("Host and group labels are inferred from pass names. Hidden passes stay in the captured graph.");
        ImGui::EndPopup();
    }
    if (snapshot.accepted)
    {
        const size_t      slash   = source.path.find_last_of("/\\");
        const std::string scene   = slash == std::string::npos ? source.path : source.path.substr(slash + 1);
        const std::string summary = fmt::format(
            "{}  \xc2\xb7  rev {}  \xc2\xb7  frame {}  \xc2\xb7  {} passes / {} culled / {} resources", scene, source.revision,
            snapshot.frame, snapshot.stats.passCount, snapshot.stats.culledPassCount, snapshot.stats.resourceCount);
        ImGui::TextDisabled("%s", Shorten(nullptr, summary, width).c_str());
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("%s\n%s\n%s", source.path.c_str(), summary.c_str(), tooltip.c_str());
        }
    }
}

void RenderGraphView::Draw(RenderGraphCaptureState& state)
{
    const rc::RDGDebugSnapshot& snapshot = state.GetSnapshot();
    if (snapshot.accepted)
    {
        const RenderGraphCaptureSource& source = state.GetCapturedSource();
        if (m_scene != source.scene || m_play != source.play)
        {
            m_scene = source.scene;
            m_play  = source.play;
            SelectPass(-1);
            m_layout         = {};
            m_fit            = true;
            m_autoFit        = true;
            m_stableCaptures = 0;
        }
        const ImVec2 previous   = m_selected >= 0 ? m_layout.positions[m_selected] : ImVec2(-1, -1);
        const bool   newCapture = snapshot.frame != m_stableFrame;
        m_stableFrame           = snapshot.frame;
        if (newCapture)
        {
            ++m_stableCaptures;
        }
        if (m_layout.Update(snapshot, m_showCulled, false, m_hideHost))
        {
            m_layoutChanged     = true;
            m_stableCaptures    = 0;
            const auto selected = std::find(m_layout.keys.begin(), m_layout.keys.end(), m_selectedKey);
            m_selected          = selected != m_layout.keys.end() ? int32_t(selected - m_layout.keys.begin()) : -1;
            if (m_selected >= 0 && previous.x >= 0 && m_layout.positions[m_selected].x >= 0)
            {
                // Keep the selected pass where it was on screen, including mid-animation.
                const ImVec2 current = m_layout.positions[m_selected];
                const float  zoom    = (m_navigation.IsActive() ? m_navigation.targetZoom : m_navigation.zoom) * EditorScale();
                m_navigation.Offset(ImVec2((previous.x - current.x) * zoom, (previous.y - current.y) * zoom));
            }
        }
        if (!m_requestedSelection.empty())
        {
            for (const rc::RDGDebugPass& pass : snapshot.passes)
            {
                if (m_requestedSelection == pass.name && m_layout.positions[pass.id].x >= 0)
                {
                    SelectPass(pass.id);
                    m_focusSelection = true;
                    m_requestedSelection.clear();
                }
            }
        }
    }
    DrawToolbar(state);
    if (state.IsAwaitingApply())
    {
        ImGui::TextColored(GetEditorPalette().warning, "Pending Apply changes are not included.");
    }
    if (!state.GetUnavailable().empty())
    {
        ImGui::TextWrapped("%s", state.GetUnavailable().c_str());
    }
    if (!state.GetError().empty())
    {
        ImGui::TextColored(GetEditorPalette().error, "%s", state.GetError().c_str());
    }
    if (!state.GetFallback().empty())
    {
        ImGui::TextWrapped("Fallback: %s", state.GetFallback().c_str());
    }
    if (snapshot.accepted)
    {
        const float inspector = std::min(380 * EditorScale(), ImGui::GetContentRegionAvail().x * 0.36f);
        ImGui::BeginChild("GraphCanvas", ImVec2(-inspector, 0), ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        DrawCanvas(snapshot);
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("GraphInspector", ImVec2(0, 0));
        DrawInspector(snapshot);
        ImGui::EndChild();
    }
    else if (state.GetUnavailable().empty())
    {
        ImGui::TextUnformatted("Waiting for the next scene frame...");
    }
}

void RenderGraphView::DrawEdgeDetails(const rc::RDGDebugSnapshot& snapshot, const RenderGraphEdge& edge)
{
    ImGui::Text("%s -> %s", snapshot.passes[edge.source].name.c_str(), snapshot.passes[edge.destination].name.c_str());
    if (snapshot.passes[edge.source].queue != snapshot.passes[edge.destination].queue)
    {
        ImGui::TextUnformatted(QueueConnectionName(snapshot, edge));
    }
    if (edge.ordering)
    {
        ImGui::TextUnformatted("Submission group ordering");
    }
    for (uint32_t index : edge.reasons)
    {
        const rc::RDGDependency&    reason   = snapshot.dependencies[index];
        const rc::RDGDebugResource* resource = FindResource(snapshot, reason.resourceId);
        ImGui::TextWrapped("%s: %s (#%d), version %d", ReasonName(reason.reason),
                           resource != nullptr ? resource->name.c_str() : "Resource", int32_t(reason.resourceId),
                           reason.version);
    }
}

void RenderGraphView::DrawInspector(const rc::RDGDebugSnapshot& snapshot)
{
    if (m_selected >= 0 && size_t(m_selected) < snapshot.passes.size())
    {
        // Header as in the mockup: the pass name, one muted identity line, then chips.
        const rc::RDGDebugPass& pass = snapshot.passes[m_selected];
        const uint32_t          lane = m_layout.passLanes[pass.id];
        ImGui::TextWrapped("%s", pass.name.c_str());
        ImGui::TextDisabled("#%d  \xc2\xb7  order %s  \xc2\xb7  %s queue", pass.id,
                            pass.live ? std::to_string(pass.order).c_str() : "--", LaneName(lane));
        uint32_t waits = 0;
        for (uint32_t index : m_layout.incoming[pass.id])
        {
            waits += m_layout.passLanes[m_layout.edges[index].source] != lane;
        }
        const size_t      inCount  = m_layout.incoming[pass.id].size();
        const size_t      outCount = m_layout.outgoing[pass.id].size();
        const std::string inputs   = fmt::format("{} input{}", inCount, inCount == 1 ? "" : "s");
        const std::string outputs  = fmt::format("{} output{}", outCount, outCount == 1 ? "" : "s");
        const std::string queue    = fmt::format("{} queue wait{}", waits, waits == 1 ? "" : "s");
        StatusChip(nullptr, pass.live ? "Live" : "Culled",
                   pass.live ? RenderGraphStyle::Queues[1] : RenderGraphStyle::Secondary, pass.live ? 0.2f : 0);
        ImGui::SameLine();
        StatusChip(nullptr, inputs.c_str(), RenderGraphStyle::Text, 0);
        ImGui::SameLine();
        StatusChip(nullptr, outputs.c_str(), RenderGraphStyle::Text, 0);
        if (waits > 0 && ImGui::GetContentRegionAvail().x > ChipWidth(nullptr, queue.c_str()) + 8)
        {
            ImGui::SameLine();
            StatusChip(nullptr, queue.c_str(), RenderGraphStyle::Queues[1], 0.2f);
        }
        ImGui::SeparatorText("Resources");
        if (ImGui::BeginTable("PassResources", 4,
                              ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_Resizable
                                  | ImGuiTableFlags_SizingStretchProp))
        {
            // Short columns fit their content so formats never wrap; the name takes the rest.
            ImGui::TableSetupColumn("Resource", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Access", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("Access").x);
            ImGui::TableSetupColumn("Format", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("RGBA16F").x);
            ImGui::TableSetupColumn("Ver", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("+v99").x);
            ImGui::TableHeadersRow();
            for (const rc::RDGAccess& access : pass.accesses)
            {
                const rc::RDGDebugResource* resource = FindResource(snapshot, access.resourceId);
                const bool                  writes   = access.accessMode == RHIAccessMode::eReadWrite;
                const char* mode = writes ? "R/W" : access.accessMode == RHIAccessMode::eRead ? "Read" : "State";
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                const std::string resourceName = resource != nullptr ? resource->name : "Resource";
                ImGui::TextUnformatted(Shorten(nullptr, resourceName, ImGui::GetContentRegionAvail().x).c_str());
                if (resource != nullptr && ImGui::IsItemHovered())
                {
                    ResourceTooltip(*resource, access);
                }
                ImGui::TableNextColumn();
                ImGui::TextColored(writes ? GetEditorPalette().brand : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled), "%s",
                                   mode);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(resource != nullptr && resource->type == rc::RDGResourceType::eTexture
                                           ? FormatName(resource->texture.format)
                                           : "Buffer");
                ImGui::TableNextColumn();
                for (const rc::RDGVersionAccess& version : pass.versions)
                {
                    if (version.resourceId == access.resourceId && version.version >= 0
                        && size_t(version.version) < snapshot.versions.size())
                    {
                        ImGui::TextColored(version.writes ? GetEditorPalette().brand
                                                          : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled),
                                           "%s%u", version.writes ? "+v" : "v", snapshot.versions[version.version].number);
                    }
                }
            }
            ImGui::EndTable();
        }
        ImGui::SeparatorText("Dependencies");
        for (const RenderGraphEdge& edge : m_layout.edges)
        {
            if (edge.source == pass.id || edge.destination == pass.id)
            {
                const bool    input      = edge.destination == pass.id;
                const int32_t other      = input ? edge.source : edge.destination;
                const bool    crossQueue = m_layout.passLanes[edge.source] != m_layout.passLanes[edge.destination];
                const bool    visible    = m_layout.positions[other].x >= 0;
                std::string   reason     = crossQueue           ? QueueConnectionName(snapshot, edge)
                                         : edge.reasons.empty() ? "Submission order"
                                                                : ReasonName(snapshot.dependencies[edge.reasons[0]].reason);
                if (!crossQueue && edge.reasons.size() > 1)
                {
                    reason += fmt::format(" \xc3\x97{}", edge.reasons.size());
                }
                ImGui::PushID(other);
                if (DependencyRow(snapshot.passes[other].name, reason.c_str(),
                                  crossQueue ? RenderGraphStyle::Queues[1] : RenderGraphStyle::Secondary, input, visible))
                {
                    SelectPass(other);
                    m_focusSelection = true;
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                {
                    ImGui::BeginTooltip();
                    DrawEdgeDetails(snapshot, edge);
                    if (!visible)
                    {
                        ImGui::TextUnformatted("Hidden by the current View filters");
                    }
                    ImGui::EndTooltip();
                }
                ImGui::PopID();
            }
        }
        ImGui::SeparatorText("Queue decision");
        ImGui::TextWrapped("%s", pass.live ? LaneName(lane) : "Culled; declared accesses only, no executed queue.");
        if (pass.preference == rc::RDGQueuePreference::ePreferAsyncCompute)
        {
            ImGui::TextWrapped("Async requested: %s%s", rc::AsyncComputeEligibilityName(pass.eligibility),
                               snapshot.mode == rc::RDGDebugExecutionMode::Grouped ? ""
                                                                                   : " (single graphics list; not applied)");
        }
        if (pass.group < snapshot.groups.size())
        {
            const rc::RDGSubmissionGroup& group = snapshot.groups[pass.group];
            ImGui::TextDisabled("Group %u | queue equivalence %u", group.id, group.queueEquivalenceId);
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Groups with the same equivalence share a native queue.");
            }
            for (uint32_t external : group.externalPredecessors)
            {
                ImGui::Text("External predecessor %u", external);
            }
        }
    }
    else
    {
        ImGui::TextWrapped(m_selectedKey.empty()
                               ? "Select a pass to see its resources, dependencies and queue decision."
                               : "The selected pass is absent from this capture; selection will resume when it returns.");
        ImGui::Spacing();
        ImGui::TextWrapped("Find a pass in the toolbar to jump to it.");
    }
    ImGui::Spacing();
    const std::string warnings = fmt::format("Graph warnings ({})", snapshot.warnings.size());
    if (!snapshot.warnings.empty() && ImGui::CollapsingHeader(warnings.c_str()))
    {
        for (const std::string& warning : snapshot.warnings)
        {
            ImGui::TextWrapped("%s", warning.c_str());
        }
    }
}
} // namespace zen::editor
