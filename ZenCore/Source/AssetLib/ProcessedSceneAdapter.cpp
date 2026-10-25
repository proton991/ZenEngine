#include "AssetLib/ProcessedSceneAdapter.h"
#include "ProcessedAnimationAdapter.h"
#include "ZenAsset/Format/Scene.h"
#include <algorithm>
#include <type_traits>

namespace zen::asset
{
namespace
{
class SceneAdapter
{
public:
    SceneAdapter(const zenasset::Scene& source, const SceneImportControl* control, SceneImportResult& result) :
        m_source(source), m_control(control), m_result(result)
    {
        m_result.status = SceneImportStatus::Success;
    }

    void Run(uint64_t maxRuntimeGeometryBytes)
    {
        if (Active())
        {
            CheckImages();
        }
        if (Active())
        {
            SharedPtr<SceneAsset, MultiThreadCounter> asset = MakeShared<SceneAsset, MultiThreadCounter>();
            Copy(*asset, m_source);
            if (Active())
            {
                std::string failure;
                if (!ValidateSceneAsset(*asset, maxRuntimeGeometryBytes, &failure))
                {
                    Fail(SceneImportStatus::InvalidData, "Processed scene validation failed: " + failure);
                }
            }
            if (Active())
            {
                m_result.asset = std::move(asset);
            }
        }
    }

    bool ConvertAnimationTarget(const zenasset::AnimationTarget& source, AnimationTarget& target)
    {
        AnimationTarget candidate;
        Copy(candidate, source);
        if (Active())
        {
            target = candidate;
        }
        return m_result.status == SceneImportStatus::Success;
    }

private:
    void Fail(SceneImportStatus status, std::string message, const std::string& object = {})
    {
        if (m_result.status == SceneImportStatus::Success)
        {
            m_result.status = status;
            m_result.error  = std::move(message);
            m_result.diagnostics.push_back(
                {SceneDiagnosticSeverity::Error, status, m_source.sourcePath, object, m_result.error});
        }
    }

    bool Active()
    {
        if (m_control != nullptr && m_control->IsCancelled())
        {
            Fail(SceneImportStatus::Cancelled, "Processed scene adaptation cancelled");
        }
        return m_result.status == SceneImportStatus::Success;
    }

    void CheckImages()
    {
        for (const zenasset::ImageAsset& image : m_source.images)
        {
            if (Active())
            {
                if (!image.pixels)
                {
                    Fail(SceneImportStatus::InvalidData, "Processed image has no pixel owner", image.name);
                }
                else
                {
                    Format format;
                    Copy(format, image.pixels->format);
                }
            }
        }
        for (const zenasset::TextureAsset& texture : m_source.textures)
        {
            if (Active())
            {
                Format format;
                Copy(format, texture.format);
            }
        }
    }

    template <class T> void Copy(T& target, const T& source)
    {
        target = source;
    }

    template <class Destination, class Source> void CopyArray(Destination& target, const std::vector<Source>& source)
    {
        if (Active())
        {
            target.resize(source.size());
            for (size_t first = 0; first < source.size() && Active(); first += 4096)
            {
                const size_t end = first + std::min(size_t(4096), source.size() - first);
                if constexpr (std::is_same_v<typename Destination::value_type, Source> && std::is_trivially_copyable_v<Source>)
                {
                    std::copy(source.data() + first, source.data() + end, target.data() + first);
                }
                else
                {
                    for (size_t index = first; index < end && m_result.status == SceneImportStatus::Success; ++index)
                    {
                        Copy(target[index], source[index]);
                    }
                }
            }
        }
    }

    template <class Destination, class Source> void Copy(HeapVector<Destination>& target, const std::vector<Source>& source)
    {
        CopyArray(target, source);
    }

    void Copy(VertexTextureCoordinates& target, const zenasset::VertexTextureCoordinates& source)
    {
        CopyArray(target, source);
    }

    template <class Destination, class Source, size_t Count>
    void Copy(Destination (&target)[Count], const Source (&source)[Count])
    {
        for (size_t index = 0; index < Count; ++index)
        {
            Copy(target[index], source[index]);
        }
    }

    void Copy(Vec2& target, const zenasset::Vec2& source)
    {
        target = Vec2(source.x, source.y);
    }

    void Copy(Vec3& target, const zenasset::Vec3& source)
    {
        target = Vec3(source.x, source.y, source.z);
    }

    void Copy(Vec4& target, const zenasset::Vec4& source)
    {
        target = Vec4(source.x, source.y, source.z, source.w);
    }

    void Copy(Quat& target, const zenasset::Quat& source)
    {
        target = Quat(source.w, source.x, source.y, source.z);
    }

    void Copy(Mat4& target, const zenasset::Mat4& source)
    {
        for (uint32_t column = 0; column < 4; ++column)
        {
            Copy(target[column], source[column]);
        }
    }

