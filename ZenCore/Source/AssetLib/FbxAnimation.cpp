#include "AssetLib/FbxAnimation.h"
#include "AssetLib/AnimatedMaterial.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace zen::asset
{
namespace
{
float Scalar(const ufbx_material_map& map, float fallback)
{
    return map.has_value ? static_cast<float>(map.value_real) : fallback;
}

Vec3 Color(const ufbx_material_map& map, const Vec3& fallback)
{
    return map.has_value ? FbxVector(map.value_vec3) : fallback;
}

constexpr AnimatedProperty MaterialProperties[] = {AnimatedProperty::BaseColor,
                                                   AnimatedProperty::Metallic,
                                                   AnimatedProperty::Roughness,
                                                   AnimatedProperty::Emissive,
                                                   AnimatedProperty::Ior,
                                                   AnimatedProperty::Specular,
                                                   AnimatedProperty::SpecularColor,
                                                   AnimatedProperty::Clearcoat,
                                                   AnimatedProperty::ClearcoatRoughness,
                                                   AnimatedProperty::Transmission,
                                                   AnimatedProperty::SheenColor,
                                                   AnimatedProperty::SheenRoughness,
                                                   AnimatedProperty::Anisotropy,
                                                   AnimatedProperty::AnisotropyRotation};

struct BakeTrack
{
    AnimationTarget target;
    size_t          offset{0};
};

struct BakeFrame
{
    float             time{0};
    HeapVector<float> values;
};

class AnimationBaker
{
public:
    AnimationBaker(const ufbx_scene& source, SceneAsset& asset, const AnimationBakeSettings& settings, ImportErrors& errors) :
        m_source(source), m_asset(asset), m_settings(settings), m_errors(errors)
    {}

    void Run();

private:
    void          Add(AnimatedObject object, uint32_t index, AnimatedProperty property, uint32_t dimensions);
    void          Layout();
    MaterialAsset CaptureMaterial(const ufbx_material& source);
    void          CaptureCamera(const ufbx_camera& camera, AnimatedProperty property, HeapVector<float>& values);
    void          CaptureLight(const ufbx_light& light, AnimatedProperty property, HeapVector<float>& values);
    BakeFrame     Capture(const ufbx_scene& scene, float time);
    BakeFrame     Evaluate(float time);
    void          Times(const ufbx_anim_stack& stack, HeapVector<float>& times);
    void          Append(const BakeFrame& frame);
    void          Refine(const BakeFrame& left, const BakeFrame& right, uint32_t depth);
    bool          Close(const BakeTrack& track, const float* a, const float* b) const;
    bool          Fits(const BakeFrame& left, const BakeFrame& right, const BakeFrame& sample) const;
    void          Finish(const ufbx_anim_stack& stack, const BakeFrame& baseline);

    const ufbx_scene&            m_source;
    SceneAsset&                  m_asset;
    const AnimationBakeSettings& m_settings;
    ImportErrors&                m_errors;
    HeapVector<BakeTrack>        m_tracks;
    HeapVector<bool>             m_activeTracks;
    size_t                       m_dimensions{0};
    const ufbx_anim_stack*       m_stack{nullptr};
    HeapVector<float>            m_times;
    HeapVector<float>            m_values;
    uint64_t                     m_totalValues{0};
};

void AnimationBaker::Add(AnimatedObject object, uint32_t index, AnimatedProperty property, uint32_t dimensions)
{
    if (dimensions > 0)
    {
        m_tracks.push_back({{object, index, property, dimensions}, m_dimensions});
        m_dimensions += dimensions;
    }
}

// A node's local transform also changes without curves of its own when it is a
// helper (geometric-transform or scale-compensation) of an animated parent, or when
// it does not inherit scale normally and the node it takes scale from animates.
// state: 0 unknown, 1 static, 2 animated, per typed node id.
bool NodeAnimated(const ufbx_node& node, const HeapVector<uint8_t>& animated, HeapVector<uint8_t>& state)
{
    if (state[node.typed_id] == 0)
    {
        bool       result = animated[node.element_id] != 0;
        const bool dependent =
            node.is_geometry_transform_helper || node.is_scale_helper || node.original_inherit_mode != UFBX_INHERIT_MODE_NORMAL;
        if (!result && dependent && node.parent != nullptr)
        {
            result = NodeAnimated(*node.parent, animated, state);
        }
        if (!result && dependent && node.inherit_scale_node != nullptr)
        {
            result = NodeAnimated(*node.inherit_scale_node, animated, state);
        }
        state[node.typed_id] = result ? 2 : 1;
    }
    return state[node.typed_id] == 2;
}

bool WeightsAnimated(const ufbx_node& node, const HeapVector<uint8_t>& animated)
{
    bool result = false;
    if (node.mesh != nullptr)
    {
        for (const ufbx_blend_deformer* deformer : node.mesh->blend_deformers)
        {
            for (const ufbx_blend_channel* channel : deformer->channels)
            {
                result |= animated[channel->element.element_id] != 0;
            }
        }
    }
    return result;
}

// Bakes only elements that some stack animates. Constant elements keep their
// authored state from the asset, so the sample budget scales with animated
// content instead of scene size.
void AnimationBaker::Layout()
{
    HeapVector<uint8_t> animated(m_source.elements.count, 0);
    for (const ufbx_anim_layer* layer : m_source.anim_layers)
    {
        for (const ufbx_anim_prop& property : layer->anim_props)
        {
            animated[property.element->element_id] = 1;
        }
    }
    HeapVector<uint8_t> nodeState(m_source.nodes.count, 0);
    for (const ufbx_node* node : m_source.nodes)
    {
        if (NodeAnimated(*node, animated, nodeState))
        {
            Add(AnimatedObject::Node, node->typed_id, AnimatedProperty::Translation, 3);
            Add(AnimatedObject::Node, node->typed_id, AnimatedProperty::Rotation, 4);
            Add(AnimatedObject::Node, node->typed_id, AnimatedProperty::Scale, 3);
            Add(AnimatedObject::Node, node->typed_id, AnimatedProperty::Visible, 1);
        }
        if (WeightsAnimated(*node, animated))
        {
            Add(AnimatedObject::Node, node->typed_id, AnimatedProperty::Weights,
                static_cast<uint32_t>(m_asset.meshes[node->mesh->typed_id].morphWeights.size()));
        }
    }
    for (const ufbx_material* material : m_source.materials)
    {
        for (AnimatedProperty property : MaterialProperties)
        {
            if (animated[material->element.element_id] != 0)
            {
                Add(AnimatedObject::Material, material->typed_id, property,
                    static_cast<uint32_t>(GetAnimatedMaterialProperty(m_asset.materials[material->typed_id], property).size()));
            }
        }
    }
    for (const ufbx_camera* camera : m_source.cameras)
    {
        const AnimatedProperty properties[] = {AnimatedProperty::CameraAspect, AnimatedProperty::CameraFov,
                                               AnimatedProperty::CameraNear,   AnimatedProperty::CameraFar,
                                               AnimatedProperty::CameraXMag,   AnimatedProperty::CameraYMag};
        for (AnimatedProperty property : properties)
        {
            if (animated[camera->element.element_id] != 0)
            {
                Add(AnimatedObject::Camera, camera->typed_id, property, 1);
            }
        }
    }
    for (const ufbx_light* light : m_source.lights)
    {
        if (animated[light->element.element_id] != 0)
        {
            Add(AnimatedObject::Light, light->typed_id, AnimatedProperty::LightColor, 3);
            Add(AnimatedObject::Light, light->typed_id, AnimatedProperty::LightIntensity, 1);
            Add(AnimatedObject::Light, light->typed_id, AnimatedProperty::LightInnerCone, 1);
            Add(AnimatedObject::Light, light->typed_id, AnimatedProperty::LightOuterCone, 1);
        }
    }
    if (m_dimensions > m_settings.maxValues / 8)
    {
        m_errors.Fail("FBX animation pose exceeds the bake value budget", SceneImportStatus::ResourceLimit);
    }
}

void PushVector(HeapVector<float>& values, const ufbx_vec3& vector)
{
    values.push_back(static_cast<float>(vector.x));
    values.push_back(static_cast<float>(vector.y));
    values.push_back(static_cast<float>(vector.z));
}

void CaptureNode(const ufbx_node& node, AnimatedProperty property, HeapVector<float>& values)
{
    switch (property)
    {
        case AnimatedProperty::Translation: PushVector(values, node.local_transform.translation); break;
        case AnimatedProperty::Rotation:
        {
            const Quat rotation = glm::normalize(FbxQuaternion(node.local_transform.rotation));
            values.push_back(rotation.x);
            values.push_back(rotation.y);
            values.push_back(rotation.z);
            values.push_back(rotation.w);
            break;
        }
        case AnimatedProperty::Scale: PushVector(values, node.local_transform.scale); break;
        case AnimatedProperty::Visible: values.push_back(node.visible ? 1.0f : 0.0f); break;
        default:
            for (const ufbx_blend_deformer* deformer : node.mesh->blend_deformers)
            {
                for (const ufbx_blend_channel* channel : deformer->channels)
                {
                    for (const ufbx_blend_keyframe& key : channel->keyframes)
                    {
                        values.push_back(static_cast<float>(key.effective_weight));
                    }
                }
            }
            break;
    }
}

MaterialAsset AnimationBaker::CaptureMaterial(const ufbx_material& source)
{
    MaterialAsset material;
    SetFbxMaterialFactors(source, material);
    const MaterialAsset& baseline = m_asset.materials[source.typed_id];
    if (material.doubleSided != baseline.doubleSided || material.unlit != baseline.unlit)
    {
        m_errors.Fail("Unsupported animated FBX material shading mode: " + FbxText(source.name),
                      SceneImportStatus::UnsupportedFeature);
    }
    if (source.features.roughness_as_glossiness.enabled && source.pbr.roughness.texture_enabled
        && source.pbr.roughness.texture != nullptr)
    {
        const ufbx_material& original = *m_source.materials.data[source.typed_id];
        if (Scalar(source.pbr.roughness, 1) != Scalar(original.pbr.roughness, 1))
        {
            m_errors.Fail("Unsupported animated FBX gloss factor with a gloss texture: " + FbxText(source.name),
                          SceneImportStatus::UnsupportedFeature);
        }
        // This factor was already baked into the derived roughness pixels.
        material.roughnessFactor = 1;
    }
    if (material.baseColorFactor.a < 1)
    {
        m_asset.materials[source.typed_id].alphaMode = AlphaMode::Blend;
    }
    return material;
}

void AnimationBaker::CaptureCamera(const ufbx_camera& camera, AnimatedProperty property, HeapVector<float>& values)
{
    const bool orthographic = camera.projection_mode == UFBX_PROJECTION_MODE_ORTHOGRAPHIC;
    if (property == AnimatedProperty::CameraAspect)
    {
        if (orthographic != m_asset.cameras[camera.typed_id].orthographic)
        {
            m_errors.Fail("Unsupported animated FBX camera projection mode: " + FbxText(camera.name),
                          SceneImportStatus::UnsupportedFeature);
        }
        if (camera.aspect_ratio <= 0 || camera.near_plane <= 0 || camera.far_plane <= camera.near_plane
            || (!orthographic && (camera.field_of_view_deg.y <= 0 || camera.field_of_view_deg.y >= 180))
            || (orthographic && (camera.orthographic_size.x <= 0 || camera.orthographic_size.y <= 0)))
        {
            m_errors.Fail("Invalid animated FBX camera projection: " + FbxText(camera.name));
        }
    }
    switch (property)
    {
        case AnimatedProperty::CameraAspect: values.push_back(static_cast<float>(camera.aspect_ratio)); break;
        case AnimatedProperty::CameraFov: values.push_back(glm::radians(static_cast<float>(camera.field_of_view_deg.y))); break;
        case AnimatedProperty::CameraNear: values.push_back(static_cast<float>(camera.near_plane)); break;
        case AnimatedProperty::CameraFar: values.push_back(static_cast<float>(camera.far_plane)); break;
        case AnimatedProperty::CameraXMag: values.push_back(static_cast<float>(camera.orthographic_size.x * 0.5)); break;
        default: values.push_back(static_cast<float>(camera.orthographic_size.y * 0.5)); break;
    }
}

void AnimationBaker::CaptureLight(const ufbx_light& light, AnimatedProperty property, HeapVector<float>& values)
{
    if (property == AnimatedProperty::LightColor)
    {
        const ufbx_light& original = *m_source.lights.data[light.typed_id];
        if (light.type != original.type || light.decay != original.decay)
        {
            m_errors.Fail("Unsupported animated FBX light type or decay: " + FbxText(light.name),
                          SceneImportStatus::UnsupportedFeature);
        }
        if (light.intensity < 0
            || (light.type == UFBX_LIGHT_SPOT
                && (light.inner_angle < 0 || light.outer_angle <= 0 || light.inner_angle > light.outer_angle
                    || light.outer_angle > 180)))
        {
            m_errors.Fail("Invalid animated FBX light values: " + FbxText(light.name));
        }
    }
    switch (property)
    {
        case AnimatedProperty::LightColor: PushVector(values, light.color); break;
        case AnimatedProperty::LightIntensity:
            values.push_back(light.cast_light ? static_cast<float>(light.intensity) : 0);
            break;
        case AnimatedProperty::LightInnerCone:
            values.push_back(glm::radians(static_cast<float>(light.inner_angle * 0.5)));
            break;
        default: values.push_back(glm::radians(static_cast<float>(light.outer_angle * 0.5))); break;
    }
}

BakeFrame AnimationBaker::Capture(const ufbx_scene& scene, float time)
{
    BakeFrame frame;
    frame.time = time;
    frame.values.reserve(m_dimensions);
    MaterialAsset material;
    uint32_t      materialIndex = UINT32_MAX;
    for (const BakeTrack& track : m_tracks)
    {
        const uint32_t index = track.target.index;
        switch (track.target.object)
        {
            case AnimatedObject::Node: CaptureNode(*scene.nodes.data[index], track.target.property, frame.values); break;
            case AnimatedObject::Material:
                // A material's tracks are contiguous; evaluate its factors once per frame.
                if (index != materialIndex)
                {
                    material      = CaptureMaterial(*scene.materials.data[index]);
                    materialIndex = index;
                }
                for (float value : GetAnimatedMaterialProperty(material, track.target.property))
                {
                    frame.values.push_back(value);
                }
                break;
            case AnimatedObject::Camera: CaptureCamera(*scene.cameras.data[index], track.target.property, frame.values); break;
            default: CaptureLight(*scene.lights.data[index], track.target.property, frame.values); break;
        }
    }
    if (frame.values.size() != m_dimensions)
    {
        m_errors.Fail("FBX animation changed the scene's property layout");
    }
    for (float value : frame.values)
    {
        if (!std::isfinite(value))
        {
            m_errors.Fail("Non-finite FBX animation sample");
        }
    }
    return frame;
}

BakeFrame AnimationBaker::Evaluate(float time)
{
    BakeFrame frame;
    if (!m_errors.Failed())
    {
        ufbx_evaluate_opts options{};
        options.temp_allocator.memory_limit   = size_t(8) << 30;
        options.result_allocator.memory_limit = size_t(8) << 30;
        ufbx_error  error{};
        ufbx_scene* evaluated = ufbx_evaluate_scene(&m_source, m_stack->anim, m_stack->time_begin + time, &options, &error);
        if (evaluated != nullptr)
        {
            frame = Capture(*evaluated, time);
            ufbx_free_scene(evaluated);
        }
        else
        {
            char description[1024]{};
            ufbx_format_error(description, sizeof(description), &error);
            m_errors.Fail("Could not evaluate FBX animation " + FbxText(m_stack->name) + ": " + description,
                          error.type == UFBX_ERROR_MEMORY_LIMIT ? SceneImportStatus::ResourceLimit
                                                                : SceneImportStatus::InvalidData);
        }
    }
    return frame;
}

void AnimationBaker::Times(const ufbx_anim_stack& stack, HeapVector<float>& times)
{
    const double   duration = stack.time_end - stack.time_begin;
    const double   steps    = std::ceil(duration * m_settings.sampleRate);
    const uint64_t limit    = m_settings.maxValues / std::max(m_dimensions, size_t(1));
    if (!std::isfinite(stack.time_begin) || !std::isfinite(stack.time_end) || duration < 0 || !std::isfinite(steps)
        || steps + 2 > static_cast<double>(limit))
    {
        m_errors.Fail("FBX animation duration exceeds the bake budget: " + FbxText(stack.name),
                      SceneImportStatus::ResourceLimit);
    }
    else
    {
        const float end = static_cast<float>(duration);
        times.push_back(0);
        for (uint64_t index = 1; index < static_cast<uint64_t>(steps); ++index)
        {
            times.push_back(static_cast<float>(static_cast<double>(index) / m_settings.sampleRate));
        }
        times.push_back(end);
        // Preserve keys and both sides of discontinuities in the runtime's float
        // time domain. Adjacent double times would collapse when serialized.
        for (const ufbx_anim_layer* layer : stack.layers)
        {
            for (const ufbx_anim_value* value : layer->anim_values)
            {
                for (const ufbx_anim_curve* curve : value->curves)
                {
                    if (curve != nullptr)
                    {
                        for (const ufbx_keyframe& key : curve->keyframes)
                        {
                            const float time = static_cast<float>(key.time - stack.time_begin);
                            if (time >= 0 && time <= end && !m_errors.Failed())
                            {
                                const float neighbors[] = {std::nextafter(time, -std::numeric_limits<float>::infinity()), time,
                                                           std::nextafter(time, std::numeric_limits<float>::infinity())};
                                for (float neighbor : neighbors)
                                {
                                    if (neighbor >= 0 && neighbor <= end)
                                    {
                                        times.push_back(neighbor);
                                    }
                                }
                                if (times.size() > limit)
                                {
                                    m_errors.Fail("FBX animation keys exceed the bake budget",
                                                  SceneImportStatus::ResourceLimit);
                                }
                            }
                        }
                    }
                }
            }
        }
        std::sort(times.begin(), times.end());
        times.resize(static_cast<size_t>(std::unique(times.begin(), times.end()) - times.begin()));
    }
}

bool AnimationBaker::Close(const BakeTrack& track, const float* a, const float* b) const
{
    bool                   close    = true;
    const AnimatedProperty property = track.target.property;
    if (IsQuaternionTarget(track.target))
    {
        double same     = 0;
        double opposite = 0;
        for (uint32_t component = 0; component < 4; ++component)
        {
            const double difference  = double(a[component]) - b[component];
            const double sum         = double(a[component]) + b[component];
            same                    += difference * difference;
            opposite                += sum * sum;
        }
        // Quaternion chord length: 2*sin(angle/4), treating q and -q equally.
        close = std::sqrt(std::min(same, opposite)) <= 2 * std::sin(double(m_settings.rotationTolerance) * 0.25);
    }
    else
    {
        const float tolerance = property == AnimatedProperty::Translation ? m_settings.translationTolerance
                              : property == AnimatedProperty::Scale       ? m_settings.scaleTolerance
                              : property == AnimatedProperty::Visible     ? 0
                                                                          : m_settings.valueTolerance;
        for (uint32_t component = 0; component < track.target.dimensions; ++component)
        {
            close &= std::abs(double(a[component]) - b[component]) <= tolerance;
        }
    }
    return close;
}

bool AnimationBaker::Fits(const BakeFrame& left, const BakeFrame& right, const BakeFrame& sample) const
{
    const float       blend = (sample.time - left.time) / (right.time - left.time);
    bool              fits  = true;
    HeapVector<float> interpolated;
    for (const BakeTrack& track : m_tracks)
    {
        const float* a        = left.values.data() + track.offset;
        const float* b        = right.values.data() + track.offset;
        const float* expected = sample.values.data() + track.offset;
        interpolated.resize(track.target.dimensions);
        for (uint32_t component = 0; component < track.target.dimensions; ++component)
        {
            interpolated[component] =
                track.target.property == AnimatedProperty::Visible ? a[component] : glm::mix(a[component], b[component], blend);
        }
        if (IsQuaternionTarget(track.target))
        {
            const Quat value = glm::normalize(glm::slerp(Quat(a[3], a[0], a[1], a[2]), Quat(b[3], b[0], b[1], b[2]), blend));
            interpolated[0]  = value.x;
            interpolated[1]  = value.y;
            interpolated[2]  = value.z;
            interpolated[3]  = value.w;
        }
        if (!Close(track, interpolated.data(), expected))
        {
            fits = false;
            break;
        }
    }
    return fits;
}

void AnimationBaker::Append(const BakeFrame& frame)
{
    if (!m_errors.Failed())
    {
        if (m_dimensions > m_settings.maxValues - m_totalValues)
        {
            m_errors.Fail("FBX animation samples exceed the bake budget", SceneImportStatus::ResourceLimit);
        }
        else
        {
            m_times.push_back(frame.time);
            for (float value : frame.values)
            {
                m_values.push_back(value);
            }
            m_totalValues += m_dimensions;
        }
    }
}

void AnimationBaker::Refine(const BakeFrame& left, const BakeFrame& right, uint32_t depth)
{
    bool fits = true;
    // Quarter samples catch curves whose midpoint lies on the endpoint chord.
    for (uint32_t index = 1; !m_errors.Failed() && index < 4; ++index)
    {
        const float time = static_cast<float>(double(left.time) + (double(right.time) - left.time) * (index * 0.25));
        if (time > left.time && time < right.time)
        {
            const BakeFrame sample = Evaluate(time);
            if (!m_errors.Failed())
            {
                fits &= Fits(left, right, sample);
            }
        }
    }
    if (!m_errors.Failed())
    {
        if (fits)
        {
            Append(right);
        }
        else if (depth == m_settings.maxSubdivisions)
        {
            m_errors.Fail("FBX animation cannot meet bake tolerances: " + FbxText(m_stack->name),
                          SceneImportStatus::UnsupportedFeature);
        }
        else
        {
            const BakeFrame middle = Evaluate(static_cast<float>((double(left.time) + right.time) * 0.5));
            if (!m_errors.Failed())
            {
                Refine(left, middle, depth + 1);
                Refine(middle, right, depth + 1);
            }
        }
    }
}

void AnimationBaker::Finish(const ufbx_anim_stack& stack, const BakeFrame& baseline)
{
    AnimationAsset animation;
    animation.name         = FbxText(stack.name);
    animation.baked        = true;
    animation.bakeSettings = m_settings;
    for (size_t index = 0; index < m_tracks.size(); ++index)
    {
        const BakeTrack& track   = m_tracks[index];
        bool             changed = false;
        for (size_t frame = 0; !changed && frame < m_times.size(); ++frame)
        {
            changed =
                !Close(track, baseline.values.data() + track.offset, m_values.data() + frame * m_dimensions + track.offset);
        }
        // Keep constant tracks for properties animated by another clip so
        // switching clips restores their authored state. Prune after all clips.
        m_activeTracks[index] = m_activeTracks[index] || changed || index == 0;
        AnimationSampler sampler;
        sampler.components = track.target.dimensions;
        sampler.interpolation =
            track.target.property == AnimatedProperty::Visible ? AnimationInterpolation::Step : AnimationInterpolation::Linear;
        sampler.times = m_times;
        for (size_t frame = 0; frame < m_times.size(); ++frame)
        {
            const float* values = m_values.data() + frame * m_dimensions + track.offset;
            for (uint32_t component = 0; component < sampler.components; ++component)
            {
                sampler.values.push_back(values[component]);
            }
        }
        AnimationChannel channel;
        channel.sampler = static_cast<uint32_t>(animation.samplers.size());
        channel.target  = track.target;
        channel.node    = track.target.object == AnimatedObject::Node ? track.target.index : 0;
        channel.path    = track.target.property == AnimatedProperty::Translation ? AnimationPath::Translation
                        : track.target.property == AnimatedProperty::Rotation    ? AnimationPath::Rotation
                        : track.target.property == AnimatedProperty::Scale       ? AnimationPath::Scale
                        : track.target.property == AnimatedProperty::Weights     ? AnimationPath::Weights
                                                                                 : AnimationPath::Property;
        animation.samplers.push_back(std::move(sampler));
        animation.channels.push_back(std::move(channel));
    }
    m_asset.animations.push_back(std::move(animation));
}

void AnimationBaker::Run()
{
    const bool valid = std::isfinite(m_settings.sampleRate) && m_settings.sampleRate > 0 && m_settings.sampleRate <= 1000
                    && std::isfinite(m_settings.translationTolerance) && m_settings.translationTolerance > 0
                    && std::isfinite(m_settings.rotationTolerance) && m_settings.rotationTolerance > 0
                    && std::isfinite(m_settings.scaleTolerance) && m_settings.scaleTolerance > 0
                    && std::isfinite(m_settings.valueTolerance) && m_settings.valueTolerance > 0
                    && m_settings.maxSubdivisions <= 20 && m_settings.maxValues > 0;
    if (!valid)
    {
        m_errors.Fail("Invalid FBX animation bake settings", SceneImportStatus::InvalidRequest);
    }
    if (!m_errors.Failed())
    {
        Layout();
    }
    if (!m_errors.Failed())
    {
        const BakeFrame baseline = Capture(m_source, 0);
        m_activeTracks.resize(m_tracks.size());
        for (const ufbx_anim_stack* stack : m_source.anim_stacks)
        {
            if (!m_errors.Failed() && m_tracks.empty())
            {
                // Nothing the engine represents is animated: keep the named clip
                // without evaluating the scene at every sample.
                AnimationAsset animation;
                animation.name         = FbxText(stack->name);
                animation.baked        = true;
                animation.bakeSettings = m_settings;
                m_asset.animations.push_back(std::move(animation));
            }
            else if (!m_errors.Failed())
            {
                m_stack = stack;
                m_times.clear();
                m_values.clear();
                HeapVector<float> times;
                Times(*stack, times);
                if (!m_errors.Failed())
                {
                    BakeFrame left = Evaluate(times.front());
                    Append(left);
                    for (size_t index = 1; !m_errors.Failed() && index < times.size(); ++index)
                    {
                        BakeFrame right = Evaluate(times[index]);
                        if (!m_errors.Failed())
                        {
                            Refine(left, right, 0);
                        }
                        left = std::move(right);
                    }
                    if (!m_errors.Failed())
                    {
                        Finish(*stack, baseline);
                    }
                }
            }
        }
        if (!m_errors.Failed())
        {
            for (AnimationAsset& animation : m_asset.animations)
            {
                HeapVector<AnimationChannel> channels;
                HeapVector<AnimationSampler> samplers;
                for (size_t index = 0; index < m_tracks.size(); ++index)
                {
                    if (m_activeTracks[index])
                    {
                        AnimationChannel channel = std::move(animation.channels[index]);
                        channel.sampler          = static_cast<uint32_t>(samplers.size());
                        channels.push_back(std::move(channel));
                        samplers.push_back(std::move(animation.samplers[index]));
                    }
                }
                animation.channels = std::move(channels);
                animation.samplers = std::move(samplers);
            }
        }
    }
}
} // namespace

void SetFbxMaterialFactors(const ufbx_material& source, MaterialAsset& material)
{
    const ufbx_material_pbr_maps& pbr = source.pbr;
    material.baseColorFactor = Vec4(Color(pbr.base_color, Vec3(1)) * Scalar(pbr.base_factor, 1), Scalar(pbr.opacity, 1));
    if (pbr.base_color.value_components == 4)
    {
        material.baseColorFactor.a *= static_cast<float>(pbr.base_color.value_vec4.w);
    }
    material.metallicFactor  = Scalar(pbr.metalness, pbr.metalness.texture_enabled && pbr.metalness.texture != nullptr ? 1 : 0);
    material.roughnessFactor = Scalar(pbr.roughness, 1);
    if (source.features.roughness_as_glossiness.enabled && !(pbr.roughness.texture_enabled && pbr.roughness.texture != nullptr))
    {
        material.roughnessFactor = 1 - material.roughnessFactor;
    }
    material.emissiveFactor              = Vec4(Color(pbr.emission_color, Vec3(0)) * Scalar(pbr.emission_factor, 1), 0);
    material.doubleSided                 = source.features.double_sided.enabled;
    material.unlit                       = source.features.unlit.enabled;
    material.features.ior                = Scalar(pbr.specular_ior, 1.5f);
    material.features.specular           = Scalar(pbr.specular_factor, 1);
    material.features.specularColor      = Color(pbr.specular_color, Vec3(1));
    material.features.clearcoat          = Scalar(pbr.coat_factor, 0);
    material.features.clearcoatRoughness = Scalar(pbr.coat_roughness, 0);
    material.features.transmission       = Scalar(pbr.transmission_factor, 0);
    material.features.sheenColor         = Color(pbr.sheen_color, Vec3(0)) * Scalar(pbr.sheen_factor, 1);
    material.features.sheenRoughness     = Scalar(pbr.sheen_roughness, 0);
    material.features.anisotropy         = Scalar(pbr.specular_anisotropy, 0);
    material.features.anisotropyRotation = Scalar(pbr.specular_rotation, 0) * glm::two_pi<float>();
}

void BakeFbxAnimations(const ufbx_scene&                  source,
                       SceneAsset&                        asset,
                       const AnimationBakeSettings&       settings,
                       ImportErrors&                      errors,
                       HeapVector<SceneImportDiagnostic>& diagnostics)
{
    if (source.anim_curves.count > 0 && !errors.Failed())
    {
        errors.SetStage(SceneImportStage::BakingAnimations);
        if (source.anim_stacks.count == 0)
        {
            errors.Fail("FBX animation curves have no animation stack", SceneImportStatus::UnsupportedFeature);
        }
        for (const ufbx_anim_layer* layer : source.anim_layers)
        {
            for (const ufbx_anim_prop& property : layer->anim_props)
            {
                const ufbx_element_type type = property.element->type;
                if (type == UFBX_ELEMENT_TEXTURE)
                {
                    errors.Fail("Unsupported FBX animated texture property: " + FbxText(property.prop_name),
                                SceneImportStatus::UnsupportedFeature);
                }
                else if (type != UFBX_ELEMENT_NODE && type != UFBX_ELEMENT_MATERIAL && type != UFBX_ELEMENT_CAMERA
                         && type != UFBX_ELEMENT_LIGHT && type != UFBX_ELEMENT_BLEND_CHANNEL && type != UFBX_ELEMENT_ANIM_LAYER)
                {
                    diagnostics.push_back({SceneDiagnosticSeverity::Warning, SceneImportStatus::UnsupportedFeature,
                                           asset.sourcePath, FbxText(property.element->name),
                                           "Unmapped FBX animated property: " + FbxText(property.prop_name)});
                }
            }
        }
        AnimationBaker baker(source, asset, settings, errors);
        baker.Run();
    }
}
} // namespace zen::asset
