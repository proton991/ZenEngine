#pragma once
#include "Templates/HeapVector.h"
#include <string>
#include "Templates/HashMap.h"
#include "Math/Math.h"
#include "Component.h"
#include <cmath>
#include <algorithm>
#include <limits>

namespace zen::sg
{
struct NodeData
{
    Mat4 modelMatrix{1.0f};

    Mat4 normalMatrix{1.0f};

    // Physical thickness scaling remains available after skinning bakes world-space vertices.
    Vec4 surfaceScale{1.0f};
};

static_assert(sizeof(NodeData) == 144);

class Node
{
public:
    explicit Node(uint32_t index, std::string name) : m_index(index), m_name(std::move(name)) {}

    void AddComponent(Component* pComponent)
    {
        const HashMap<TypeId, Component*>::iterator it = m_components.find(pComponent->GetTypeId());
        if (it != m_components.end())
        {
            it->second = pComponent;
        }
        else
        {
            m_components.insert({pComponent->GetTypeId(), pComponent});
        }
    }

    int32_t skinIndex{-1};

    bool deformationInWorldSpace{false};

    bool visible{true};

    bool selectable{true};

    bool hoverable{true};

    HeapVector<float> morphWeights;

    // GPU-instanced children share the authored source node's animated weight state.
    int32_t morphWeightsSourceNode{-1};

    template <class T> bool HasComponent() const
    {
        return m_components.count(typeid(T)) > 0;
    }

    template <class T> inline T* GetComponent() const
    {
        return dynamic_cast<T*>(m_components.at(typeid(T)));
    }

    void AddChild(Node* pChild)
    {
        m_children.push_back(pChild);
    }

    void SetParent(Node* pNode)
    {
        m_pParent = pNode;
    }

    Node* GetParent() const
    {
        return m_pParent;
    }

    bool IsVisible() const
    {
        bool result = true;

        const Node* node = this;

        while (node != nullptr && result)
        {
            result &= node->visible;

            node = node->GetParent();
        }

        return result;
    }

    const std::string& GetName() const
    {
        return m_name;
    }

    uint32_t GetIndex() const
    {
        return m_index;
    }

    uint32_t GetRenderableIndex() const
    {
        return m_renderableIndex;
    }

    uint64_t GetHash() const
    {
        return *(reinterpret_cast<const uint64_t*>(this));
    }

    void SetData(uint32_t renderableIndex, const Mat4& modelMatrix)
    {
        m_data.modelMatrix = modelMatrix;
        // pre-calculate normal transform matrix
        const float determinant = glm::determinant(m_data.modelMatrix);

        m_data.normalMatrix = std::isfinite(determinant) && std::abs(determinant) > 1e-20f ?
            glm::transpose(glm::inverse(m_data.modelMatrix)) :
            Mat4(1.0f);
        m_renderableIndex   = renderableIndex;

        SetSurfaceScale(modelMatrix);

        // Facing follows the render transform, including identity for world-space skinning.
        const double orientation = glm::determinant(glm::dmat3(modelMatrix));

        m_data.surfaceScale.w = orientation < 0.0 ? -1.0f : 1.0f;
    }

    void SetSurfaceScale(const Mat4& matrix)
    {
        for (uint32_t axis = 0; axis < 3; ++axis)
        {
            const double scale = glm::length(glm::dvec3(matrix[axis]));

            m_data.surfaceScale[axis] = static_cast<float>(
                std::min(scale, static_cast<double>(std::numeric_limits<float>::max())));
        }
    }

    void SetData(const NodeData& data)
    {
        m_data = data;
    }

    const NodeData& GetData() const
    {
        return m_data;
    }

private:
    uint32_t m_index{0};

    uint32_t m_renderableIndex{0};

    std::string m_name;

    Node* m_pParent{nullptr};

    HeapVector<Node*> m_children;

    // One unique instance for each Component
    HashMap<TypeId, Component*> m_components;

    // Used for renderer's uniform buffer
    NodeData m_data{};
};
} // namespace zen::sg
