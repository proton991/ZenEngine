#include "Editor/Model/EditorScene.h"
#include "Editor/Model/EditorText.h"
#include "AssetLib/FastGLTFLoader.h"
#include <algorithm>
#include <limits>

namespace zen::editor
{
namespace
{
bool RayBounds(const sg::AABB& bounds, Vec3 origin, Vec3 direction, float& distance)
{
    float low  = 0.0f;

    float high = std::numeric_limits<float>::max();

    bool hit   = true;

    for (int axis = 0; axis < 3; ++axis)
    {
        if (std::abs(direction[axis]) < 1e-8f)
        {
            hit = hit && origin[axis] >= bounds.GetMin()[axis] && origin[axis] <= bounds.GetMax()[axis];
        }
        else
        {
            const float first  = (bounds.GetMin()[axis] - origin[axis]) / direction[axis];

            const float second = (bounds.GetMax()[axis] - origin[axis]) / direction[axis];

            low                = std::max(low, std::min(first, second));

            high               = std::min(high, std::max(first, second));
        }
    }

    distance = low;

    return hit && high >= low;
}

const HeapVector<NodeId>& GetEmptyNodeList()
{
    static const HeapVector<NodeId> empty;

    return empty;
}
} // namespace

UniquePtr<LoadedScene> ParseScene(const std::string& path, std::string& error)
{
    UniquePtr<LoadedScene> candidate = MakeUnique<LoadedScene>();

    candidate->scene                 = MakeUnique<sg::Scene>();

    asset::FastGLTFLoader loader;

    if (loader.LoadFromFile(path, candidate->scene.Get()))
    {
        candidate->vertices = HeapVector<asset::Vertex>(loader.GetVertices().data(), loader.GetVertices().size());

        candidate->indices  = HeapVector<uint32_t>(loader.GetIndices().data(), loader.GetIndices().size());

        candidate->path     = path;

        const float extent  = candidate->scene->GetAABB().GetMaxExtent();

        if (std::isfinite(extent) && extent > 1e-6f)
        {
            candidate->normalizationCenter = candidate->scene->GetAABB().GetCenter();

            candidate->normalizationScale  = 1.0f / extent;
        }

        error.clear();
    }
    else
    {
        error = loader.GetError();

        candidate.Reset();
    }

    return candidate;
}

void EditorScene::Replace(UniquePtr<LoadedScene> scene)
{
    m_scene = std::move(scene);

    ++m_generation;

    m_nodes.clear();

    m_children.clear();

    m_roots.clear();

    if (m_scene && m_scene->scene)
    {
        for (const UniquePtr<sg::Node>& node : m_scene->scene->GetNodes())
        {
            m_nodes[node->GetIndex()] = node.Get();
        }

        for (const UniquePtr<sg::Node>& node : m_scene->scene->GetNodes())
        {
            const NodeId id{m_generation, node->GetIndex()};

            if (node->GetParent() != nullptr && m_nodes.count(node->GetParent()->GetIndex()) != 0)
            {
                m_children[node->GetParent()->GetIndex()].push_back(id);
            }
            else
            {
                m_roots.push_back(id);
            }
        }
    }

    m_assets.Build(m_scene ? m_scene->scene.Get() : nullptr, m_generation);
}

const LoadedScene* EditorScene::Get() const
{
    return m_scene.Get();
}

uint64_t EditorScene::GetGeneration() const
{
    return m_generation;
}

const sg::Node* EditorScene::Resolve(NodeId id) const
{
    const HashMap<uint32_t, const sg::Node*>::const_iterator found = m_nodes.find(id.index);

    return id.generation == m_generation && found != m_nodes.end() ? found->second : nullptr;
}

std::string EditorScene::GetNodeDisplayName(NodeId id) const
{
    const sg::Node* node = Resolve(id);

    std::string name     = node != nullptr ? node->GetName() : "Missing Node";

    if (node != nullptr && name.empty())
    {
        if (node->HasComponent<sg::Mesh>())
        {
            name = m_assets.Describe(m_assets.FindMesh(node->GetComponent<sg::Mesh>())).name;
        }
        else if (node->HasComponent<sg::SceneCamera>())
        {
            const sg::SceneCamera& camera = *node->GetComponent<sg::SceneCamera>();

            name                          = camera.GetName();

            if (name.empty())
            {
                name = camera.orthographic ? "Orthographic Camera" : "Perspective Camera";
            }
        }
        else if (node->HasComponent<sg::Light>())
        {
            const sg::Light& light = *node->GetComponent<sg::Light>();

            name                   = light.GetName();

            if (name.empty())
            {
                switch (light.GetType())
                {
                    case sg::Directional: name = "Directional Light"; break;
                    case sg::Point: name = "Point Light"; break;
                    case sg::Spot: name = "Spot Light"; break;
                    default: name = "Light"; break;
                }
            }
        }

        const std::string index = std::to_string(id.index);

        name = name.empty() ? (GetChildren(id).empty() ? "Node " : "Group ") + index : name + " (Node " + index + ")";
    }

    return name;
}

const HeapVector<NodeId>& EditorScene::GetRoots() const
{
    return m_roots;
}

const HeapVector<NodeId>& EditorScene::GetChildren(NodeId id) const
{
    const HashMap<uint32_t, HeapVector<NodeId>>::const_iterator found = m_children.find(id.index);

    return id.generation == m_generation && found != m_children.end() ? found->second : GetEmptyNodeList();
}

HashMap<uint32_t, bool> EditorScene::FilterHierarchy(const std::string& search) const
{
    HashMap<uint32_t, bool> included;

    for (const std::pair<const uint32_t, const sg::Node*>& entry : m_nodes)
    {
        if (MatchesSearch(GetNodeDisplayName({m_generation, entry.first}), search))
        {
            const sg::Node* ancestor = entry.second;

            while (ancestor != nullptr && included.count(ancestor->GetIndex()) == 0)
            {
                included[ancestor->GetIndex()] = true;

                ancestor                       = ancestor->GetParent();
            }
        }
    }

    return included;
}

size_t EditorScene::GetNodeCount() const
{
    return m_nodes.size();
}

NodeInspection EditorScene::Inspect(NodeId id) const
{
    const sg::Node* node  = Resolve(id);

    NodeInspection result = node != nullptr ? InspectNode(*node, id) : NodeInspection{};

    if (result.hasMesh)
    {
        result.meshAsset = m_assets.FindMesh(node->GetComponent<sg::Mesh>());
    }

    return result;
}

bool EditorScene::GetBounds(NodeId id, sg::AABB& bounds) const
{
    const sg::Node* selected = Resolve(id);

    bool valid               = false;

    bounds                   = {};

    HeapVector<NodeId> pending;

    if (selected != nullptr)
    {
        pending.push_back(id);
    }

    while (!pending.empty())
    {
        const NodeId current = pending.back();

        pending.pop_back();

        const sg::Node* node = Resolve(current);

        if (node->HasComponent<sg::Mesh>())
        {
            sg::AABB nodeBounds = node->GetComponent<sg::Mesh>()->GetAABB();

            if (node->HasComponent<sg::Transform>() && !node->deformationInWorldSpace)
            {
                nodeBounds.Transform(node->GetComponent<sg::Transform>()->GetWorldMatrix());
            }

            bounds.SetMin(nodeBounds.GetMin());

            bounds.SetMax(nodeBounds.GetMax());

            valid = true;
        }

        for (const NodeId child : GetChildren(current))
        {
            pending.push_back(child);
        }
    }

    if (!valid && selected != nullptr && selected->HasComponent<sg::Transform>())
    {
        const Vec3 position(selected->GetComponent<sg::Transform>()->GetWorldMatrix()[3]);

        bounds = sg::AABB(position - Vec3(0.025f), position + Vec3(0.025f));

        valid  = true;
    }

    return valid;
}

bool EditorScene::GetSceneBounds(sg::AABB& bounds) const
{
    const bool valid = m_scene && m_scene->scene;

    bounds           = valid ? m_scene->scene->GetAABB() : sg::AABB{};

    return valid;
}

bool EditorScene::GetMeshBounds(SceneAssetId mesh, sg::AABB& bounds) const
{
    const sg::Mesh* source = m_assets.ResolveMesh(mesh);

    // Deformation updates these bounds together with the posed render vertices. The
    // imported vertex snapshot predates render-scene normalization for skinned meshes.
    bounds = source != nullptr ? source->GetAABB() : sg::AABB{};

    const bool valid =
        source != nullptr && source->GetNumIndices() > 0 && glm::all(glm::lessThanEqual(bounds.GetMin(), bounds.GetMax()));

    return valid;
}

NodeId EditorScene::PickBounds(Vec3 origin, Vec3 direction) const
{
    NodeId nearest;

    float distance = std::numeric_limits<float>::max();

    for (const std::pair<const uint32_t, const sg::Node*>& entry : m_nodes)
    {
        const sg::Node& node = *entry.second;

        if (node.IsVisible() && node.selectable && node.HasComponent<sg::Mesh>())
        {
            sg::AABB bounds;

            float current = 0;

            const NodeId id{m_generation, entry.first};

            // Ties keep the lower node index, independent of hash-map iteration order.
            if (GetBounds(id, bounds) && RayBounds(bounds, origin, direction, current)
                && (current < distance || (current == distance && id.index < nearest.index)))
            {
                distance = current;

                nearest  = id;
            }
        }
    }

    return nearest;
}

const SceneAssetIndex& EditorScene::GetAssets() const
{
    return m_assets;
}
} // namespace zen::editor
