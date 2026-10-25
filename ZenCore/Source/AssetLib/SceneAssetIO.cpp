#include "AssetLib/SceneAssetIO.h"
#include "AssetLib/SceneAssetIOInternal.h"
#include "AssetLib/AssetHash.h"
#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <type_traits>
#include "ZenAsset/Format/Utf8.h"
#include "AssetLib/ProcessedSceneIO.h"
#if defined(_WIN32)
#    include <Windows.h>
#    include <fcntl.h>
#    include <io.h>
#else
#    include <unistd.h>
#endif

namespace zen::asset
{
namespace
{
// Each list below is the schema, shared by the reader and writer. Integers and
// IEEE float bit patterns use little-endian order, bools one byte, enums/IDs u32,
// and arrays/UTF-8 strings a u64 count followed by their semantic elements.
template <class Archive> void Fields(Archive& ar, typename Archive::template Value<Vec2>& v)
{
    ar(v.x, v.y);
}

template <class Archive> void Fields(Archive& ar, typename Archive::template Value<Vec3>& v)
{
    ar(v.x, v.y, v.z);
}

template <class Archive> void Fields(Archive& ar, typename Archive::template Value<Vec4>& v)
{
    ar(v.x, v.y, v.z, v.w);
}

template <class Archive> void Fields(Archive& ar, typename Archive::template Value<Quat>& v)
{
    ar(v.w, v.x, v.y, v.z);
}

template <class Archive> void Fields(Archive& ar, typename Archive::template Value<Vertex>& v)
{
    ar(v.pos, v.normal, v.tangent, v.uv0, v.uv1, v.joint0, v.weight0, v.color);
}

template <class Archive> void Fields(Archive& ar, typename Archive::template Value<NodeAsset>& v)
{
    ar(v.name, v.parent, v.children, v.translation, v.rotation, v.scale, v.baseMatrix, v.matrixTransform, v.mesh, v.materials,
       v.skin, v.camera, v.light, v.morphWeights, v.morphWeightsSource, v.visible, v.selectable, v.hoverable, v.renderMesh);
}

template <class Archive> void Fields(Archive& ar, typename Archive::template Value<MeshPrimitiveAsset>& v)
{
    ar(v.name, v.firstVertex, v.vertexCount, v.firstIndex, v.indexCount, v.material, v.materialSlot, v.topology, v.bounds,
       v.flatNormals, v.sourceVertices, v.influences);
}

template <class Archive> void Fields(Archive& ar, typename Archive::template Value<MeshAsset>& v)
{
    ar(v.name, v.primitives, v.morphWeights);
}

template <class Archive> void Fields(Archive& ar, typename Archive::template Value<CameraAsset>& v)
{
    ar(v.name, v.orthographic, v.infiniteFar, v.fixedAspect, v.aspect, v.verticalFov, v.nearPlane, v.farPlane, v.xmag, v.ymag);
}

template <class Archive> void Fields(Archive& ar, typename Archive::template Value<LightAsset>& v)
{
    ar(v.name, v.kind, v.color, v.intensity, v.range, v.innerConeAngle, v.outerConeAngle);
}

template <class Archive> void Fields(Archive& ar, typename Archive::template Value<SkinAsset>& v)
{
    ar(v.name, v.skeleton, v.joints, v.inverseBindMatrices);
}

template <class Archive> void Fields(Archive& ar, typename Archive::template Value<AnimationTarget>& v)
{
    ar(v.object, v.index, v.property, v.dimensions, v.element, v.textureSlot);
}

template <class Archive> void Fields(Archive& ar, typename Archive::template Value<AnimationSampler>& v)
{
    ar(v.interpolation, v.components, v.times, v.values);
}

template <class Archive> void Fields(Archive& ar, typename Archive::template Value<AnimationChannel>& v)
{
    ar(v.sampler, v.node, v.path, v.target, v.sourceTarget);
}

template <class Archive> void Fields(Archive& ar, typename Archive::template Value<AnimationBakeSettings>& v)
{
    ar(v.sampleRate, v.translationTolerance, v.rotationTolerance, v.scaleTolerance, v.valueTolerance, v.maxSubdivisions,
       v.maxValues);
}

template <class Archive> void Fields(Archive& ar, typename Archive::template Value<AnimationAsset>& v)
{
    ar(v.name, v.samplers, v.channels, v.baked, v.bakeSettings);
}

template <class Archive> void Fields(Archive& ar, typename Archive::template Value<MorphColorAttributeAsset>& v)
{
    ar(v.set, v.values);
}

template <class Archive> void Fields(Archive& ar, typename Archive::template Value<MorphTargetAsset>& v)
{
    ar(v.positions, v.normals, v.tangents, v.texCoords, v.colors, v.extraColors);
}

template <class Archive> void Fields(Archive& ar, typename Archive::template Value<MorphPrimitiveAsset>& v)
{
    ar(v.mesh, v.primitive, v.weights, v.targets);
}

template <class Archive> void Fields(Archive& ar, typename Archive::template Value<VertexJointInfluence>& v)
{
    ar(v.joint, v.weight);
}

template <class Archive> void Fields(Archive& ar, typename Archive::template Value<MaterialVariantPrimitiveAsset>& v)
{
    ar(v.mesh, v.primitive, v.materials);
}

template <class Archive> void Fields(Archive& ar, typename Archive::template Value<VertexAttributeAsset>& v)
{
    ar(v.mesh, v.primitive, v.semantic, v.morphTarget, v.components, v.componentType, v.normalized, v.domain, v.values);
}

template <class Archive> void Fields(Archive& ar, typename Archive::template Value<ImageBasedLightAsset>& v)
{
    if constexpr (!Archive::Reading)
    {
        if (v.specularFormat != Format::R32G32B32A32_SFLOAT || v.specularBlockMipFaces)
        {
            ar.Fail(SceneImportStatus::UnsupportedFeature, "Legacy v2 cannot store compressed environments; use processed v3");
        }
    }
    ar(v.name, v.intensity, v.rotation, v.irradianceCoefficients, v.size, v.mipLevels, v.specularMipFaces);
}

template <class Archive> void Fields(Archive& ar, typename Archive::template Value<TextureTransform>& v)
{
    ar(v.row0, v.row1, v.uvOffset, v.uvScale, v.uvRotation);
}

template <class Archive> void Fields(Archive& ar, typename Archive::template Value<MaterialTextureBinding>& v)
{
    ar(v.texture, v.texCoord, v.transform, v.scale);
}

template <class Archive> void Fields(Archive& ar, typename Archive::template Value<MaterialFeatureFactors>& v)
{
    ar(v.ior, v.dispersion, v.specular, v.specularColor, v.glossiness, v.clearcoat, v.clearcoatRoughness, v.sheenColor,
       v.sheenRoughness, v.transmission, v.thickness, v.attenuationColor, v.attenuationDistance, v.iridescence,
       v.iridescenceIor, v.iridescenceThicknessMin, v.iridescenceThicknessMax, v.anisotropy, v.anisotropyRotation,
       v.diffuseTransmission, v.diffuseTransmissionColor, v.multiscatterColor, v.retroreflection);
}

template <class Archive> void Fields(Archive& ar, typename Archive::template Value<SamplerAsset>& v)
{
    ar(v.name, v.minFilter, v.magFilter, v.mipFilter, v.useMipmaps, v.wrapS, v.wrapT);
}

template <class Archive> void Fields(Archive& ar, typename Archive::template Value<TextureAsset>& v)
{
    if constexpr (!Archive::Reading)
    {
        if (v.interpretation != TextureInterpretation::Default || v.arrayLayer != 0 || v.atlasRegion != ImageRegion{})
        {
            ar.Fail(SceneImportStatus::UnsupportedFeature,
                    "Legacy v2 cannot store texture interpretation, array layers or spatial regions; use processed v3");
        }
    }
    ar(v.name, v.image, v.sampler, v.linearSource, v.format, v.flatNormal);
}

template <class Archive> void Fields(Archive& ar, typename Archive::template Value<ImagePixels>& v)
{
    if constexpr (!Archive::Reading)
    {
        if (v.arrayLayers != 1)
        {
            ar.Fail(SceneImportStatus::UnsupportedFeature, "Legacy v2 cannot store array pages; use processed v3");
        }
    }
    ar(v.width, v.height, v.format, v.bytes, v.mips);
}

template <class Archive> void Fields(Archive& ar, typename Archive::template Value<SceneImportSettings>& v)
{
    ar(v.sceneIndex, v.animationBake, v.fallbackMissingNamedUVSets);
}

template <class Archive> void Fields(Archive& ar, typename Archive::template Value<SceneImportDiagnostic>& v)
{
    ar(v.severity, v.status, v.source, v.object, v.message);
}

template <class Archive> void Fields(Archive& ar, typename Archive::template Value<SceneDependency>& v)
{
    ar(v.path, v.exists, v.size, v.modified, v.contentHash);
}

template <class Archive> void Fields(Archive& ar, typename Archive::template Value<SceneAssetFileMetadata>& v)
{
    ar(v.sourceFormat, v.importerVersion, v.processorVersion, v.buildFingerprint, v.settings, v.cacheKey, v.inputs,
       v.diagnostics);
}

template <class Archive> void Fields(Archive& ar, typename Archive::template Value<Mat4>& v)
{
    for (uint32_t column = 0; column < 4; ++column)
    {
        ar(v[column]);
    }
}

template <class Archive> void Fields(Archive& ar, typename Archive::template Value<AABB>& v)
{
    Vec3 minimum = v.GetMin();
    Vec3 maximum = v.GetMax();
    ar(minimum, maximum);
    if constexpr (Archive::Reading)
    {
        v = AABB(minimum, maximum);
    }
}

template <class Archive> void Fields(Archive& ar, typename Archive::template Value<MaterialAsset>& v)
{
    ar(v.name, v.alphaMode, v.doubleSided, v.alphaCutoff, v.metallicFactor, v.roughnessFactor, v.normalScale,
       v.occlusionStrength);
    for (uint32_t slot = 0; slot < 5; ++slot)
    {
        ar(v.textureTransforms[slot]);
    }
    ar(static_cast<typename Archive::template Value<MaterialFeatureFactors>&>(v.features));
    for (MaterialFeatures::TextureMember member : MaterialFeatures::textureMembers)
    {
        ar(v.features.*member);
    }
    ar(v.baseColorFactor, v.emissiveFactor, v.m_pBaseColorTexture, v.m_pMetallicRoughnessTexture, v.m_pNormalTexture,
       v.m_pOcclusionTexture, v.m_pEmissiveTexture, v.texCoordSets.baseColor, v.texCoordSets.metallicRoughness,
       v.texCoordSets.specularGlossiness, v.texCoordSets.normal, v.texCoordSets.occlusion, v.texCoordSets.emissive,
       v.extension.pSpecularGlossinessTexture, v.extension.pDiffuseTexture, v.extension.diffuseFactor,
       v.extension.specularFactor, v.pbrWorkflows.metallicRoughness, v.pbrWorkflows.specularGlossiness, v.unlit,
       v.emissiveStrength);
}

template <class Archive> void Fields(Archive& ar, typename Archive::template Value<ImageAsset>& v)
{
    ar(v.name);
    if (ar.Reserve(1, sizeof(ImagePixels)))
    {
        if constexpr (Archive::Reading)
        {
            SharedPtr<ImagePixels, MultiThreadCounter> pixels(new ImagePixels);
            ar(*pixels);
            v.pixels = std::move(pixels);
        }
        else
        {
            ar(*v.pixels);
        }
    }
}

// Arrays of these element types are stored as their little-endian memory image:
// the field lists above match the member order and the types have no padding.
template <class T> constexpr bool RawElement()
{
    using U = std::remove_cv_t<T>;
    return std::endian::native == std::endian::little
        && ((std::is_arithmetic_v<U> && !std::is_same_v<U, bool>) || std::is_same_v<U, Vec2> || std::is_same_v<U, Vec3>
            || std::is_same_v<U, Vec4> || std::is_same_v<U, Mat4> || std::is_same_v<U, Vertex>
            || std::is_same_v<U, VertexJointInfluence>);
}
static_assert(sizeof(Vec3) == 12 && sizeof(Vec4) == 16 && sizeof(Mat4) == 64 && sizeof(Vertex) == 28 * sizeof(float)
              && sizeof(VertexJointInfluence) == 8);

constexpr size_t BufferBytes = size_t(4) << 20;
constexpr size_t ChunkBytes  = size_t(64) << 20;

template <bool IsReading> class AssetArchive
{
public:
    static constexpr bool Reading  = IsReading;
    template <class T> using Value = std::conditional_t<Reading, T, const T>;

    AssetArchive(FILE*                       file,
                 uint64_t                    bytes,
                 const SceneAssetReadLimits& limits,
                 uint64_t&                   budget,
                 AssetHash&                  hash,
                 SceneImportControl*         control) :
        m_file(file), m_remaining(bytes), m_unread(bytes), m_limits(limits), m_budget(budget), m_hash(hash), m_control(control)
    {
        if (m_file != nullptr)
        {
            m_buffer.resize_for_overwrite(BufferBytes);
        }
    }

    template <class... T> void operator()(T&... value)
    {
        (One(value), ...);
    }

    bool Good() const
    {
        return m_status == SceneImportStatus::Success;
    }
    uint64_t Remaining() const
    {
        return m_remaining;
    }
    SceneImportStatus Status() const
    {
        return m_status;
    }
    const std::string& Error() const
    {
        return m_error;
    }

    void Fail(SceneImportStatus status, const char* message)
    {
        if (Good())
        {
            m_status = status;
            m_error  = message;
        }
    }

    bool Reserve(uint64_t count, size_t stride)
    {
        if (Good() && (count > m_limits.maxArrayElements || count > m_remaining))
        {
            Fail(SceneImportStatus::InvalidData, "Internal asset count exceeds its section bounds");
        }
        if (Good() && count > m_budget / stride)
        {
            Fail(SceneImportStatus::ResourceLimit, "Internal asset allocation budget exceeded");
        }
        if (Good())
        {
            m_budget -= count * stride;
        }
        return Good();
    }

    // Charges decoded memory whose element count was already bounded by stored bytes.
    bool Budget(uint64_t count, size_t stride)
    {
        if (Good() && count > m_budget / stride)
        {
            Fail(SceneImportStatus::ResourceLimit, "Internal asset allocation budget exceeded");
        }
        if (Good())
        {
            m_budget -= count * stride;
        }
        return Good();
    }

    // Moves bytes through a buffer bounded by the section, or directly for large spans.
    template <class Byte> void Bytes(Byte* data, uint64_t count)
    {
        static_assert(sizeof(Byte) == 1);
        if (count > m_remaining)
        {
            Fail(SceneImportStatus::InvalidData, "Internal asset field exceeds its section bounds");
        }
        if (Good())
        {
            if constexpr (Reading)
            {
                Read(reinterpret_cast<uint8_t*>(data), count);
            }
            else
            {
                Write(reinterpret_cast<const uint8_t*>(data), count);
            }
        }
        if (Good())
        {
            m_hash.Update(data, static_cast<size_t>(count));
            m_remaining -= count;
        }
    }

    // Writers publish buffered bytes before the caller seeks or closes the file.
    void Finish()
    {
        if constexpr (!Reading)
        {
            FlushBuffer();
        }
    }

private:
    bool Cancelled() const
    {
        return m_control != nullptr && m_control->IsCancelled();
    }

    void Read(uint8_t* data, uint64_t count)
    {
        while (Good() && count > 0)
        {
            if (m_bufferPosition < m_bufferSize)
            {
                const size_t take = static_cast<size_t>(std::min<uint64_t>(count, m_bufferSize - m_bufferPosition));
                std::memcpy(data, m_buffer.data() + m_bufferPosition, take);
                m_bufferPosition += take;
                data             += take;
                count            -= take;
            }
            else if (Cancelled())
            {
                Fail(SceneImportStatus::Cancelled, "Internal asset I/O cancelled");
            }
            else if (count >= BufferBytes)
            {
                const size_t chunk = static_cast<size_t>(std::min<uint64_t>(count, ChunkBytes));
                if (chunk > m_unread || std::fread(data, 1, chunk, m_file) != chunk)
                {
                    Fail(SceneImportStatus::IOError, "Could not read internal asset bytes");
                }
                else
                {
                    m_unread -= chunk;
                    data     += chunk;
                    count    -= chunk;
                }
            }
            else
            {
                const size_t refill = static_cast<size_t>(std::min<uint64_t>(m_buffer.size(), m_unread));
                if (refill == 0 || std::fread(m_buffer.data(), 1, refill, m_file) != refill)
                {
                    Fail(SceneImportStatus::IOError, "Could not read internal asset bytes");
                }
                else
                {
                    m_unread         -= refill;
                    m_bufferSize      = refill;
                    m_bufferPosition  = 0;
                }
            }
        }
    }

    void Write(const uint8_t* data, uint64_t count)
    {
        if (m_file != nullptr && count >= BufferBytes)
        {
            FlushBuffer();
            while (Good() && count > 0)
            {
                const size_t chunk = static_cast<size_t>(std::min<uint64_t>(count, ChunkBytes));
                if (Cancelled())
                {
                    Fail(SceneImportStatus::Cancelled, "Internal asset I/O cancelled");
                }
                else if (std::fwrite(data, 1, chunk, m_file) != chunk)
                {
                    Fail(SceneImportStatus::IOError, "Could not write internal asset bytes");
                }
                else
                {
                    data  += chunk;
                    count -= chunk;
                }
            }
        }
        else if (m_file != nullptr)
        {
            while (Good() && count > 0)
            {
                const size_t take = static_cast<size_t>(std::min<uint64_t>(count, m_buffer.size() - m_bufferSize));
                std::memcpy(m_buffer.data() + m_bufferSize, data, take);
                m_bufferSize += take;
                data         += take;
                count        -= take;
                if (m_bufferSize == m_buffer.size())
                {
                    FlushBuffer();
                }
            }
        }
    }

    void FlushBuffer()
    {
        if (Good() && m_file != nullptr && m_bufferSize > 0)
        {
            if (Cancelled())
            {
                Fail(SceneImportStatus::Cancelled, "Internal asset I/O cancelled");
            }
            else if (std::fwrite(m_buffer.data(), 1, m_bufferSize, m_file) != m_bufferSize)
            {
                Fail(SceneImportStatus::IOError, "Could not write internal asset bytes");
            }
        }
        m_bufferSize = 0;
    }

    template <class T> void Scalar(T& value)
    {
        using U    = std::remove_cv_t<T>;
        using Bits = std::conditional_t<sizeof(U) == 1, uint8_t, std::conditional_t<sizeof(U) == 4, uint32_t, uint64_t>>;
        static_assert(sizeof(U) == 1 || sizeof(U) == 4 || sizeof(U) == 8);
        Bits    bits = std::bit_cast<Bits>(value);
        uint8_t bytes[sizeof(U)]{};
        if constexpr (!Reading)
        {
            for (size_t i = 0; i < sizeof(U); ++i)
            {
                bytes[i] = static_cast<uint8_t>(bits >> (i * 8));
            }
        }
        Bytes(bytes, sizeof(U));
        if constexpr (Reading)
        {
            bits = 0;
            for (size_t i = 0; i < sizeof(U); ++i)
            {
                bits |= static_cast<Bits>(bytes[i]) << (i * 8);
            }
            value = std::bit_cast<U>(bits);
        }
    }

    template <class T> void One(T& value)
    {
        using U = std::remove_cv_t<T>;
        if (Good())
        {
            if constexpr (std::is_same_v<U, bool>)
            {
                uint8_t encoded = value ? 1 : 0;
                Scalar(encoded);
                if constexpr (Reading)
                {
                    if (encoded > 1)
                    {
                        Fail(SceneImportStatus::InvalidData, "Invalid internal asset boolean");
                    }
                    value = encoded != 0;
                }
            }
            else if constexpr (std::is_enum_v<U>)
            {
                uint32_t encoded = static_cast<uint32_t>(value);
                Scalar(encoded);
                if constexpr (Reading)
                {
                    value = static_cast<U>(encoded);
                }
            }
            else if constexpr (std::is_arithmetic_v<U>)
            {
                Scalar(value);
            }
            else if constexpr (std::is_same_v<U, std::string>)
            {
                uint64_t count = value.size();
                Scalar(count);
                if (count > m_limits.maxStringBytes)
                {
                    Fail(SceneImportStatus::ResourceLimit, "Internal asset string too large");
                }
                if (Reserve(count, 1))
                {
                    if constexpr (Reading)
                    {
                        value.resize(static_cast<size_t>(count));
                    }
                }
                if (Good())
                {
                    Bytes(value.data(), count);
                    if (Good() && !zenasset::IsValidUtf8(value))
                    {
                        Fail(SceneImportStatus::InvalidData, "Internal asset string is not valid UTF-8");
                    }
                }
            }
            else if constexpr (requires {
                                   value.IsValid();
                                   value.value;
                               })
            {
                Scalar(value.value);
            }
            else if constexpr (requires(U& collection) {
                                   typename U::value_type;
                                   collection.resize(size_t(0));
                               })
            {
                using Element  = typename U::value_type;
                uint64_t count = value.size();
                Scalar(count);
                if (Reserve(count, sizeof(Element)))
                {
                    if constexpr (Reading && RawElement<Element>()
                                  && requires(U& collection) { collection.resize_for_overwrite(size_t(0)); })
                    {
                        value.resize_for_overwrite(static_cast<size_t>(count));
                    }
                    else if constexpr (Reading)
                    {
                        value.resize(static_cast<size_t>(count));
                    }
                }
                if constexpr (RawElement<Element>())
                {
                    if (Good() && count > 0)
                    {
                        Bytes(reinterpret_cast<std::conditional_t<Reading, uint8_t, const uint8_t>*>(value.data()),
                              count * sizeof(Element));
                    }
                }
                else
                {
                    for (size_t i = 0; Good() && i < value.size(); ++i)
                    {
                        One(value[i]);
                    }
                }
            }
            else
            {
                Fields(*this, value);
            }
        }
    }

    FILE*                       m_file;
    uint64_t                    m_remaining;
    uint64_t                    m_unread;
    const SceneAssetReadLimits& m_limits;
    uint64_t&                   m_budget;
    AssetHash&                  m_hash;
    SceneImportControl*         m_control;
    HeapVector<uint8_t>         m_buffer;
    size_t                      m_bufferSize{0};
    size_t                      m_bufferPosition{0};
    SceneImportStatus           m_status{SceneImportStatus::Success};
    std::string                 m_error;
};

// Per-vertex coordinate-set counts followed by every coordinate in vertex order,
// so millions of small per-vertex arrays move as two bulk streams.
template <class Archive>
void TexCoordStreams(Archive& ar, typename Archive::template Value<HeapVector<VertexTextureCoordinates>>& coordinates)
{
    HeapVector<uint32_t> counts;
    HeapVector<Vec2>     values;
    if constexpr (!Archive::Reading)
    {
        size_t total = 0;
        counts.resize_for_overwrite(coordinates.size());
        for (size_t vertex = 0; vertex < coordinates.size(); ++vertex)
        {
            counts[vertex]  = static_cast<uint32_t>(coordinates[vertex].size());
            total          += coordinates[vertex].size();
        }
        values.resize_for_overwrite(total);
        size_t offset = 0;
        for (const VertexTextureCoordinates& set : coordinates)
        {
            if (set.size() > 0)
            {
                std::memcpy(values.data() + offset, set.data(), set.size() * sizeof(Vec2));
            }
            offset += set.size();
        }
    }
    ar(counts, values);
    if constexpr (Archive::Reading)
    {
        uint64_t total = 0;
        for (uint32_t count : counts)
        {
            total += count;
        }
        if (ar.Good() && total != values.size())
        {
            ar.Fail(SceneImportStatus::InvalidData, "Internal asset texture coordinate streams disagree");
        }
        if (ar.Good() && ar.Budget(counts.size(), sizeof(VertexTextureCoordinates)))
        {
            coordinates.resize(counts.size());
            size_t offset = 0;
            for (size_t vertex = 0; vertex < counts.size(); ++vertex)
            {
                coordinates[vertex].resize(counts[vertex]);
                if (counts[vertex] > 0)
                {
                    std::memcpy(coordinates[vertex].data(), values.data() + offset, counts[vertex] * sizeof(Vec2));
                }
                offset += counts[vertex];
            }
        }
    }
}

template <class Archive> void AssetSection(Archive&                                                  ar,
                                           typename Archive::template Value<SceneAsset>&             asset,
                                           typename Archive::template Value<SceneAssetFileMetadata>& metadata,
                                           uint32_t                                                  section)
{
    switch (section)
    {
        // Section zero stays small so cache lookups can inspect it cheaply.
        case 0: ar(metadata, asset.name, asset.sourcePath, asset.dependencies); break;
        case 1:
            ar(asset.sourceDocument, asset.nodes, asset.roots, asset.meshes, asset.materials, asset.textures, asset.samplers,
               asset.skins, asset.animations, asset.morphPrimitives, asset.cameras, asset.lights, asset.materialVariants,
               asset.variantPrimitives, asset.extraVertexAttributes, asset.imageBasedLight);
            break;
        case 2:
            ar(asset.vertices, asset.indices);
            TexCoordStreams(ar, asset.vertexTexCoords);
            ar(asset.vertexTexCoordSets, asset.images, asset.imageBasedLights);
            break;
        default: ar.Fail(SceneImportStatus::InvalidData, "Invalid internal asset section"); break;
    }
}

constexpr uint64_t HeaderBytes = 48;
constexpr uint32_t ByteOrder   = 0x01020304;
struct FileHeader
{
    uint8_t  magic[8]{'Z', 'E', 'N', 'A', 'S', 'S', 'E', 'T'};
    uint32_t version{SceneAssetSchemaVersion};
    uint32_t byteOrder{ByteOrder};
    uint64_t sections[3]{};
    uint64_t payloadHash{0};
};

template <class Archive> void Header(Archive& ar, typename Archive::template Value<FileHeader>& header)
{
    ar.Bytes(header.magic, 8);
    ar(header.version, header.byteOrder, header.sections[0], header.sections[1], header.sections[2], header.payloadHash);
}

FILE* OpenFile(const std::filesystem::path& path, bool writing)
{
    FILE* file = nullptr;
#if defined(_WIN32)
    const HANDLE handle =
        CreateFileW(path.c_str(), writing ? GENERIC_WRITE : GENERIC_READ, writing ? 0 : FILE_SHARE_READ | FILE_SHARE_DELETE,
                    nullptr, writing ? CREATE_NEW : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle != INVALID_HANDLE_VALUE)
    {
        const int descriptor =
            _open_osfhandle(reinterpret_cast<intptr_t>(handle), _O_BINARY | (writing ? _O_WRONLY : _O_RDONLY));
        if (descriptor == -1)
        {
            CloseHandle(handle);
        }
        else
        {
            file = _fdopen(descriptor, writing ? "wb" : "rb");
            if (file == nullptr)
            {
                _close(descriptor);
            }
        }
    }
#else
    file = std::fopen(path.c_str(), writing ? "wbx" : "rb");
#endif
    if (file != nullptr)
    {
        // Archives buffer themselves; bulk spans then go straight to the OS.
        std::setvbuf(file, nullptr, _IONBF, 0);
    }
    return file;
}

bool FlushFile(FILE* file, bool durable)
{
    bool result = std::fflush(file) == 0;
    if (result && durable)
    {
#if defined(_WIN32)
        result = _commit(_fileno(file)) == 0;
#else
        result = fsync(fileno(file)) == 0;
#endif
    }
    return result;
}

bool ReplaceFile(const std::filesystem::path& temporary, const std::filesystem::path& destination)
{
    bool result = false;
#if defined(_WIN32)
    std::error_code             error;
    const std::filesystem::path fullDestination = std::filesystem::absolute(destination, error);
    const size_t                pathBytes       = fullDestination.native().size() * sizeof(wchar_t);
    HANDLE                      handle          = INVALID_HANDLE_VALUE;
    if (!error && pathBytes <= 65534)
    {
        handle = CreateFileW(temporary.c_str(), DELETE | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    }
    if (handle != INVALID_HANDLE_VALUE)
    {
        const size_t         storageBytes = sizeof(FILE_RENAME_INFO) + pathBytes;
        HeapVector<uint64_t> storage((storageBytes + sizeof(uint64_t) - 1) / sizeof(uint64_t), 0);
        static_assert(alignof(FILE_RENAME_INFO) <= alignof(uint64_t));
        FILE_RENAME_INFO* information = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
        information->Flags            = FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS;
        information->FileNameLength   = static_cast<DWORD>(pathBytes);
        std::memcpy(information->FileName, fullDestination.c_str(), pathBytes);
        // Existing readers retain the old file while new opens see the complete replacement.
        result = SetFileInformationByHandle(handle, FileRenameInfoEx, information, static_cast<DWORD>(storageBytes)) != 0;
        CloseHandle(handle);
    }
#else
    std::error_code error;
    std::filesystem::rename(temporary, destination, error);
    result = !error;
#endif
    return result;
}

void FileFailure(SceneImportReport& report, SceneImportStatus status, const std::string& path, const std::string& message)
{
    if (report.error.empty())
    {
        report.status = status;
        report.error  = message;
        report.diagnostics.push_back({SceneDiagnosticSeverity::Error, status, path, {}, message});
    }
}

SceneAssetFileResult ReadAssetFile(const std::string&          path,
                                   const SceneAssetReadLimits& limits,
                                   SceneImportControl*         control,
                                   bool                        metadataOnly)
{
    SceneAssetFileResult result;
    result.format          = "zenasset";
    result.importerVersion = SceneAssetSchemaVersion;
    std::error_code             error;
    const std::filesystem::path filePath  = std::filesystem::u8path(path);
    const uint64_t              fileBytes = std::filesystem::file_size(filePath, error);
    FILE*                       file      = nullptr;
    if (control != nullptr && control->IsCancelled())
    {
        FileFailure(result, SceneImportStatus::Cancelled, path, "Internal asset load cancelled");
    }
    else if (error)
    {
        FileFailure(result, SceneImportStatus::SourceUnavailable, path, "Internal asset file is unavailable");
    }
    else if (fileBytes < HeaderBytes || fileBytes > limits.maxFileBytes)
    {
        FileFailure(result, fileBytes < HeaderBytes ? SceneImportStatus::InvalidData : SceneImportStatus::ResourceLimit, path,
                    "Internal asset file size is outside the allowed range");
    }
    else if ((file = OpenFile(filePath, false)) == nullptr)
    {
        FileFailure(result, SceneImportStatus::IOError, path, "Could not open internal asset file");
    }
    else
    {
        if (control != nullptr && !metadataOnly)
        {
            control->SetStage(SceneImportStage::Reading);
        }
        FileHeader         header;
        AssetHash          headerHash;
        uint64_t           budget = limits.maxAllocationBytes;
        AssetArchive<true> archive(file, HeaderBytes, limits, budget, headerHash, control);
        Header(archive, header);
        const FileHeader expected;
        bool valid = archive.Good() && std::equal(std::begin(header.magic), std::end(header.magic), std::begin(expected.magic))
                  && header.version == SceneAssetSchemaVersion && header.byteOrder == ByteOrder;
        uint64_t remaining = fileBytes - HeaderBytes;
        for (uint64_t section : header.sections)
        {
            if (section > remaining)
            {
                valid = false;
            }
            else
            {
                remaining -= section;
            }
        }
        valid &= remaining == 0;
        if (!valid)
        {
            FileFailure(result, archive.Good() ? SceneImportStatus::InvalidData : archive.Status(), path,
                        "Internal asset header, version or section bounds are invalid");
        }
        else
        {
            SharedPtr<SceneAsset, MultiThreadCounter> candidate(new SceneAsset);
            AssetHash                                 payloadHash;
            for (uint32_t section = 0; section < (metadataOnly ? 1u : 3u) && result.error.empty(); ++section)
            {
                AssetArchive<true> payload(file, header.sections[section], limits, budget, payloadHash, control);
                AssetSection(payload, *candidate, result.metadata, section);
                if (!payload.Good() || payload.Remaining() != 0)
                {
                    FileFailure(result, payload.Good() ? SceneImportStatus::InvalidData : payload.Status(), path,
                                payload.Good() ? "Internal asset section contains trailing bytes" : payload.Error());
                }
            }
            std::string reason;
            if (result.error.empty() && !metadataOnly && payloadHash.Finish() != header.payloadHash)
            {
                FileFailure(result, SceneImportStatus::InvalidData, path, "Internal asset checksum does not match");
            }
            else if (result.error.empty() && !metadataOnly
                     && !ValidateSceneAsset(*candidate, limits.maxRuntimeGeometryBytes, &reason))
            {
                FileFailure(result, SceneImportStatus::InvalidData, path, "Internal asset is invalid: " + reason);
            }
            if (result.error.empty())
            {
                if (!metadataOnly)
                {
                    result.asset = std::move(candidate);
                }
                result.status      = SceneImportStatus::Success;
                result.diagnostics = result.metadata.diagnostics;
            }
        }
    }
    if (file != nullptr && std::fclose(file) != 0)
    {
        FileFailure(result, SceneImportStatus::IOError, path, "Could not close internal asset file");
    }
    if (!result.error.empty())
    {
        result.asset.Reset();
    }
    if (control != nullptr && !metadataOnly)
    {
        control->SetStage(result.asset                                    ? SceneImportStage::AssetReady
                          : result.status == SceneImportStatus::Cancelled ? SceneImportStage::Cancelled
                                                                          : SceneImportStage::Failed);
    }
    return result;
}

SceneImportReport WriteAssetFile(const std::string&            path,
                                 const SceneAsset&             asset,
                                 const SceneAssetFileMetadata& metadata,
                                 SceneImportControl*           control,
                                 bool                          validate,
                                 bool                          durable)
{
    SceneImportReport result;
    result.format          = "zenasset";
    result.importerVersion = SceneAssetSchemaVersion;
    static std::atomic<uint64_t> sequence{0};
    const std::filesystem::path  destination = std::filesystem::u8path(path);
    std::filesystem::path        temporary   = destination;
    temporary        += ".tmp-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-"
                      + std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
    FILE*       file  = nullptr;
    std::string reason;
    if (control != nullptr && control->IsCancelled())
    {
        FileFailure(result, SceneImportStatus::Cancelled, path, "Internal asset write cancelled");
    }
    else if (validate && !ValidateSceneAsset(asset, SceneAssetReadLimits{}.maxRuntimeGeometryBytes, &reason))
    {
        FileFailure(result, SceneImportStatus::InvalidData, path, "Cannot serialize an invalid canonical scene: " + reason);
    }
    else if ((file = OpenFile(temporary, true)) == nullptr)
    {
        FileFailure(result, SceneImportStatus::IOError, path, "Could not create internal asset temporary file");
    }
    else
    {
        const SceneAssetReadLimits limits;
        uint64_t                   budget = limits.maxAllocationBytes;
        FileHeader                 header;
        AssetHash                  headerHash;
        AssetArchive<false>        initial(file, HeaderBytes, limits, budget, headerHash, control);
        Header(initial, header);
        initial.Finish();
        if (!initial.Good())
        {
            FileFailure(result, initial.Status(), path, initial.Error());
        }
        AssetHash payloadHash;
        uint64_t  available = limits.maxFileBytes - HeaderBytes;
        for (uint32_t section = 0; section < 3 && result.error.empty(); ++section)
        {
            AssetArchive<false> payload(file, available, limits, budget, payloadHash, control);
            AssetSection(payload, asset, metadata, section);
            payload.Finish();
            header.sections[section] = available - payload.Remaining();
            available                = payload.Remaining();
            if (!payload.Good())
            {
                FileFailure(result, payload.Status(), path, payload.Error());
            }
        }
        header.payloadHash = payloadHash.Finish();
        if (result.error.empty())
        {
            if (std::fseek(file, 0, SEEK_SET) != 0)
            {
                FileFailure(result, SceneImportStatus::IOError, path, "Could not finalize internal asset header");
            }
            else
            {
                AssetArchive<false> finalHeader(file, HeaderBytes, limits, budget, headerHash, control);
                Header(finalHeader, header);
                finalHeader.Finish();
                if (!finalHeader.Good())
                {
                    FileFailure(result, finalHeader.Status(), path, finalHeader.Error());
                }
            }
        }
        if (result.error.empty() && !FlushFile(file, durable))
        {
            FileFailure(result, SceneImportStatus::IOError, path, "Could not flush internal asset file");
        }
        if (std::fclose(file) != 0)
        {
            FileFailure(result, SceneImportStatus::IOError, path, "Could not close internal asset temporary file");
        }
        if (result.error.empty() && control != nullptr && control->IsCancelled())
        {
            FileFailure(result, SceneImportStatus::Cancelled, path, "Internal asset write cancelled");
        }
        if (result.error.empty() && !ReplaceFile(temporary, destination))
        {
            FileFailure(result, SceneImportStatus::IOError, path, "Could not replace internal asset destination");
        }
        if (!result.error.empty())
        {
            std::error_code removeError;
            std::filesystem::remove(temporary, removeError);
        }
        else
        {
            result.status = SceneImportStatus::Success;
        }
    }
    return result;
}
uint32_t PackageVersion(const std::string& path)
{
    uint32_t version = 0;
    FILE*    file    = OpenFile(std::filesystem::u8path(path), false);
    if (file != nullptr)
    {
        uint8_t header[12]{};
        if (std::fread(header, 1, sizeof(header), file) == sizeof(header) && std::memcmp(header, "ZENASSET", 8) == 0)
        {
            version =
                uint32_t(header[8]) | (uint32_t(header[9]) << 8) | (uint32_t(header[10]) << 16) | (uint32_t(header[11]) << 24);
        }
        std::fclose(file);
    }
    return version;
}

SceneAssetFileResult ReadPackageFile(const std::string&          path,
                                     const SceneAssetReadLimits& limits,
                                     SceneImportControl*         control,
                                     bool                        metadataOnly)
{
    SceneAssetFileResult result;
    if (control != nullptr && control->IsCancelled())
    {
        FileFailure(result, SceneImportStatus::Cancelled, path, "Internal asset load cancelled");
    }
    else if (path.empty() || path.find('\0') != std::string::npos || !zenasset::IsValidUtf8(path))
    {
        FileFailure(result, SceneImportStatus::InvalidRequest, path,
                    "A nonempty UTF-8 package path without null bytes is required");
    }
    else
    {
        result = PackageVersion(path) == zenasset::PackageSchemaVersion
                   ? ReadProcessedSceneFile(path, limits, control, metadataOnly)
                   : ReadAssetFile(path, limits, control, metadataOnly);
    }
    return result;
}
} // namespace

SceneAssetFileResult ReadSceneAssetFile(const std::string&          path,
                                        const SceneAssetReadLimits& limits,
                                        SceneImportControl*         control)
{
    if (control != nullptr)
    {
        control->SetFileProgress({0, 1, true});
    }
    SceneAssetFileResult result = ReadPackageFile(path, limits, control, false);
    if (control != nullptr && result.status == SceneImportStatus::Success && result.asset)
    {
        control->SetFileProgress({1, 1, true});
    }
    return result;
}

SceneAssetFileResult ReadSceneAssetMetadata(const std::string& path, SceneImportControl* control)
{
    SceneAssetReadLimits limits;
    limits.maxAllocationBytes = 64ull * 1024 * 1024;
    return ReadPackageFile(path, limits, control, true);
}

std::string SceneAssetCacheKey(const SceneAssetFileMetadata& metadata)
{
    return SceneAssetCacheKeyForVersion(metadata, SceneAssetSchemaVersion);
}

std::string SceneAssetCacheKeyForVersion(const SceneAssetFileMetadata& metadata, uint32_t schemaVersion)
{
    AssetHash                  hash;
    const SceneAssetReadLimits limits;
    uint64_t                   budget = limits.maxAllocationBytes;
    AssetArchive<false>        archive(nullptr, limits.maxFileBytes, limits, budget, hash, nullptr);
    archive(schemaVersion, metadata.sourceFormat, metadata.importerVersion, metadata.processorVersion,
            metadata.buildFingerprint, metadata.settings, metadata.inputs);
    if (schemaVersion >= zenasset::PackageSchemaVersion)
    {
        const uint64_t processingHash = zenasset::ProcessingSettingsHash(metadata.processing);
        archive(metadata.profile, processingHash);
    }
    return DigestText(hash.Finish());
}

SceneImportReport WriteSceneAssetFile(const std::string&            path,
                                      const SceneAsset&             asset,
                                      const SceneAssetFileMetadata& metadata,
                                      SceneImportControl*           control)
{
    return WriteAssetFile(path, asset, metadata, control, true, true);
}

SceneImportReport WriteValidatedSceneAssetFile(const std::string&            path,
                                               const SceneAsset&             asset,
                                               const SceneAssetFileMetadata& metadata,
                                               SceneImportControl*           control)
{
    // Cache publication: the asset was validated by ImportScene, and the payload
    // checksum rejects a file left incomplete by a crash, so no fsync is needed.
    return WriteAssetFile(path, asset, metadata, control, false, false);
}
} // namespace zen::asset
