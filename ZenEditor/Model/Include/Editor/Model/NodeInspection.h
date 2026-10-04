#pragma once
#include "Editor/Model/EditorIds.h"
#include "SceneGraph/Scene.h"

namespace zen::editor
{
struct MaterialInspection
{
    std::string   name;
    uint32_t      index{0};
    Vec4          baseColor{1.0f};
    float         metallic{0};
    float         roughness{1};
    sg::AlphaMode alphaMode{sg::AlphaMode::Opaque};
};

// Typed read-only values of one node; frontends choose wording and units.
struct NodeInspection
{
    bool        valid{false};
    std::string name;
    NodeId      id;
    bool        visible{false};
    bool        hasTransform{false};
    Vec3        translation{0.0f};
    Vec3        rotationDegrees{0.0f};
    Vec3        scale{1.0f};
    Mat4        baseMatrix{1.0f};
    Mat4        worldMatrix{1.0f};
    bool        hasMesh{false};
    // Filled by EditorScene::Inspect, which knows the scene's asset list.
    SceneAssetId                   meshAsset;
    uint32_t                       primitives{0};
    uint32_t                       indices{0};
    bool                           skinned{false};
    HeapVector<MaterialInspection> materials;
    bool                           hasLight{false};
    sg::LightType                  lightType{sg::Point};
    sg::LightProperties            light;
    bool                           hasCamera{false};
    bool                           orthographic{false};
    float                          fov{0};
    float                          nearPlane{0};
    float                          farPlane{0};
};

NodeInspection InspectNode(const sg::Node& node, NodeId id);
} // namespace zen::editor
