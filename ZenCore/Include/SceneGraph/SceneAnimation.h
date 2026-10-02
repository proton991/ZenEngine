#pragma once

#include "AssetLib/Types.h"
#include "SceneAsset.h"
#include "Templates/VectorView.h"

namespace zen::sg
{
class Scene;

class Node;

bool SampleAnimationSampler(const AnimationSampler& sampler,
                            float                   time,
                            uint32_t                dimensions,
                            bool                    quaternion,
                            HeapVector<float>&      values);

float GetAnimationDuration(const AnimationAsset& animation);

uint32_t GetNodeMorphTargetCount(const Scene& scene, const Node& node);

bool EvaluateSceneAnimation(Scene& scene, uint32_t animation, float time, bool loop = true);

// Morphs precede skinning; posed vertices and UVs are shared by raster, shadows and voxels.
bool ApplySceneDeformations(Scene& scene, VectorView<const asset::Vertex> bindVertices, HeapVector<asset::Vertex>& vertices);
} // namespace zen::sg