    void Copy(AABB& target, const zenasset::Bounds& source)
    {
        target = AABB(Vec3(source.min.x, source.min.y, source.min.z), Vec3(source.max.x, source.max.y, source.max.z));
    }

    void Copy(Format& target, zenasset::PixelFormat source)
    {
        target = Format::UNDEFINED;
        switch (source)
        {
            case zenasset::PixelFormat::RGBA8Unorm: target = Format::R8G8B8A8_UNORM; break;
            case zenasset::PixelFormat::RGBA8Srgb: target = Format::R8G8B8A8_SRGB; break;
            case zenasset::PixelFormat::BC4Unorm: target = Format::BC4_UNORM_BLOCK; break;
            case zenasset::PixelFormat::BC5Unorm: target = Format::BC5_UNORM_BLOCK; break;
            case zenasset::PixelFormat::BC7Unorm: target = Format::BC7_UNORM_BLOCK; break;
            case zenasset::PixelFormat::BC7Srgb: target = Format::BC7_SRGB_BLOCK; break;
            default:
                Fail(SceneImportStatus::UnsupportedFeature, "Processed image format "
                                                                + std::to_string(static_cast<uint32_t>(source))
                                                                + " is unsupported by the runtime adapter");
                break;
        }
    }

    void Copy(TextureInterpretation& target, zenasset::TextureInterpretation source)
    {
        switch (source)
        {
            case zenasset::TextureInterpretation::Default: target = TextureInterpretation::Default; break;
            case zenasset::TextureInterpretation::NormalXYPositiveZ: target = TextureInterpretation::NormalXYPositiveZ; break;
            default: Fail(SceneImportStatus::InvalidData, "Unknown processed texture interpretation"); break;
        }
    }

    void Copy(ImageAsset& target, const zenasset::ImageAsset& source)
    {
        Copy(target.name, source.name);
        if (Active())
        {
            SharedPtr<ImagePixels, MultiThreadCounter> pixels = MakeShared<ImagePixels, MultiThreadCounter>();
            Copy(*pixels, *source.pixels);
            if (Active())
            {
                target.pixels = std::move(pixels);
            }
        }
    }

    void Copy(NodeId& target, zenasset::NodeId source)
    {
        target.value = source.value;
    }

    void Copy(MeshId& target, zenasset::MeshId source)
    {
        target.value = source.value;
    }

    void Copy(MaterialId& target, zenasset::MaterialId source)
    {
        target.value = source.value;
    }

    void Copy(ImageId& target, zenasset::ImageId source)
    {
        target.value = source.value;
    }

    void Copy(TextureId& target, zenasset::TextureId source)
    {
        target.value = source.value;
    }

    void Copy(SamplerId& target, zenasset::SamplerId source)
    {
        target.value = source.value;
    }

    void Copy(SkinId& target, zenasset::SkinId source)
    {
        target.value = source.value;
    }

    void Copy(CameraId& target, zenasset::CameraId source)
    {
        target.value = source.value;
    }

    void Copy(LightId& target, zenasset::LightId source)
    {
        target.value = source.value;
    }

    void Copy(AlphaMode& target, zenasset::AlphaMode source)
    {
        switch (source)
        {
            case zenasset::AlphaMode::Opaque: target = AlphaMode::Opaque; break;
            case zenasset::AlphaMode::Mask: target = AlphaMode::Mask; break;
            case zenasset::AlphaMode::Blend: target = AlphaMode::Blend; break;
            default: Fail(SceneImportStatus::InvalidData, "Unknown AlphaMode value in processed scene"); break;
        }
    }

    void Copy(MeshTopology& target, zenasset::MeshTopology source)
    {
        switch (source)
        {
            case zenasset::MeshTopology::Points: target = MeshTopology::Points; break;
            case zenasset::MeshTopology::Lines: target = MeshTopology::Lines; break;
            case zenasset::MeshTopology::Triangles: target = MeshTopology::Triangles; break;
            default: Fail(SceneImportStatus::InvalidData, "Unknown MeshTopology value in processed scene"); break;
        }
    }

    void Copy(VertexAttributeDomain& target, zenasset::VertexAttributeDomain source)
    {
        switch (source)
        {
            case zenasset::VertexAttributeDomain::SourceVertex: target = VertexAttributeDomain::SourceVertex; break;
            case zenasset::VertexAttributeDomain::PrimitiveVertex: target = VertexAttributeDomain::PrimitiveVertex; break;
            default: Fail(SceneImportStatus::InvalidData, "Unknown VertexAttributeDomain value in processed scene"); break;
        }
    }

    void Copy(LightKind& target, zenasset::LightKind source)
    {
        switch (source)
        {
            case zenasset::LightKind::Directional: target = LightKind::Directional; break;
            case zenasset::LightKind::Point: target = LightKind::Point; break;
            case zenasset::LightKind::Spot: target = LightKind::Spot; break;
            default: Fail(SceneImportStatus::InvalidData, "Unknown LightKind value in processed scene"); break;
        }
    }

