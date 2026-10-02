#include "SceneGraph/SceneAnimation.h"
#include "SceneGraph/SceneAnimationPointer.h"
#include "SceneGraph/Scene.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace zen::sg
{
static bool FiniteMatrix(const Mat4& matrix)
{
    bool finite = true;

    for (uint32_t column = 0; column < 4; ++column)
    {
        for (uint32_t row = 0; row < 4; ++row)
        {
            finite &= std::isfinite(matrix[column][row]);
        }
    }

    return finite;
}

static Vec3 UnitVector(const Vec3& vector, const Vec3& fallback)
{
    const float squared = glm::dot(vector, vector);

    const Vec3 result   = std::isfinite(squared) && squared > 1e-20f ? vector / std::sqrt(squared) : fallback;

    return result;
}

static Node* FindNode(Scene& scene, uint32_t index)
{
    Node* result = nullptr;

    for (const UniquePtr<Node>& node : scene.GetNodes())
    {
        if (node->GetIndex() == index)
        {
            result = node.Get();

            break;
        }
    }

    return result;
}

bool SampleAnimationSampler(const AnimationSampler& sampler,
                            float                   time,
                            uint32_t                dimensions,
                            bool                    quaternion,
                            HeapVector<float>&      values)
{
    const size_t keys    = sampler.times.size();

    const size_t triples = sampler.interpolation == AnimationInterpolation::CubicSpline ? 3 : 1;

    bool valid           = std::isfinite(time) && dimensions > 0 && keys > 0
              && dimensions <= std::numeric_limits<size_t>::max() / triples / std::max(keys, size_t(1))
              && sampler.values.size() == keys * dimensions * triples && (!quaternion || dimensions == 4);

    for (size_t key = 0; valid && key < keys; ++key)
    {
        valid &= std::isfinite(sampler.times[key]) && sampler.times[key] >= 0.0f
              && (key == 0 || sampler.times[key] > sampler.times[key - 1]);
    }

    for (float value : sampler.values)
    {
        valid &= std::isfinite(value);
    }

    if (valid)
    {
        size_t left = 0;

        while (left + 1 < keys && time >= sampler.times[left + 1])
        {
            ++left;
        }

        const size_t right     = time <= sampler.times.front() ? 0 : std::min(left + 1, keys - 1);

        left                   = time <= sampler.times.front() ? 0 : left;

        const float duration   = sampler.times[right] - sampler.times[left];

        const float blend      = duration > 0.0f ? std::clamp((time - sampler.times[left]) / duration, 0.0f, 1.0f) : 0.0f;

        const size_t valuePart = triples == 3 ? 1 : 0;

        const size_t first     = (left * triples + valuePart) * dimensions;

        const size_t second    = (right * triples + valuePart) * dimensions;

        HeapVector<float> sampled(dimensions);

        for (uint32_t component = 0; component < dimensions; ++component)
        {
            const float a = sampler.values[first + component];

            const float b = sampler.values[second + component];

            float result  = a;

            if (sampler.interpolation == AnimationInterpolation::Linear)
            {
                result = glm::mix(a, b, blend);
            }
            else if (sampler.interpolation == AnimationInterpolation::CubicSpline && left != right)
            {
                const float t2         = blend * blend;

                const float t3         = t2 * blend;

                const float outTangent = sampler.values[(left * 3 + 2) * dimensions + component];

                const float inTangent  = sampler.values[right * 3 * dimensions + component];

                result = (2 * t3 - 3 * t2 + 1) * a + (t3 - 2 * t2 + blend) * duration * outTangent + (-2 * t3 + 3 * t2) * b
                       + (t3 - t2) * duration * inTangent;
            }

            sampled[component] = result;
        }

        if (quaternion)
        {
            Quat rotation(sampled[3], sampled[0], sampled[1], sampled[2]);

            if (sampler.interpolation == AnimationInterpolation::Linear && left != right)
            {
                const Quat a(sampler.values[first + 3], sampler.values[first], sampler.values[first + 1],
                             sampler.values[first + 2]);

                const Quat b(sampler.values[second + 3], sampler.values[second], sampler.values[second + 1],
                             sampler.values[second + 2]);

                valid &= glm::dot(a, a) > 1e-20f && glm::dot(b, b) > 1e-20f;

                if (valid)
                {
                    rotation = glm::slerp(glm::normalize(a), glm::normalize(b), blend);
                }
            }

            const float squared  = glm::dot(rotation, rotation);

            valid               &= std::isfinite(squared) && squared > 1e-20f;

            if (valid)
            {
                rotation   = glm::normalize(rotation);

                sampled[0] = rotation.x;

                sampled[1] = rotation.y;

                sampled[2] = rotation.z;

                sampled[3] = rotation.w;
            }
        }

        for (float value : sampled)
        {
            valid &= std::isfinite(value);
        }

        if (valid)
        {
            values = std::move(sampled);
        }
    }

    return valid;
}

float GetAnimationDuration(const AnimationAsset& animation)
{
    float duration = 0.0f;

    for (const AnimationSampler& sampler : animation.samplers)
    {
        if (!sampler.times.empty() && std::isfinite(sampler.times.back()))
        {
            duration = std::max(duration, sampler.times.back());
        }
    }

    return duration;
}

uint32_t GetNodeMorphTargetCount(const Scene& scene, const Node& node)
{
    uint32_t count = static_cast<uint32_t>(node.morphWeights.size());

    for (const DeformationPrimitiveAsset& binding : scene.GetAssetData().deformations)
    {
        if (binding.node == node.GetIndex() && binding.morphPrimitive >= 0
            && static_cast<size_t>(binding.morphPrimitive) < scene.GetAssetData().morphPrimitives.size())
        {
            count = static_cast<uint32_t>(scene.GetAssetData().morphPrimitives[binding.morphPrimitive].targets.size());

            break;
        }
    }

    if (count == 0 && node.HasComponent<Mesh>() && !node.GetComponent<Mesh>()->GetSubMeshes().empty())
    {
        const uint32_t mesh = node.GetComponent<Mesh>()->GetSubMeshes().front()->assetMesh;

        for (const MorphPrimitiveAsset& morph : scene.GetAssetData().morphPrimitives)
        {
            if (morph.mesh == mesh)
            {
                count = std::max(count, static_cast<uint32_t>(morph.targets.size()));
            }
        }
    }

    return count;
}

bool EvaluateSceneAnimation(Scene& scene, uint32_t animationIndex, float time, bool loop)
{
    const SceneAssetData& data = scene.GetAssetData();

    bool valid                 = animationIndex < data.animations.size() && std::isfinite(time) && time >= 0.0f;

    if (valid)
    {
        const AnimationAsset& animation = data.animations[animationIndex];

        const float duration            = GetAnimationDuration(animation);

        const float sampleTime          = loop && duration > 0.0f ? std::fmod(time, duration) : time;

        // Validate every channel before publishing a new pose.
        HeapVector<HeapVector<float>> samples(animation.channels.size());

        HeapVector<Node*> nodes(animation.channels.size(), nullptr);

        for (size_t index = 0; valid && index < animation.channels.size(); ++index)
        {
            const AnimationChannel& channel = animation.channels[index];

            nodes[index]                    = FindNode(scene, channel.node);

            if (channel.path == AnimationPath::Pointer)
            {
                valid &= channel.sampler < animation.samplers.size();

                if (valid)
                {
                    const AnimationSampler& sampler  = animation.samplers[channel.sampler];

                    const size_t triples             = sampler.interpolation == AnimationInterpolation::CubicSpline ? 3 : 1;

                    const size_t count               = sampler.times.size() * triples;

                    const size_t dimensions          = count > 0 ? sampler.values.size() / count : 0;

                    valid                           &= dimensions > 0 && dimensions <= UINT32_MAX
                          && SampleAnimationSampler(
                                 sampler, sampleTime, static_cast<uint32_t>(dimensions),
                                 dimensions == 4
                                     && (channel.pointer.starts_with("/nodes/")
                                         || channel.pointer.starts_with("/extensions/EXT_lights_image_based/lights/"))
                                     && channel.pointer.ends_with("/rotation"),
                                 samples[index])
                          && ValidateAnimationPointer(scene, channel.pointer, MakeVecView(samples[index]));
                }
            }
            else if (nodes[index] != nullptr)
            {
                const uint32_t dimensions  = channel.path == AnimationPath::Weights
                                               ? GetNodeMorphTargetCount(scene, *nodes[index])
                                           : channel.path == AnimationPath::Rotation ? 4
                                                                                     : 3;

                valid                     &= channel.sampler < animation.samplers.size();

                if (valid)
                {
                    valid &= SampleAnimationSampler(animation.samplers[channel.sampler], sampleTime, dimensions,
                                                    channel.path == AnimationPath::Rotation, samples[index]);
                }
            }
        }

        if (valid)
        {
            for (size_t index = 0; index < animation.channels.size(); ++index)
            {
                Node* node                      = nodes[index];

                const AnimationChannel& channel = animation.channels[index];

                if (channel.path == AnimationPath::Pointer)
                {
                    valid &= ApplyAnimationPointer(scene, channel.pointer, MakeVecView(samples[index]));
                }
                else if (node != nullptr)
                {
                    const HeapVector<float>& sample = samples[index];

                    if (channel.path == AnimationPath::Weights)
                    {
                        node->morphWeights = sample;
                    }
                    else if (node->HasComponent<Transform>())
                    {
                        Transform* transform = node->GetComponent<Transform>();

                        switch (channel.path)
                        {
                            case AnimationPath::Translation:
                                transform->SetTranslation(Vec3(sample[0], sample[1], sample[2]));

                                break;

                            case AnimationPath::Rotation:
                                transform->SetRotation(Quat(sample[3], sample[0], sample[1], sample[2]));

                                break;

                            case AnimationPath::Scale: transform->SetScale(Vec3(sample[0], sample[1], sample[2])); break;

                            default: break;
                        }
                    }
                }
            }

            for (const UniquePtr<Node>& node : scene.GetNodes())
            {
                if (node->HasComponent<Transform>())
                {
                    const Mat4 world = node->GetComponent<Transform>()->GetWorldMatrix();

                    node->SetData(node->GetRenderableIndex(), world);

                    if (node->HasComponent<SceneCamera>())
                    {
                        node->GetComponent<SceneCamera>()->worldMatrix = world;
                    }

                    if (node->HasComponent<Light>())
                    {
                        Light* light               = node->GetComponent<Light>();

                        LightProperties properties = light->GetProperties();

                        properties.position        = Vec3(world[3]);

                        properties.direction       = Vec4(UnitVector(-Vec3(world[2]), Vec3(0, 0, -1)), 0);

                        light->SetProperties(properties);
                    }
                }
            }
        }
    }

    return valid;
}

static void MorphVertex(const MorphPrimitiveAsset& morph,
                        const Node&                node,
                        uint32_t                   source,
                        asset::Vertex&             vertex,
                        HeapVector<Vec2>&          coordinates,
                        bool                       flatNormals)
{
    for (size_t targetIndex = 0; targetIndex < morph.targets.size(); ++targetIndex)
    {
        const float weight             = targetIndex < node.morphWeights.size() ? node.morphWeights[targetIndex]
                                       : targetIndex < morph.weights.size()     ? morph.weights[targetIndex]
                                                                                : 0.0f;

        const MorphTargetAsset& target = morph.targets[targetIndex];

        if (source < target.positions.size())
        {
            vertex.pos += Vec4(target.positions[source] * weight, 0.0f);
        }

        if (source < target.normals.size())
        {
            vertex.normal += Vec4(target.normals[source] * weight, 0.0f);
        }

        if (!flatNormals && source < target.tangents.size())
        {
            vertex.tangent += Vec4(target.tangents[source] * weight, 0.0f);
        }

        if (source < target.colors.size())
        {
            vertex.color += target.colors[source] * weight;
        }

        for (size_t set = 0; set < target.texCoords.size(); ++set)
        {
            if (source < target.texCoords[set].size())
            {
                if (coordinates.size() <= set)
                {
                    coordinates.resize(set + 1);
                }

                coordinates[set] += target.texCoords[set][source] * weight;
            }
        }
    }

    if (!coordinates.empty())
    {
        vertex.uv0 = coordinates[0];
    }

    if (coordinates.size() > 1)
    {
        vertex.uv1 = coordinates[1];
    }

    vertex.color = glm::clamp(vertex.color, Vec4(0.0f), Vec4(1.0f));
}

static HeapVector<Mat4> SkinPalette(Scene& scene, const SkinAsset& skin)
{
    HeapVector<Mat4> palette(skin.joints.size(), Mat4(1.0f));

    for (size_t joint = 0; joint < skin.joints.size(); ++joint)
    {
        Node* node = FindNode(scene, skin.joints[joint]);

        if (node != nullptr && node->HasComponent<Transform>())
        {
            palette[joint] = node->GetComponent<Transform>()->GetWorldMatrix()
                           * (joint < skin.inverseBindMatrices.size() ? skin.inverseBindMatrices[joint] : Mat4(1));
        }
    }

    return palette;
}

static bool SkinVertex(const HeapVector<Mat4>&                 palette,
                       const HeapVector<VertexJointInfluence>& influences,
                       asset::Vertex&                          vertex)
{
    Mat4 matrix(0.0f);

    float total = 0.0f;

    bool valid  = true;

    for (const VertexJointInfluence& influence : influences)
    {
        valid &= influence.joint < palette.size() && std::isfinite(influence.weight) && influence.weight >= 0;

        if (valid && influence.weight > 0)
        {
            matrix += palette[influence.joint] * influence.weight;

            total  += influence.weight;
        }
    }

    valid &= std::isfinite(total) && total > 1e-20f;

    if (valid)
    {
        matrix /= total;

        valid  &= FiniteMatrix(matrix);

        if (valid)
        {
            vertex.pos              = matrix * Vec4(Vec3(vertex.pos), 1.0f);

            const float determinant = glm::determinant(matrix);

            const Mat3 normal  = std::abs(determinant) > 1e-20f ? Mat3(glm::transpose(glm::inverse(matrix))) : Mat3(matrix);

            vertex.normal      = Vec4(UnitVector(normal * Vec3(vertex.normal), Vec3(0, 1, 0)), 0);

            const Vec3 tangent = UnitVector(Mat3(matrix) * Vec3(vertex.tangent), Vec3(0));

            vertex.tangent     = Vec4(tangent, vertex.tangent.w * (determinant < 0 ? -1.0f : 1.0f));
        }
    }

    return valid;
}

bool ApplySceneDeformations(Scene& scene, VectorView<const asset::Vertex> bindVertices, HeapVector<asset::Vertex>& vertices)
{
    HeapVector<asset::Vertex> posed(bindVertices.data(), bindVertices.size());

    const HeapVector<HeapVector<Vec2>>& authoredCoordinates = scene.GetAssetData().bindVertexTexCoords.empty()
                                                                ? scene.GetAssetData().vertexTexCoords
                                                                : scene.GetAssetData().bindVertexTexCoords;

    HeapVector<HeapVector<Vec2>> coordinates                = authoredCoordinates;

    if (coordinates.size() < posed.size())
    {
        const size_t first = coordinates.size();

        coordinates.resize(posed.size());

        for (size_t index = first; index < posed.size(); ++index)
        {
            coordinates[index] = {posed[index].uv0, posed[index].uv1};
        }
    }

    bool valid = true;

    for (const DeformationPrimitiveAsset& binding : scene.GetAssetData().deformations)
    {
        Node* node         = FindNode(scene, binding.node);

        Node* morphSource  = node != nullptr && node->morphWeightsSourceNode >= 0
                               ? FindNode(scene, static_cast<uint32_t>(node->morphWeightsSourceNode))
                               : node;

        valid             &= node != nullptr && morphSource != nullptr && binding.firstVertex <= posed.size()
              && binding.vertexCount <= posed.size() - binding.firstVertex;

        if (valid)
        {
            const MorphPrimitiveAsset* morph =
                binding.morphPrimitive >= 0
                        && static_cast<size_t>(binding.morphPrimitive) < scene.GetAssetData().morphPrimitives.size()
                    ? &scene.GetAssetData().morphPrimitives[binding.morphPrimitive]
                    : nullptr;

            const bool skinned = node->skinIndex >= 0 && !binding.influences.empty()
                              && static_cast<size_t>(node->skinIndex) < scene.GetAssetData().skins.size();

            HeapVector<Mat4> palette =
                skinned ? SkinPalette(scene, scene.GetAssetData().skins[node->skinIndex]) : HeapVector<Mat4>();

            if (skinned && node->morphWeightsSourceNode >= 0 && node->HasComponent<Transform>()
                && morphSource->HasComponent<Transform>())
            {
                const Mat4 sourceWorld  = morphSource->GetComponent<Transform>()->GetWorldMatrix();

                const float determinant = glm::determinant(sourceWorld);

                const Mat4 delta        = std::isfinite(determinant) && std::abs(determinant) > 1e-20f
                                            ? node->GetComponent<Transform>()->GetWorldMatrix() * glm::inverse(sourceWorld)
                                            : node->GetComponent<Transform>()->GetPrefixMatrix()
                                           * node->GetComponent<Transform>()->GetLocalMatrix();

                for (Mat4& joint : palette)
                {
                    joint = delta * joint;
                }
            }

            for (uint32_t index = 0; valid && index < binding.vertexCount; ++index)
            {
                asset::Vertex& vertex = posed[binding.firstVertex + index];

                const uint32_t source = index < binding.sourceVertices.size() ? binding.sourceVertices[index] : index;

                if (morph != nullptr)
                {
                    MorphVertex(*morph, *morphSource, source, vertex, coordinates[binding.firstVertex + index],
                                binding.flatNormals);
                }

                if (skinned)
                {
                    valid &= index < binding.influences.size();

                    if (valid)
                    {
                        valid &= SkinVertex(palette, binding.influences[index], vertex);
                    }
                }
                else
                {
                    vertex.normal  = Vec4(UnitVector(Vec3(vertex.normal), Vec3(0, 1, 0)), 0);

                    vertex.tangent = Vec4(UnitVector(Vec3(vertex.tangent), Vec3(0)), vertex.tangent.w);
                }

                valid &= std::isfinite(vertex.pos.x) && std::isfinite(vertex.pos.y) && std::isfinite(vertex.pos.z)
                      && std::isfinite(vertex.color.x) && std::isfinite(vertex.color.y) && std::isfinite(vertex.color.z)
                      && std::isfinite(vertex.color.w);

                for (const Vec2& coordinate : coordinates[binding.firstVertex + index])
                {
                    valid &= std::isfinite(coordinate.x) && std::isfinite(coordinate.y);
                }
            }

            if (valid && binding.flatNormals)
            {
                for (uint32_t index = 0; index + 2 < binding.vertexCount; index += 3)
                {
                    asset::Vertex* triangle = posed.data() + binding.firstVertex + index;

                    const Vec3 normal =
                        UnitVector(glm::cross(Vec3(triangle[1].pos - triangle[0].pos), Vec3(triangle[2].pos - triangle[0].pos)),
                                   Vec3(0, 1, 0));

                    for (uint32_t corner = 0; corner < 3; ++corner)
                    {
                        triangle[corner].normal = Vec4(normal, 0);
                    }
                }
            }
        }
    }

    if (valid)
    {
        vertices                             = std::move(posed);

        scene.GetAssetData().vertexTexCoords = std::move(coordinates);

        for (const DeformationPrimitiveAsset& binding : scene.GetAssetData().deformations)
        {
            Node* node = FindNode(scene, binding.node);

            if (node != nullptr)
            {
                node->deformationInWorldSpace = node->skinIndex >= 0 && !binding.influences.empty();

                if (node->deformationInWorldSpace)
                {
                    node->SetData(node->GetRenderableIndex(), Mat4(1.0f));

                    if (node->HasComponent<Transform>())
                    {
                        node->SetSurfaceScale(node->GetComponent<Transform>()->GetWorldMatrix());
                    }
                }

                if (node->HasComponent<Mesh>())
                {
                    node->GetComponent<Mesh>()->GetAABB() = AABB();
                }
            }
        }

        const zen::HeapVector<SubMesh*> subMeshes = scene.GetComponents<SubMesh>();

        for (const DeformationPrimitiveAsset& binding : scene.GetAssetData().deformations)
        {
            Node* node = FindNode(scene, binding.node);

            if (node != nullptr && node->HasComponent<Mesh>())
            {
                // Deformed instances own private meshes, so bounds never leak between poses.
                AABB bounds;

                for (uint32_t index = 0; index < binding.vertexCount; ++index)
                {
                    const Vec3 position(vertices[binding.firstVertex + index].pos);

                    bounds.SetMin(position);

                    bounds.SetMax(position);
                }

                node->GetComponent<Mesh>()->SetAABB(bounds.GetMin(), bounds.GetMax());

                if (binding.subMesh < subMeshes.size())
                {
                    subMeshes[binding.subMesh]->SetBounds(bounds);
                }
            }
        }

        scene.UpdateAABB();
    }

    return valid;
}
} // namespace zen::sg
