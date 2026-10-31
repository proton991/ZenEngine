#include "Editor/ImGui/RenderGraphView.h"
#include "Templates/HashMap.h"
#include <algorithm>
#include <cmath>

namespace zen::editor
{
namespace
{
constexpr float kColumnSpacing = RenderGraphStyle::ColumnPitch;
constexpr float kLayerSpacing  = RenderGraphStyle::LayerPitch;
constexpr float kLaneGap       = RenderGraphStyle::LaneGap;
constexpr float kHeaderHeight  = RenderGraphStyle::HeaderHeight;

void HashValue(uint64_t& hash, uint64_t value)
{
    hash = (hash ^ value) * 1099511628211ull;
}

uint64_t Fingerprint(const rc::RDGDebugSnapshot& snapshot, bool culled, bool hideHost)
{
    uint64_t hash = 14695981039346656037ull;
    HashValue(hash, uint64_t(snapshot.mode));
    HashValue(hash, culled);
    HashValue(hash, hideHost);
    for (const rc::RDGDebugPass& pass : snapshot.passes)
    {
        HashValue(hash, uint64_t(pass.id));
        HashValue(hash, uint64_t(pass.type));
        HashValue(hash, pass.live);
        HashValue(hash, pass.order);
        HashValue(hash, uint64_t(pass.queue));
        HashValue(hash, pass.group);
        for (unsigned char c : pass.name)
        {
            HashValue(hash, c);
        }
    }
    for (const rc::RDGDependency& edge : snapshot.dependencies)
    {
        HashValue(hash, uint64_t(edge.source));
        HashValue(hash, uint64_t(edge.destination));
        HashValue(hash, uint64_t(edge.resourceId));
        HashValue(hash, uint64_t(edge.reason));
        HashValue(hash, uint64_t(edge.version));
    }
    for (const rc::RDGSubmissionGroup& group : snapshot.groups)
    {
        HashValue(hash, group.id);
        HashValue(hash, group.queueEquivalenceId);
        for (const rc::RDGScheduledPass& pass : group.passes)
        {
            HashValue(hash, uint64_t(pass.nodeId));
        }
        for (uint32_t predecessor : group.predecessors)
        {
            HashValue(hash, predecessor);
        }
    }
    return hash;
}

uint32_t Lane(const rc::RDGDebugPass& pass)
{
    return !pass.live                                         ? 3u
         : pass.queue == RHICommandContextType::eAsyncCompute ? 1u
         : pass.queue == RHICommandContextType::eTransfer     ? 2u
                                                              : 0u;
}

struct LayoutVertex
{
    uint32_t             lane;
    uint32_t             layer;
    float                column;
    bool                 routing;
    HeapVector<uint32_t> parents;
    HeapVector<uint32_t> children;
};

using LayoutRows = HeapVector<HeapVector<uint32_t>>;

float VertexPitch(const LayoutVertex& vertex)
{
    return vertex.routing ? 0.1f : 1.0f;
}

constexpr float kRouteWidth = 12.0f;

float VertexWidth(const LayoutVertex& vertex)
{
    return vertex.routing ? kRouteWidth : RenderGraphLayout::NodeWidth;
}

float VertexCenter(const LayoutVertex& vertex)
{
    return vertex.column + VertexWidth(vertex) / (2 * kColumnSpacing);
}

struct OccupiedRange
{
    float left;
    float right;
};

using OccupiedRows = HeapVector<HeapVector<OccupiedRange>>;

// Sorted, disjoint intervals let a channel query inspect just the first range
// ending after its left edge. Merge reservations as cards/channels are inserted.
void ReserveRange(HeapVector<OccupiedRange>& ranges, OccupiedRange range)
{
    if (ranges.empty())
    {
        ranges.push_back(range);
    }
    else
    {
        const auto first = std::lower_bound(ranges.begin(), ranges.end(), range.left,
                                            [](const OccupiedRange& occupied, float left) { return occupied.right < left; });
        auto       last  = first;
        while (last != ranges.end() && last->left <= range.right)
        {
            range.left  = std::min(range.left, last->left);
            range.right = std::max(range.right, last->right);
            ++last;
        }
        const auto position = ranges.erase(first, last);
        ranges.insert(position, &range, &range + 1);
    }
}

float FindChannel(const OccupiedRows& occupied,
                  uint32_t            lane,
                  uint32_t            firstLayer,
                  uint32_t            lastLayer,
                  float               target,
                  bool                right)
{
    float x    = target;
    bool  free = false;
    while (x >= 0 && !free)
    {
        free = true;
        for (uint32_t layer = firstLayer; free && layer <= lastLayer; ++layer)
        {
            const HeapVector<OccupiedRange>& ranges = occupied[layer * 4 + lane];
            const auto found = std::upper_bound(ranges.begin(), ranges.end(), x,
                                                [](float left, const OccupiedRange& range) { return left < range.right; });
            if (found != ranges.end() && x + kRouteWidth > found->left)
            {
                // Jump to the next gap on the same four-pixel lattice instead of
                // trying every position inside a card or an already occupied channel.
                x    = right ? std::max(x + 4, target + std::ceil((found->right - target) / 4) * 4)
                             : std::min(x - 4, target + std::floor((found->left - kRouteWidth - target) / 4) * 4);
                free = false;
            }
        }
    }
    return x;
}

// Where a long edge's routing point should sit: on the line between its endpoints
// within the destination lane, or directly above the destination for a cross-lane edge.
float RouteTarget(const LayoutVertex& source, const LayoutVertex& destination, uint32_t step, uint32_t steps)
{
    const float start = source.lane == destination.lane ? VertexCenter(source) : VertexCenter(destination);
    const float t     = float(step) / float(steps);
    return start + (VertexCenter(destination) - start) * t - kRouteWidth / (2 * kColumnSpacing);
}

// A long edge runs in one vertical channel: free space beside every card and every
// earlier channel across its skipped layers, as near as possible to the midpoint of its
// endpoints. The edge then bends only where it leaves the source and enters the
// destination, instead of stepping sideways at every layer. Cards never move.
void ChannelRoutes(HeapVector<LayoutVertex>&               vertices,
                   const LayoutRows&                       rows,
                   const HeapVector<HeapVector<uint32_t>>& routes,
                   const HeapVector<uint32_t>&             sources,
                   const HeapVector<uint32_t>&             destinations)
{
    OccupiedRows occupied(rows.size() * 4);
    for (const LayoutVertex& vertex : vertices)
    {
        if (!vertex.routing)
        {
            const float x = vertex.column * kColumnSpacing;
            ReserveRange(occupied[vertex.layer * 4 + vertex.lane], {x - 8, x + VertexWidth(vertex) + 8});
        }
    }
    HeapVector<uint32_t> order;
    for (uint32_t index = 0; index < routes.size(); ++index)
    {
        if (!routes[index].empty())
        {
            order.push_back(index);
        }
    }
    // The longest edges choose first, so they get the channels nearest their endpoints.
    std::stable_sort(order.begin(), order.end(),
                     [&routes](uint32_t a, uint32_t b) { return routes[a].size() > routes[b].size(); });
    for (uint32_t index : order)
    {
        const LayoutVertex& source      = vertices[sources[index]];
        const LayoutVertex& destination = vertices[destinations[index]];
        const float         center = source.lane == destination.lane ? (VertexCenter(source) + VertexCenter(destination)) * 0.5f
                                                                     : VertexCenter(destination);
        const float         target = center * kColumnSpacing - kRouteWidth * 0.5f;
        const uint32_t      firstLayer = vertices[routes[index].front()].layer;
        const uint32_t      lastLayer  = vertices[routes[index].back()].layer;
        const float         left       = FindChannel(occupied, destination.lane, firstLayer, lastLayer, target, false);
        const float         right      = FindChannel(occupied, destination.lane, firstLayer, lastLayer, target, true);
        const float         x          = left >= 0 && target - left <= right - target ? left : right;
        for (uint32_t id : routes[index])
        {
            vertices[id].column = x / kColumnSpacing;
            ReserveRange(occupied[vertices[id].layer * 4 + destination.lane], {x - 4, x + kRouteWidth + 4});
        }
    }
}

ImVec2 EdgeCurvePoint(ImVec2 a, ImVec2 b, float t)
{
    const float u      = 1 - t;
    const float middle = (a.y + b.y) * 0.5f;
    return ImVec2(a.x * (u * u * u + 3 * u * u * t) + b.x * (3 * u * t * t + t * t * t),
                  a.y * u * u * u + middle * (3 * u * u * t + 3 * u * t * t) + b.y * t * t * t);
}

// True when the single curve between two ports clears every other card, so a long edge
// can be drawn as one smooth curve, as in the mockup, rather than through a channel.
bool ClearsCards(const RenderGraphLayout& layout, ImVec2 a, ImVec2 b, int32_t source, int32_t destination)
{
    bool clear = true;
    for (uint32_t sample = 1; clear && sample < 24; ++sample)
    {
        const ImVec2 p = EdgeCurvePoint(a, b, float(sample) / 24);
        for (uint32_t id = 0; clear && id < layout.positions.size(); ++id)
        {
            const ImVec2 card = layout.positions[id];
            clear             = card.x < 0 || int32_t(id) == source || int32_t(id) == destination || p.x < card.x - 8
                             || p.x > card.x + RenderGraphLayout::NodeWidth + 8 || p.y < card.y - 6
                             || p.y > card.y + RenderGraphLayout::NodeHeight + 6;
        }
    }
    return clear;
}

float NeighbourCenter(const HeapVector<LayoutVertex>& vertices, uint32_t id, bool down)
{
    const LayoutVertex&         vertex     = vertices[id];
    const HeapVector<uint32_t>& neighbours = down ? vertex.parents : vertex.children;
    float                       total      = 0;
    for (uint32_t neighbour : neighbours)
    {
        total += vertices[neighbour].column + VertexWidth(vertices[neighbour]) / (2 * kColumnSpacing);
    }
    return neighbours.empty() ? vertex.column : total / float(neighbours.size());
}

uint64_t CountCrossings(const HeapVector<LayoutVertex>& vertices, const LayoutRows& rows)
{
    uint64_t count = 0;
    for (const HeapVector<uint32_t>& row : rows)
    {
        for (size_t a = 0; a < row.size(); ++a)
        {
            const LayoutVertex& left = vertices[row[a]];
            for (size_t b = a + 1; b < row.size(); ++b)
            {
                const LayoutVertex& right = vertices[row[b]];
                for (uint32_t first : left.children)
                {
                    for (uint32_t second : right.children)
                    {
                        const LayoutVertex& p = vertices[first];
                        const LayoutVertex& q = vertices[second];
                        if (p.lane > q.lane || (p.lane == q.lane && p.column > q.column))
                        {
                            ++count;
                        }
                    }
                }
            }
        }
    }
    return count;
}

void OrderLayers(HeapVector<LayoutVertex>& vertices, LayoutRows& rows)
{
    LayoutRows best      = rows;
    uint64_t   crossings = CountCrossings(vertices, rows);
    // Alternating barycenter sweeps include routing vertices. Keep the best
    // ordering, since a later sweep can trade one crossing for another.
    for (uint32_t sweep = 0; sweep < 4; ++sweep)
    {
        const bool down = sweep % 2 == 0;
        for (size_t step = 0; step < rows.size(); ++step)
        {
            HeapVector<uint32_t>& row = rows[down ? step : rows.size() - step - 1];
            HeapVector<float>     centers(vertices.size());
            for (uint32_t id : row)
            {
                centers[id] = NeighbourCenter(vertices, id, down);
            }
            std::stable_sort(row.begin(), row.end(), [&centers, &vertices](uint32_t a, uint32_t b) {
                return vertices[a].lane != vertices[b].lane ? vertices[a].lane < vertices[b].lane : centers[a] < centers[b];
            });
            float columns[4]{};
            for (uint32_t id : row)
            {
                vertices[id].column         = columns[vertices[id].lane];
                columns[vertices[id].lane] += VertexPitch(vertices[id]);
            }
        }
        const uint64_t current = CountCrossings(vertices, rows);
        if (current < crossings)
        {
            crossings = current;
            best      = rows;
        }
    }
    rows = std::move(best);
    float laneSpans[4]{};
    for (const HeapVector<uint32_t>& row : rows)
    {
        float columns[4]{};
        for (uint32_t id : row)
        {
            vertices[id].column           = columns[vertices[id].lane];
            columns[vertices[id].lane]   += VertexPitch(vertices[id]);
            laneSpans[vertices[id].lane]  = std::max(laneSpans[vertices[id].lane], columns[vertices[id].lane]);
        }
    }
    // Pull each ordered row toward its parents' median, without changing its
    // order or allowing nodes (including invisible routing nodes) to overlap.
    for (const HeapVector<uint32_t>& row : rows)
    {
        float next[4]{};
        float remaining[4]{};
        for (uint32_t id : row)
        {
            remaining[vertices[id].lane] += VertexPitch(vertices[id]);
        }
        for (uint32_t id : row)
        {
            LayoutVertex&     vertex = vertices[id];
            HeapVector<float> parents;
            for (uint32_t parent : vertex.parents)
            {
                if (vertices[parent].lane == vertex.lane)
                {
                    parents.push_back(vertices[parent].column + VertexWidth(vertices[parent]) / (2 * kColumnSpacing));
                }
            }
            std::sort(parents.begin(), parents.end());
            const float median =
                parents.empty() ? vertex.column : parents[parents.size() / 2] - VertexWidth(vertex) / (2 * kColumnSpacing);
            vertex.column           = std::clamp(median, next[vertex.lane],
                                                 std::max(next[vertex.lane], laneSpans[vertex.lane] - remaining[vertex.lane]));
            remaining[vertex.lane] -= VertexPitch(vertex);
            next[vertex.lane]       = vertex.column + VertexPitch(vertex);
        }
    }
}

// Lane widths come from cards and from the channels of edges that still use them.
void PlaceLanes(RenderGraphLayout&              layout,
                const HeapVector<LayoutVertex>& vertices,
                const HeapVector<uint32_t>&     passVertices,
                const HeapVector<uint32_t>&     owners,
                const HeapVector<bool>&         direct)
{
    std::fill(std::begin(layout.laneWidth), std::end(layout.laneWidth), 0.0f);
    for (uint32_t id = 0; id < vertices.size(); ++id)
    {
        const LayoutVertex& vertex = vertices[id];
        if (!vertex.routing || owners[id] == UINT32_MAX || !direct[owners[id]])
        {
            layout.laneWidth[vertex.lane] =
                std::max(layout.laneWidth[vertex.lane], vertex.column * kColumnSpacing + VertexWidth(vertex));
        }
    }
    float x = 0;
    for (uint32_t lane = 0; lane < 4; ++lane)
    {
        layout.laneX[lane] = x;
        if (layout.laneWidth[lane] > 0)
        {
            layout.extent.x = x + layout.laneWidth[lane];
            x               = layout.extent.x + kLaneGap;
        }
    }
    for (uint32_t id = 0; id < passVertices.size(); ++id)
    {
        if (passVertices[id] != UINT32_MAX)
        {
            const LayoutVertex& vertex = vertices[passVertices[id]];
            layout.positions[id]       = ImVec2(layout.laneX[vertex.lane] + vertex.column * kColumnSpacing,
                                                kHeaderHeight + float(vertex.layer) * kLayerSpacing);
        }
    }
}

// Stable port order follows the opposite endpoint's horizontal position.
void AssignPorts(const RenderGraphLayout& layout, HeapVector<float>& sourcePorts, HeapVector<float>& destinationPorts)
{
    for (uint32_t id = 0; id < layout.positions.size(); ++id)
    {
        HeapVector<uint32_t> outgoing;
        HeapVector<uint32_t> incoming;
        for (uint32_t index : layout.outgoing[id])
        {
            if (layout.positions[layout.edges[index].destination].x >= 0)
            {
                outgoing.push_back(index);
            }
        }
        for (uint32_t index : layout.incoming[id])
        {
            if (layout.positions[layout.edges[index].source].x >= 0)
            {
                incoming.push_back(index);
            }
        }
        std::stable_sort(outgoing.begin(), outgoing.end(), [&layout](uint32_t a, uint32_t b) {
            return layout.positions[layout.edges[a].destination].x < layout.positions[layout.edges[b].destination].x;
        });
        std::stable_sort(incoming.begin(), incoming.end(), [&layout](uint32_t a, uint32_t b) {
            return layout.positions[layout.edges[a].source].x < layout.positions[layout.edges[b].source].x;
        });
        for (uint32_t port = 0; port < outgoing.size(); ++port)
        {
            sourcePorts[outgoing[port]] = float(port + 1) / float(outgoing.size() + 1);
        }
        for (uint32_t port = 0; port < incoming.size(); ++port)
        {
            destinationPorts[incoming[port]] = float(port + 1) / float(incoming.size() + 1);
        }
    }
}

ImVec2 SourcePort(const RenderGraphLayout& layout, const RenderGraphEdge& edge, float port)
{
    const ImVec2 source = layout.positions[edge.source];
    return ImVec2(source.x + 12 + port * (RenderGraphLayout::NodeWidth - 24), source.y + RenderGraphLayout::NodeHeight);
}

ImVec2 DestinationPort(const RenderGraphLayout& layout, const RenderGraphEdge& edge, float port)
{
    const ImVec2 destination = layout.positions[edge.destination];
    return ImVec2(destination.x + 12 + port * (RenderGraphLayout::NodeWidth - 24), destination.y);
}

void PlaceRoutes(RenderGraphLayout& layout, const HeapVector<uint32_t>& layers, bool rebuild)
{
    HeapVector<LayoutVertex> vertices;
    HeapVector<uint32_t>     passVertices(layout.positions.size(), UINT32_MAX);
    uint32_t                 maxLayer = 0;
    for (uint32_t id = 0; id < layout.positions.size(); ++id)
    {
        if (layout.positions[id].x >= 0)
        {
            passVertices[id] = uint32_t(vertices.size());
            vertices.push_back({layout.passLanes[id], layers[id], layout.positions[id].x / kColumnSpacing, false, {}, {}});
            maxLayer = std::max(maxLayer, layers[id]);
        }
    }
    LayoutRows rows(maxLayer + 1);
    for (uint32_t id = 0; id < vertices.size(); ++id)
    {
        rows[vertices[id].layer].push_back(id);
    }
    HeapVector<HeapVector<uint32_t>> routes(layout.edges.size());
    HeapVector<uint32_t>             routeSources(layout.edges.size(), 0);
    HeapVector<uint32_t>             routeDestinations(layout.edges.size(), 0);
    for (uint32_t index = 0; index < layout.edges.size(); ++index)
    {
        const RenderGraphEdge& edge = layout.edges[index];
        if (passVertices[edge.source] != UINT32_MAX && passVertices[edge.destination] != UINT32_MAX)
        {
            uint32_t       previous    = passVertices[edge.source];
            const uint32_t destination = passVertices[edge.destination];
            const uint32_t lane        = vertices[destination].lane;
            const uint32_t steps       = layers[edge.destination] - layers[edge.source];
            routeSources[index]        = previous;
            routeDestinations[index]   = destination;
            for (uint32_t layer = layers[edge.source] + 1; layer < layers[edge.destination]; ++layer)
            {
                // This is only a seed for the ordering sweeps. ChannelRoutes assigns
                // collision-free positions afterwards, including incremental updates.
                const float    column = std::round(RouteTarget(vertices[passVertices[edge.source]], vertices[destination],
                                                               layer - layers[edge.source], steps)
                                                   * 10)
                                      / 10;
                const uint32_t id     = uint32_t(vertices.size());
                vertices.push_back({lane, layer, column, true, {previous}, {}});
                vertices[previous].children.push_back(id);
                rows[layer].push_back(id);
                routes[index].push_back(id);
                previous = id;
            }
            vertices[previous].children.push_back(destination);
            vertices[destination].parents.push_back(previous);
        }
    }
    for (HeapVector<uint32_t>& row : rows)
    {
        std::sort(row.begin(), row.end(), [&vertices](uint32_t a, uint32_t b) {
            return vertices[a].lane != vertices[b].lane ? vertices[a].lane < vertices[b].lane
                                                        : vertices[a].column < vertices[b].column;
        });
    }
    if (rebuild)
    {
        OrderLayers(vertices, rows);
    }
    ChannelRoutes(vertices, rows, routes, routeSources, routeDestinations);
    HeapVector<uint32_t> owners(vertices.size(), UINT32_MAX);
    for (uint32_t index = 0; index < routes.size(); ++index)
    {
        for (uint32_t id : routes[index])
        {
            owners[id] = index;
        }
    }
    // Long edges whose single curve clears every card drop their channel, and the lanes
    // are then sized again without it.
    HeapVector<bool>  direct(layout.edges.size(), false);
    HeapVector<float> sourcePorts(layout.edges.size(), 0.5f);
    HeapVector<float> destinationPorts(layout.edges.size(), 0.5f);
    PlaceLanes(layout, vertices, passVertices, owners, direct);
    AssignPorts(layout, sourcePorts, destinationPorts);
    bool anyDirect = false;
    for (uint32_t index = 0; index < layout.edges.size(); ++index)
    {
        const RenderGraphEdge& edge = layout.edges[index];
        if (!routes[index].empty())
        {
            direct[index]  = ClearsCards(layout, SourcePort(layout, edge, sourcePorts[index]),
                                         DestinationPort(layout, edge, destinationPorts[index]), edge.source, edge.destination);
            anyDirect     |= direct[index];
        }
    }
    if (anyDirect)
    {
        PlaceLanes(layout, vertices, passVertices, owners, direct);
        AssignPorts(layout, sourcePorts, destinationPorts);
    }
    for (uint32_t index = 0; index < layout.edges.size(); ++index)
    {
        RenderGraphEdge& edge = layout.edges[index];
        if (layout.positions[edge.source].x >= 0 && layout.positions[edge.destination].x >= 0)
        {
            edge.route.push_back(SourcePort(layout, edge, sourcePorts[index]));
            for (uint32_t id : routes[index])
            {
                const LayoutVertex& vertex = vertices[id];
                const float px = layout.laneX[vertex.lane] + vertex.column * kColumnSpacing + VertexWidth(vertex) * 0.5f;
                const float py = kHeaderHeight + float(vertex.layer) * kLayerSpacing;
                if (!direct[index])
                {
                    edge.route.push_back(ImVec2(px, py - 5));
                    edge.route.push_back(ImVec2(px, py + RenderGraphLayout::NodeHeight + 5));
                }
            }
            edge.route.push_back(DestinationPort(layout, edge, destinationPorts[index]));
        }
    }
}
} // namespace

bool IsRenderGraphHostPass(const std::string& name)
{
    return name == "UI" || name == "WorkspaceClear" || name == "EditorObjectIDs" || name == "EditorReadPick"
        || name == "EditorPickReadback" || name == "EditorSelectionBounds" || name.starts_with("EditorMeshPreview")
        || name == "ScenePresentation";
}

bool RenderGraphLayout::Update(const rc::RDGDebugSnapshot& snapshot, bool showCulled, bool rebuild, bool hideHost)
{
    const uint64_t hash    = Fingerprint(snapshot, showCulled, hideHost);
    const bool     changed = rebuild || fingerprint != hash;
    if (changed)
    {
        bool fullLayout = rebuild || keys.empty();
        fingerprint     = hash;
        struct PreviousPosition
        {
            ImVec2   position;
            uint32_t lane;
        };
        HashMap<std::string, PreviousPosition> previous;
        for (size_t id = 0; id < keys.size(); ++id)
        {
            if (!rebuild && positions[id].x >= 0)
            {
                previous[keys[id]] = {ImVec2(positions[id].x - laneX[passLanes[id]], positions[id].y - kHeaderHeight),
                                      passLanes[id]};
            }
        }
        keys.clear();
        passLanes.clear();
        HashMap<std::string, uint32_t> occurrences;
        size_t                         matches = 0;
        size_t                         shown   = 0;
        for (const rc::RDGDebugPass& pass : snapshot.passes)
        {
            const std::string name = std::to_string(uint32_t(pass.type)) + ":" + pass.name;
            keys.push_back(std::to_string(occurrences[name]++) + ":" + name);
            passLanes.push_back(Lane(pass));
            const bool visible  = (pass.live || showCulled) && !(hideHost && IsRenderGraphHostPass(pass.name));
            shown              += visible;
            matches            += visible && previous.contains(keys.back());
        }
        // Retained cells only keep the view steady across small conditional changes. When
        // most passes are new or gone (an environment bake ending, a mode switch), start a
        // compact layout instead of keeping the old one's gaps.
        if (matches * 2 < previous.size() || matches * 2 < shown)
        {
            previous.clear();
            fullLayout = true;
        }
        positions.clear();
        positions.resize(snapshot.passes.size());
        std::fill(positions.begin(), positions.end(), ImVec2(-10000, -10000));
        edges.clear();
        std::fill(std::begin(lanes), std::end(lanes), false);
        std::fill(std::begin(laneCounts), std::end(laneCounts), 0u);
        incoming = HeapVector<HeapVector<uint32_t>>(snapshot.passes.size());
        outgoing = HeapVector<HeapVector<uint32_t>>(snapshot.passes.size());
        HeapVector<HeapVector<int32_t>> predecessors(snapshot.passes.size());
        HashMap<uint64_t, uint32_t>     pairs;
        for (uint32_t index = 0; index < snapshot.dependencies.size(); ++index)
        {
            const rc::RDGDependency& dependency = snapshot.dependencies[index];
            if (dependency.source >= 0 && dependency.destination >= 0 && size_t(dependency.source) < positions.size()
                && size_t(dependency.destination) < positions.size())
            {
                const uint64_t key = (uint64_t(uint32_t(dependency.source)) << 32) | uint32_t(dependency.destination);
                if (!pairs.contains(key))
                {
                    pairs[key] = uint32_t(edges.size());
                    edges.push_back({dependency.source, dependency.destination, false, {}});
                    predecessors[dependency.destination].push_back(dependency.source);
                }
                edges[pairs[key]].reasons.push_back(index);
            }
        }
        for (const rc::RDGSubmissionGroup& group : snapshot.groups)
        {
            for (uint32_t predecessor : group.predecessors)
            {
                if (!group.passes.empty() && predecessor < snapshot.groups.size()
                    && !snapshot.groups[predecessor].passes.empty())
                {
                    const int32_t  source      = snapshot.groups[predecessor].passes.back().nodeId;
                    const int32_t  destination = group.passes.front().nodeId;
                    const uint64_t key         = (uint64_t(uint32_t(source)) << 32) | uint32_t(destination);
                    if (!pairs.contains(key))
                    {
                        pairs[key] = uint32_t(edges.size());
                        edges.push_back({source, destination, true, {}});
                        predecessors[destination].push_back(source);
                    }
                    else
                    {
                        edges[pairs[key]].ordering = true;
                    }
                }
            }
        }
        for (uint32_t index = 0; index < edges.size(); ++index)
        {
            outgoing[edges[index].source].push_back(index);
            incoming[edges[index].destination].push_back(index);
        }
        HeapVector<uint32_t> layers(positions.size(), 0);
        for (const rc::RDGDebugPass& pass : snapshot.passes)
        {
            const uint32_t lane  = Lane(pass);
            const auto     found = previous.find(keys[pass.id]);
            if ((pass.live || showCulled) && !(hideHost && IsRenderGraphHostPass(pass.name)) && found != previous.end()
                && found->second.lane == lane)
            {
                positions[pass.id] = found->second.position;
                layers[pass.id]    = uint32_t(positions[pass.id].y / kLayerSpacing);
            }
        }
        HeapVector<int32_t> order;
        for (const rc::RDGDebugPass& pass : snapshot.passes)
        {
            if (pass.live || (showCulled && fullLayout))
            {
                order.push_back(pass.id);
            }
        }
        std::sort(order.begin(), order.end(),
                  [&snapshot](int32_t a, int32_t b) { return snapshot.passes[a].order < snapshot.passes[b].order; });
        HashMap<uint64_t, uint32_t> layerCounts;
        for (int32_t id : order)
        {
            const uint32_t previousLayer = layers[id];
            for (int32_t source : predecessors[id])
            {
                layers[id] = std::max(layers[id], layers[source] + 1);
            }
            if (fullLayout && !(hideHost && IsRenderGraphHostPass(snapshot.passes[id].name)))
            {
                // Wrap independent shadow faces only on Fit/full layout, so a
                // wide parallel frontier remains readable in a vertical view.
                const uint64_t lane = uint64_t(passLanes[id]) << 32;
                while (layerCounts[lane | layers[id]] >= 4)
                {
                    ++layers[id];
                }
                ++layerCounts[lane | layers[id]];
            }
            // A retained position is valid only while it sits below every current
            // predecessor. Propagate layer changes through all downstream passes.
            if (layers[id] != previousLayer)
            {
                positions[id] = ImVec2(-10000, -10000);
            }
        }
        // Reserve valid retained cells before placing new or displaced passes.
        HashMap<uint64_t, HeapVector<float>> occupancy;
        for (const rc::RDGDebugPass& pass : snapshot.passes)
        {
            if (positions[pass.id].x >= 0)
            {
                const uint64_t key = (uint64_t(Lane(pass)) << 32) | layers[pass.id];
                occupancy[key].push_back(positions[pass.id].x);
            }
        }
        extent = ImVec2(NodeWidth, kHeaderHeight + NodeHeight);
        for (const rc::RDGDebugPass& pass : snapshot.passes)
        {
            if ((pass.live || showCulled) && !(hideHost && IsRenderGraphHostPass(pass.name)))
            {
                const uint32_t lane = Lane(pass);
                if (positions[pass.id].x < 0)
                {
                    HeapVector<float>& occupied  = occupancy[(uint64_t(lane) << 32) | layers[pass.id]];
                    float              x         = 0;
                    bool               collision = true;
                    while (collision)
                    {
                        collision = false;
                        for (float other : occupied)
                        {
                            collision |= x + kColumnSpacing > other && x < other + kColumnSpacing;
                        }
                        if (collision)
                        {
                            x += kColumnSpacing;
                        }
                    }
                    occupied.push_back(x);
                    positions[pass.id] = ImVec2(x, float(layers[pass.id]) * kLayerSpacing);
                }
                ++laneCounts[lane];
                lanes[lane] = true;
                extent.y    = std::max(extent.y, positions[pass.id].y + kHeaderHeight + NodeHeight);
            }
        }
        PlaceRoutes(*this, layers, fullLayout);
    }
    return changed;
}
} // namespace zen::editor