    void Copy(AnimatedObject& target, zenasset::AnimatedObject source)
    {
        switch (source)
        {
            case zenasset::AnimatedObject::Node: target = AnimatedObject::Node; break;
            case zenasset::AnimatedObject::Material: target = AnimatedObject::Material; break;
            case zenasset::AnimatedObject::Camera: target = AnimatedObject::Camera; break;
            case zenasset::AnimatedObject::Light: target = AnimatedObject::Light; break;
            case zenasset::AnimatedObject::Environment: target = AnimatedObject::Environment; break;
            default: Fail(SceneImportStatus::InvalidData, "Unknown AnimatedObject value in processed scene"); break;
        }
    }

    void Copy(AnimatedProperty& target, zenasset::AnimatedProperty source)
    {
        switch (source)
        {
            case zenasset::AnimatedProperty::Invalid: target = AnimatedProperty::Invalid; break;
            case zenasset::AnimatedProperty::Translation: target = AnimatedProperty::Translation; break;
            case zenasset::AnimatedProperty::Rotation: target = AnimatedProperty::Rotation; break;
            case zenasset::AnimatedProperty::Scale: target = AnimatedProperty::Scale; break;
            case zenasset::AnimatedProperty::Weights: target = AnimatedProperty::Weights; break;
            case zenasset::AnimatedProperty::Visible: target = AnimatedProperty::Visible; break;
            case zenasset::AnimatedProperty::Selectable: target = AnimatedProperty::Selectable; break;
            case zenasset::AnimatedProperty::Hoverable: target = AnimatedProperty::Hoverable; break;
            case zenasset::AnimatedProperty::BaseColor: target = AnimatedProperty::BaseColor; break;
            case zenasset::AnimatedProperty::Metallic: target = AnimatedProperty::Metallic; break;
            case zenasset::AnimatedProperty::Roughness: target = AnimatedProperty::Roughness; break;
            case zenasset::AnimatedProperty::Emissive: target = AnimatedProperty::Emissive; break;
            case zenasset::AnimatedProperty::AlphaCutoff: target = AnimatedProperty::AlphaCutoff; break;
            case zenasset::AnimatedProperty::NormalScale: target = AnimatedProperty::NormalScale; break;
            case zenasset::AnimatedProperty::OcclusionStrength: target = AnimatedProperty::OcclusionStrength; break;
            case zenasset::AnimatedProperty::EmissiveStrength: target = AnimatedProperty::EmissiveStrength; break;
            case zenasset::AnimatedProperty::Ior: target = AnimatedProperty::Ior; break;
            case zenasset::AnimatedProperty::Dispersion: target = AnimatedProperty::Dispersion; break;
            case zenasset::AnimatedProperty::Specular: target = AnimatedProperty::Specular; break;
            case zenasset::AnimatedProperty::SpecularColor: target = AnimatedProperty::SpecularColor; break;
            case zenasset::AnimatedProperty::Diffuse: target = AnimatedProperty::Diffuse; break;
            case zenasset::AnimatedProperty::SpecularGlossinessColor: target = AnimatedProperty::SpecularGlossinessColor; break;
            case zenasset::AnimatedProperty::Glossiness: target = AnimatedProperty::Glossiness; break;
            case zenasset::AnimatedProperty::Clearcoat: target = AnimatedProperty::Clearcoat; break;
            case zenasset::AnimatedProperty::ClearcoatRoughness: target = AnimatedProperty::ClearcoatRoughness; break;
            case zenasset::AnimatedProperty::ClearcoatNormalScale: target = AnimatedProperty::ClearcoatNormalScale; break;
            case zenasset::AnimatedProperty::SheenColor: target = AnimatedProperty::SheenColor; break;
            case zenasset::AnimatedProperty::SheenRoughness: target = AnimatedProperty::SheenRoughness; break;
            case zenasset::AnimatedProperty::Transmission: target = AnimatedProperty::Transmission; break;
            case zenasset::AnimatedProperty::Thickness: target = AnimatedProperty::Thickness; break;
            case zenasset::AnimatedProperty::AttenuationDistance: target = AnimatedProperty::AttenuationDistance; break;
            case zenasset::AnimatedProperty::AttenuationColor: target = AnimatedProperty::AttenuationColor; break;
            case zenasset::AnimatedProperty::Iridescence: target = AnimatedProperty::Iridescence; break;
            case zenasset::AnimatedProperty::IridescenceIor: target = AnimatedProperty::IridescenceIor; break;
            case zenasset::AnimatedProperty::IridescenceThicknessMinimum:
                target = AnimatedProperty::IridescenceThicknessMinimum;
                break;
            case zenasset::AnimatedProperty::IridescenceThicknessMaximum:
                target = AnimatedProperty::IridescenceThicknessMaximum;
                break;
            case zenasset::AnimatedProperty::Anisotropy: target = AnimatedProperty::Anisotropy; break;
            case zenasset::AnimatedProperty::AnisotropyRotation: target = AnimatedProperty::AnisotropyRotation; break;
            case zenasset::AnimatedProperty::DiffuseTransmission: target = AnimatedProperty::DiffuseTransmission; break;
            case zenasset::AnimatedProperty::DiffuseTransmissionColor:
                target = AnimatedProperty::DiffuseTransmissionColor;
                break;
            case zenasset::AnimatedProperty::MultiscatterColor: target = AnimatedProperty::MultiscatterColor; break;
            case zenasset::AnimatedProperty::Retroreflection: target = AnimatedProperty::Retroreflection; break;
            case zenasset::AnimatedProperty::TextureOffset: target = AnimatedProperty::TextureOffset; break;
            case zenasset::AnimatedProperty::TextureScale: target = AnimatedProperty::TextureScale; break;
            case zenasset::AnimatedProperty::TextureRotation: target = AnimatedProperty::TextureRotation; break;
            case zenasset::AnimatedProperty::CameraAspect: target = AnimatedProperty::CameraAspect; break;
            case zenasset::AnimatedProperty::CameraFov: target = AnimatedProperty::CameraFov; break;
            case zenasset::AnimatedProperty::CameraNear: target = AnimatedProperty::CameraNear; break;
            case zenasset::AnimatedProperty::CameraFar: target = AnimatedProperty::CameraFar; break;
            case zenasset::AnimatedProperty::CameraXMag: target = AnimatedProperty::CameraXMag; break;
            case zenasset::AnimatedProperty::CameraYMag: target = AnimatedProperty::CameraYMag; break;
            case zenasset::AnimatedProperty::LightColor: target = AnimatedProperty::LightColor; break;
            case zenasset::AnimatedProperty::LightIntensity: target = AnimatedProperty::LightIntensity; break;
            case zenasset::AnimatedProperty::LightRange: target = AnimatedProperty::LightRange; break;
            case zenasset::AnimatedProperty::LightInnerCone: target = AnimatedProperty::LightInnerCone; break;
            case zenasset::AnimatedProperty::LightOuterCone: target = AnimatedProperty::LightOuterCone; break;
            case zenasset::AnimatedProperty::EnvironmentIntensity: target = AnimatedProperty::EnvironmentIntensity; break;
            case zenasset::AnimatedProperty::EnvironmentRotation: target = AnimatedProperty::EnvironmentRotation; break;
            default: Fail(SceneImportStatus::InvalidData, "Unknown AnimatedProperty value in processed scene"); break;
        }
    }

    void Copy(AnimationPath& target, zenasset::AnimationPath source)
    {
        switch (source)
        {
            case zenasset::AnimationPath::Translation: target = AnimationPath::Translation; break;
            case zenasset::AnimationPath::Rotation: target = AnimationPath::Rotation; break;
            case zenasset::AnimationPath::Scale: target = AnimationPath::Scale; break;
            case zenasset::AnimationPath::Weights: target = AnimationPath::Weights; break;
            case zenasset::AnimationPath::Property: target = AnimationPath::Property; break;
            default: Fail(SceneImportStatus::InvalidData, "Unknown AnimationPath value in processed scene"); break;
        }
    }

    void Copy(AnimationInterpolation& target, zenasset::AnimationInterpolation source)
    {
        switch (source)
        {
            case zenasset::AnimationInterpolation::Linear: target = AnimationInterpolation::Linear; break;
            case zenasset::AnimationInterpolation::Step: target = AnimationInterpolation::Step; break;
            case zenasset::AnimationInterpolation::CubicSpline: target = AnimationInterpolation::CubicSpline; break;
            default: Fail(SceneImportStatus::InvalidData, "Unknown AnimationInterpolation value in processed scene"); break;
        }
    }

    void Copy(TextureFilter& target, zenasset::TextureFilter source)
    {
        switch (source)
        {
            case zenasset::TextureFilter::Nearest: target = TextureFilter::Nearest; break;
            case zenasset::TextureFilter::Linear: target = TextureFilter::Linear; break;
            case zenasset::TextureFilter::Unspecified: target = TextureFilter::MaxEnum; break;
            default: Fail(SceneImportStatus::InvalidData, "Unknown TextureFilter value in processed scene"); break;
        }
    }

    void Copy(SamplerAddressMode& target, zenasset::SamplerAddressMode source)
    {
        switch (source)
        {
            case zenasset::SamplerAddressMode::Repeat: target = SamplerAddressMode::Repeat; break;
            case zenasset::SamplerAddressMode::MirroredRepeat: target = SamplerAddressMode::MirroredRepeat; break;
            case zenasset::SamplerAddressMode::ClampToEdge: target = SamplerAddressMode::ClampToEdge; break;
            case zenasset::SamplerAddressMode::ClampToBorder: target = SamplerAddressMode::ClampToBorder; break;
            case zenasset::SamplerAddressMode::MirrorClampToEdge: target = SamplerAddressMode::MirrorClampToEdge; break;
            case zenasset::SamplerAddressMode::Unspecified: target = SamplerAddressMode::MaxEnum; break;
            default: Fail(SceneImportStatus::InvalidData, "Unknown SamplerAddressMode value in processed scene"); break;
        }
    }

    void Copy(Vertex& target, const zenasset::Vertex& source)
    {
        Copy(target.pos, source.pos);
        Copy(target.normal, source.normal);
        Copy(target.tangent, source.tangent);
        Copy(target.uv0, source.uv0);
        Copy(target.uv1, source.uv1);
        Copy(target.joint0, source.joint0);
        Copy(target.weight0, source.weight0);
        Copy(target.color, source.color);
    }

    void Copy(CameraParameters& target, const zenasset::CameraParameters& source)
    {
        Copy(target.orthographic, source.orthographic);
        Copy(target.infiniteFar, source.infiniteFar);
        Copy(target.fixedAspect, source.fixedAspect);
        Copy(target.aspect, source.aspect);
        Copy(target.verticalFov, source.verticalFov);
        Copy(target.nearPlane, source.nearPlane);
        Copy(target.farPlane, source.farPlane);
        Copy(target.xmag, source.xmag);
        Copy(target.ymag, source.ymag);
    }

    void Copy(SkinAsset& target, const zenasset::SkinAsset& source)
    {
        Copy(target.name, source.name);
        Copy(target.skeleton, source.skeleton);
        Copy(target.joints, source.joints);
        Copy(target.inverseBindMatrices, source.inverseBindMatrices);
    }

    void Copy(MorphColorAttributeAsset& target, const zenasset::MorphColorAttributeAsset& source)
    {
        Copy(target.set, source.set);
        Copy(target.values, source.values);
    }

    void Copy(MorphTargetAsset& target, const zenasset::MorphTargetAsset& source)
    {
        Copy(target.positions, source.positions);
        Copy(target.normals, source.normals);
        Copy(target.tangents, source.tangents);
        Copy(target.texCoords, source.texCoords);
        Copy(target.colors, source.colors);
        Copy(target.extraColors, source.extraColors);
    }

    void Copy(MorphPrimitiveAsset& target, const zenasset::MorphPrimitiveAsset& source)
    {
        Copy(target.mesh, source.mesh);
        Copy(target.primitive, source.primitive);
        Copy(target.weights, source.weights);
        Copy(target.targets, source.targets);
    }

    void Copy(VertexJointInfluence& target, const zenasset::VertexJointInfluence& source)
    {
        Copy(target.joint, source.joint);
        Copy(target.weight, source.weight);
    }

    void Copy(MaterialVariantPrimitiveAsset& target, const zenasset::MaterialVariantPrimitiveAsset& source)
    {
        Copy(target.mesh, source.mesh);
        Copy(target.primitive, source.primitive);
        Copy(target.materials, source.materials);
    }

    void Copy(VertexAttributeAsset& target, const zenasset::VertexAttributeAsset& source)
    {
        Copy(target.mesh, source.mesh);
        Copy(target.primitive, source.primitive);
        Copy(target.semantic, source.semantic);
        Copy(target.morphTarget, source.morphTarget);
        Copy(target.components, source.components);
        Copy(target.componentType, source.componentType);
        Copy(target.normalized, source.normalized);
        Copy(target.domain, source.domain);
        Copy(target.values, source.values);
    }

    void Copy(ImageBasedLightAsset& target, const zenasset::ImageBasedLightAsset& source)
    {
        Copy(target.name, source.name);
        Copy(target.intensity, source.intensity);
        Copy(target.rotation, source.rotation);
        Copy(target.irradianceCoefficients, source.irradianceCoefficients);
        if (!source.specular)
        {
            Fail(SceneImportStatus::InvalidData, "Processed environment has no cube pixel owner", source.name);
        }
        else if (Active())
        {
            Copy(target.size, source.specular->size);
            Copy(target.mipLevels, source.specular->mipLevels);
            if (source.specular->format == zenasset::PixelFormat::RGBA32Float && source.specular->blockMipFaces.empty())
            {
                target.specularFormat = Format::R32G32B32A32_SFLOAT;
                Copy(target.specularMipFaces, source.specular->rgba32FloatMipFaces);
            }
            else if (source.specular->format == zenasset::PixelFormat::BC6HUfloat
                     && source.specular->rgba32FloatMipFaces.empty())
            {
                target.specularFormat = Format::BC6H_UFLOAT_BLOCK;
                SharedPtr<HeapVector<HeapVector<uint8_t>>, MultiThreadCounter> blocks =
                    MakeShared<HeapVector<HeapVector<uint8_t>>, MultiThreadCounter>();
                Copy(*blocks, source.specular->blockMipFaces);
                if (Active())
                {
                    target.specularBlockMipFaces = std::move(blocks);
                }
            }
            else
            {
                Fail(SceneImportStatus::InvalidData, "Unsupported or mixed processed environment payload", source.name);
            }
        }
    }

    void Copy(NodeAsset& target, const zenasset::NodeAsset& source)
    {
        Copy(target.name, source.name);
        Copy(target.parent, source.parent);
        Copy(target.children, source.children);
        Copy(target.translation, source.translation);
        Copy(target.rotation, source.rotation);
        Copy(target.scale, source.scale);
        Copy(target.baseMatrix, source.baseMatrix);
        Copy(target.matrixTransform, source.matrixTransform);
        Copy(target.mesh, source.mesh);
        Copy(target.materials, source.materials);
        Copy(target.skin, source.skin);
        Copy(target.camera, source.camera);
        Copy(target.light, source.light);
        Copy(target.morphWeights, source.morphWeights);
        Copy(target.morphWeightsSource, source.morphWeightsSource);
        Copy(target.visible, source.visible);
        Copy(target.selectable, source.selectable);
        Copy(target.hoverable, source.hoverable);
        Copy(target.renderMesh, source.renderMesh);
    }

    void Copy(MeshPrimitiveAsset& target, const zenasset::MeshPrimitiveAsset& source)
    {
        Copy(target.name, source.name);
        Copy(target.firstVertex, source.firstVertex);
        Copy(target.vertexCount, source.vertexCount);
        Copy(target.firstIndex, source.firstIndex);
        Copy(target.indexCount, source.indexCount);
        Copy(target.material, source.material);
        Copy(target.materialSlot, source.materialSlot);
        Copy(target.topology, source.topology);
        Copy(target.bounds, source.bounds);
        Copy(target.flatNormals, source.flatNormals);
        Copy(target.sourceVertices, source.sourceVertices);
        Copy(target.influences, source.influences);
    }

    void Copy(MeshAsset& target, const zenasset::MeshAsset& source)
    {
        Copy(target.name, source.name);
        Copy(target.primitives, source.primitives);
        Copy(target.morphWeights, source.morphWeights);
    }

    void Copy(CameraAsset& target, const zenasset::CameraAsset& source)
    {
        Copy(static_cast<CameraParameters&>(target), static_cast<const zenasset::CameraParameters&>(source));
        Copy(target.name, source.name);
    }

    void Copy(LightAsset& target, const zenasset::LightAsset& source)
    {
        Copy(target.name, source.name);
        Copy(target.kind, source.kind);
        Copy(target.color, source.color);
        Copy(target.intensity, source.intensity);
        Copy(target.range, source.range);
        Copy(target.innerConeAngle, source.innerConeAngle);
        Copy(target.outerConeAngle, source.outerConeAngle);
    }

    void Copy(SceneDependency& target, const zenasset::SceneDependency& source)
    {
        Copy(target.path, source.path);
        Copy(target.exists, source.exists);
        Copy(target.size, source.size);
        Copy(target.modified, source.modified);
        Copy(target.contentHash, source.contentHash);
    }

    void Copy(SceneAsset& target, const zenasset::Scene& source)
    {
        Copy(target.name, source.name);
        Copy(target.sourcePath, source.sourcePath);
        Copy(target.dependencies, source.dependencies);
        Copy(target.sourceDocument, source.sourceDocument);
        Copy(target.nodes, source.nodes);
        Copy(target.roots, source.roots);
        Copy(target.meshes, source.meshes);
        Copy(target.vertices, source.vertices);
        Copy(target.indices, source.indices);
        Copy(target.vertexTexCoords, source.vertexTexCoords);
        Copy(target.vertexTexCoordSets, source.vertexTexCoordSets);
        Copy(target.materials, source.materials);
        Copy(target.images, source.images);
        Copy(target.textures, source.textures);
        Copy(target.samplers, source.samplers);
        Copy(target.skins, source.skins);
        Copy(target.animations, source.animations);
        Copy(target.morphPrimitives, source.morphPrimitives);
        Copy(target.cameras, source.cameras);
        Copy(target.lights, source.lights);
        Copy(target.materialVariants, source.materialVariants);
        Copy(target.variantPrimitives, source.variantPrimitives);
        Copy(target.extraVertexAttributes, source.extraVertexAttributes);
        Copy(target.imageBasedLights, source.imageBasedLights);
        Copy(target.imageBasedLight, source.imageBasedLight);
    }

    void Copy(TextureTransform& target, const zenasset::TextureTransform& source)
    {
        Copy(target.row0, source.row0);
        Copy(target.row1, source.row1);
        Copy(target.uvOffset, source.uvOffset);
        Copy(target.uvScale, source.uvScale);
        Copy(target.uvRotation, source.uvRotation);
    }

    void Copy(MaterialTextureBinding& target, const zenasset::MaterialTextureBinding& source)
    {
        Copy(target.texture, source.texture);
        Copy(target.texCoord, source.texCoord);
        Copy(target.transform, source.transform);
        Copy(target.scale, source.scale);
    }

    void Copy(MaterialFeatureFactors& target, const zenasset::MaterialFeatureFactors& source)
    {
        Copy(target.ior, source.ior);
        Copy(target.dispersion, source.dispersion);
        Copy(target.specular, source.specular);
        Copy(target.specularColor, source.specularColor);
        Copy(target.glossiness, source.glossiness);
        Copy(target.clearcoat, source.clearcoat);
        Copy(target.clearcoatRoughness, source.clearcoatRoughness);
        Copy(target.sheenColor, source.sheenColor);
        Copy(target.sheenRoughness, source.sheenRoughness);
        Copy(target.transmission, source.transmission);
        Copy(target.thickness, source.thickness);
        Copy(target.attenuationColor, source.attenuationColor);
        Copy(target.attenuationDistance, source.attenuationDistance);
        Copy(target.iridescence, source.iridescence);
        Copy(target.iridescenceIor, source.iridescenceIor);
        Copy(target.iridescenceThicknessMin, source.iridescenceThicknessMin);
        Copy(target.iridescenceThicknessMax, source.iridescenceThicknessMax);
        Copy(target.anisotropy, source.anisotropy);
        Copy(target.anisotropyRotation, source.anisotropyRotation);
        Copy(target.diffuseTransmission, source.diffuseTransmission);
        Copy(target.diffuseTransmissionColor, source.diffuseTransmissionColor);
        Copy(target.multiscatterColor, source.multiscatterColor);
        Copy(target.retroreflection, source.retroreflection);
    }

    void Copy(MaterialFeatures& target, const zenasset::MaterialFeatures& source)
    {
        Copy(static_cast<MaterialFeatureFactors&>(target), static_cast<const zenasset::MaterialFeatureFactors&>(source));
        Copy(target.specularTexture, source.specularTexture);
        Copy(target.specularColorTexture, source.specularColorTexture);
        Copy(target.diffuseTexture, source.diffuseTexture);
        Copy(target.specularGlossinessTexture, source.specularGlossinessTexture);
        Copy(target.clearcoatTexture, source.clearcoatTexture);
        Copy(target.clearcoatRoughnessTexture, source.clearcoatRoughnessTexture);
        Copy(target.clearcoatNormalTexture, source.clearcoatNormalTexture);
        Copy(target.sheenColorTexture, source.sheenColorTexture);
        Copy(target.sheenRoughnessTexture, source.sheenRoughnessTexture);
        Copy(target.transmissionTexture, source.transmissionTexture);
        Copy(target.thicknessTexture, source.thicknessTexture);
        Copy(target.iridescenceTexture, source.iridescenceTexture);
        Copy(target.iridescenceThicknessTexture, source.iridescenceThicknessTexture);
        Copy(target.anisotropyTexture, source.anisotropyTexture);
        Copy(target.diffuseTransmissionTexture, source.diffuseTransmissionTexture);
        Copy(target.diffuseTransmissionColorTexture, source.diffuseTransmissionColorTexture);
        Copy(target.retroreflectionTexture, source.retroreflectionTexture);
    }

    void Copy(MaterialAsset& target, const zenasset::MaterialAsset& source)
    {
        Copy(target.name, source.name);
        Copy(target.alphaMode, source.alphaMode);
        Copy(target.doubleSided, source.doubleSided);
        Copy(target.alphaCutoff, source.alphaCutoff);
        Copy(target.metallicFactor, source.metallicFactor);
        Copy(target.roughnessFactor, source.roughnessFactor);
        Copy(target.normalScale, source.normalScale);
        Copy(target.occlusionStrength, source.occlusionStrength);
        Copy(target.textureTransforms, source.textureTransforms);
        Copy(target.features, source.features);
        Copy(target.baseColorFactor, source.baseColorFactor);
        Copy(target.emissiveFactor, source.emissiveFactor);
        Copy(target.m_pBaseColorTexture, source.m_pBaseColorTexture);
        Copy(target.m_pMetallicRoughnessTexture, source.m_pMetallicRoughnessTexture);
        Copy(target.m_pNormalTexture, source.m_pNormalTexture);
        Copy(target.m_pOcclusionTexture, source.m_pOcclusionTexture);
        Copy(target.m_pEmissiveTexture, source.m_pEmissiveTexture);
        Copy(target.texCoordSets.baseColor, source.texCoordSets.baseColor);
        Copy(target.texCoordSets.metallicRoughness, source.texCoordSets.metallicRoughness);
        Copy(target.texCoordSets.specularGlossiness, source.texCoordSets.specularGlossiness);
        Copy(target.texCoordSets.normal, source.texCoordSets.normal);
        Copy(target.texCoordSets.occlusion, source.texCoordSets.occlusion);
        Copy(target.texCoordSets.emissive, source.texCoordSets.emissive);
        Copy(target.extension.pSpecularGlossinessTexture, source.extension.pSpecularGlossinessTexture);
        Copy(target.extension.pDiffuseTexture, source.extension.pDiffuseTexture);
        Copy(target.extension.diffuseFactor, source.extension.diffuseFactor);
        Copy(target.extension.specularFactor, source.extension.specularFactor);
        Copy(target.pbrWorkflows.metallicRoughness, source.pbrWorkflows.metallicRoughness);
        Copy(target.pbrWorkflows.specularGlossiness, source.pbrWorkflows.specularGlossiness);
        Copy(target.unlit, source.unlit);
        Copy(target.emissiveStrength, source.emissiveStrength);
    }

    void Copy(SamplerParameters& target, const zenasset::SamplerParameters& source)
    {
        Copy(target.minFilter, source.minFilter);
        Copy(target.magFilter, source.magFilter);
        Copy(target.mipFilter, source.mipFilter);
        Copy(target.useMipmaps, source.useMipmaps);
        Copy(target.wrapS, source.wrapS);
        Copy(target.wrapT, source.wrapT);
    }

    void Copy(SamplerAsset& target, const zenasset::SamplerAsset& source)
    {
        Copy(static_cast<SamplerParameters&>(target), static_cast<const zenasset::SamplerParameters&>(source));
        Copy(target.name, source.name);
    }

    void Copy(ImagePixels& target, const zenasset::ImagePixels& source)
    {
        Copy(target.width, source.width);
        Copy(target.height, source.height);
        Copy(target.format, source.format);
        Copy(target.bytes, source.bytes);
        Copy(target.mips, source.mips);
        Copy(target.arrayLayers, source.arrayLayers);
    }

    void Copy(TextureAsset& target, const zenasset::TextureAsset& source)
    {
        Copy(target.name, source.name);
        Copy(target.image, source.image);
        Copy(target.sampler, source.sampler);
        Copy(target.linearSource, source.linearSource);
        Copy(target.format, source.format);
        Copy(target.flatNormal, source.flatNormal);
        Copy(target.interpretation, source.interpretation);
        Copy(target.arrayLayer, source.arrayLayer);
        target.atlasRegion = {source.atlasRegion.x, source.atlasRegion.y, source.atlasRegion.width, source.atlasRegion.height};
    }

    void Copy(AnimationTarget& target, const zenasset::AnimationTarget& source)
    {
        Copy(target.object, source.object);
        Copy(target.index, source.index);
        Copy(target.property, source.property);
        Copy(target.dimensions, source.dimensions);
        Copy(target.element, source.element);
        Copy(target.textureSlot, source.textureSlot);
    }

    void Copy(AnimationSampler& target, const zenasset::AnimationSampler& source)
    {
        Copy(target.interpolation, source.interpolation);
        Copy(target.components, source.components);
        Copy(target.times, source.times);
        Copy(target.values, source.values);
    }

    void Copy(AnimationChannel& target, const zenasset::AnimationChannel& source)
    {
        Copy(target.sampler, source.sampler);
        Copy(target.node, source.node);
        Copy(target.path, source.path);
        Copy(target.target, source.target);
        Copy(target.sourceTarget, source.sourceTarget);
    }

    void Copy(AnimationBakeSettings& target, const zenasset::AnimationBakeSettings& source)
    {
        Copy(target.sampleRate, source.sampleRate);
        Copy(target.translationTolerance, source.translationTolerance);
        Copy(target.rotationTolerance, source.rotationTolerance);
        Copy(target.scaleTolerance, source.scaleTolerance);
        Copy(target.valueTolerance, source.valueTolerance);
        Copy(target.maxSubdivisions, source.maxSubdivisions);
        Copy(target.maxValues, source.maxValues);
    }

    void Copy(AnimationAsset& target, const zenasset::AnimationAsset& source)
    {
        Copy(target.name, source.name);
        Copy(target.samplers, source.samplers);
        Copy(target.channels, source.channels);
        Copy(target.baked, source.baked);
        Copy(target.bakeSettings, source.bakeSettings);
    }

    const zenasset::Scene&    m_source;
    const SceneImportControl* m_control;
    SceneImportResult&        m_result;
};
} // namespace

SceneImportResult AdaptProcessedScene(const zenasset::Scene&    source,
                                      const SceneImportControl* control,
                                      uint64_t                  maxRuntimeGeometryBytes)
{
    SceneImportResult result;
    SceneAdapter      adapter(source, control, result);
    adapter.Run(maxRuntimeGeometryBytes);
    return result;
}

bool AdaptProcessedAnimationTarget(const zenasset::AnimationTarget& source, AnimationTarget& target)
{
    const zenasset::Scene definitions;
    SceneImportResult     result;
    SceneAdapter          adapter(definitions, nullptr, result);
    return adapter.ConvertAnimationTarget(source, target);
}
} // namespace zen::asset
