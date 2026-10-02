#include <future>
#include <cmath>
#include <algorithm>
#include <cctype>
#include <climits>
#include <mutex>
#include <fstream>
#include <map>
#include <tuple>
#include <charconv>
#include <type_traits>
#include <meshoptimizer.h>
#include <webp/decode.h>
#include <basisu_transcoder.h>
#include <draco/compression/decode.h>
#include <simdjson.h>
#include <stb_image.h>
#include "AssetLib/FastGLTFLoader.h"
#include <fastgltf/glm_element_traits.hpp>
#include "SceneGraph/Scene.h"
#include "SceneGraph/SceneAnimation.h"
#include "Utils/Errors.h"
#include "Utils/ThreadPool.h"
#include "Graphics/RenderCore/V2/RenderConfig.h"

namespace zen::asset
{
static fastgltf::span<const std::byte> GetBufferBytes(const fastgltf::Asset& asset,
                                                      size_t                 bufferIndex,
                                                      size_t                 offset,
                                                      size_t                 length)
{
    fastgltf::span<const std::byte> bytes;

    if (bufferIndex < asset.buffers.size())
    {
        const fastgltf::DataSource& source = asset.buffers[bufferIndex].data;

        if (const fastgltf::sources::Array* array = std::get_if<fastgltf::sources::Array>(&source))
        {
            bytes = fastgltf::span<const std::byte>(array->bytes.data(), array->bytes.size());
        }
        else if (const fastgltf::sources::Vector* vector = std::get_if<fastgltf::sources::Vector>(&source))
        {
            bytes = fastgltf::span<const std::byte>(vector->bytes.data(), vector->bytes.size());
        }
        else if (const fastgltf::sources::ByteView* view = std::get_if<fastgltf::sources::ByteView>(&source))
        {
            bytes = view->bytes;
        }
    }

    if (offset > bytes.size() || length > bytes.size() - offset)
    {
        LOG_ERROR_AND_THROW("glTF buffer range is outside its loaded data");
    }

    return bytes.subspan(offset, length);
}

static size_t AddDecodedBuffer(fastgltf::Asset& asset, std::vector<std::byte> bytes)
{
    const size_t index = asset.buffers.size();

    fastgltf::Buffer buffer;

    buffer.byteLength = bytes.size();

    buffer.data       = fastgltf::sources::Vector{std::move(bytes)};

    asset.buffers.push_back(std::move(buffer));

    return index;
}

static void DecodeMeshoptBuffers(fastgltf::Asset& asset)
{
    for (fastgltf::BufferView& view : asset.bufferViews)
    {
        if (view.meshoptCompression)
        {
            const fastgltf::CompressedBufferView& compression = *view.meshoptCompression;

            const fastgltf::span<const std::byte> source =
                GetBufferBytes(asset, compression.bufferIndex, compression.byteOffset, compression.byteLength);

            if (compression.byteStride == 0 || compression.count > SIZE_MAX / compression.byteStride)
            {
                LOG_ERROR_AND_THROW("Invalid meshopt decoded buffer size");
            }

            const bool vertexMode  = compression.mode == fastgltf::MeshoptCompressionMode::Attributes;

            const bool strideValid = vertexMode ? compression.byteStride <= 256 && compression.byteStride % 4 == 0
                                                : compression.byteStride == 2 || compression.byteStride == 4;

            const bool filterValid = compression.filter == fastgltf::MeshoptCompressionFilter::None
                                  || (vertexMode
                                      && (compression.filter == fastgltf::MeshoptCompressionFilter::Exponential
                                          || ((compression.filter == fastgltf::MeshoptCompressionFilter::Octahedral
                                               || compression.filter == fastgltf::MeshoptCompressionFilter::Color)
                                              && (compression.byteStride == 4 || compression.byteStride == 8))
                                          || (compression.filter == fastgltf::MeshoptCompressionFilter::Quaternion
                                              && compression.byteStride == 8)));

            if (!strideValid || !filterValid
                || (compression.mode == fastgltf::MeshoptCompressionMode::Triangles && compression.count % 3 != 0))
            {
                LOG_ERROR_AND_THROW("Invalid meshopt compression mode, stride or filter");
            }

            std::vector<std::byte> decoded(compression.count * compression.byteStride);

            const unsigned char* input = reinterpret_cast<const unsigned char*>(source.data());

            int status                 = -1;

            switch (compression.mode)
            {
                case fastgltf::MeshoptCompressionMode::Attributes:
                    status = meshopt_decodeVertexBuffer(decoded.data(), compression.count, compression.byteStride, input,
                                                        source.size());

                    break;

                case fastgltf::MeshoptCompressionMode::Triangles:
                    status = meshopt_decodeIndexBuffer(decoded.data(), compression.count, compression.byteStride, input,
                                                       source.size());

                    break;

                case fastgltf::MeshoptCompressionMode::Indices:
                    status = meshopt_decodeIndexSequence(decoded.data(), compression.count, compression.byteStride, input,
                                                         source.size());

                    break;
            }

            if (status != 0)
            {
                LOG_ERROR_AND_THROW(fmt::format("Failed to decode meshopt buffer ({})", status));
            }

            switch (compression.filter)
            {
                case fastgltf::MeshoptCompressionFilter::Octahedral:
                    meshopt_decodeFilterOct(decoded.data(), compression.count, compression.byteStride);

                    break;

                case fastgltf::MeshoptCompressionFilter::Quaternion:
                    meshopt_decodeFilterQuat(decoded.data(), compression.count, compression.byteStride);

                    break;

                case fastgltf::MeshoptCompressionFilter::Exponential:
                    meshopt_decodeFilterExp(decoded.data(), compression.count, compression.byteStride);

                    break;

                case fastgltf::MeshoptCompressionFilter::Color:
                    meshopt_decodeFilterColor(decoded.data(), compression.count, compression.byteStride);

                    break;

                case fastgltf::MeshoptCompressionFilter::None: break;
            }

            view.bufferIndex = AddDecodedBuffer(asset, std::move(decoded));

            view.byteOffset  = 0;

            view.byteLength  = compression.count * compression.byteStride;

            view.meshoptCompression.reset();
        }
    }
}

template <typename T> static void DecodeDracoAttributeValues(const draco::Mesh&           mesh,
                                                             const draco::PointAttribute& attribute,
                                                             std::vector<std::byte>&      bytes)
{
    const size_t components = attribute.num_components();

    bytes.resize(size_t(mesh.num_points()) * components * sizeof(T));

    for (uint32_t index = 0; index < mesh.num_points(); ++index)
    {
        T values[16]{};

        if (components > 16
            || !attribute.ConvertValue(attribute.mapped_index(draco::PointIndex(index)), static_cast<int8_t>(components),
                                       values))
        {
            LOG_ERROR_AND_THROW("Failed to convert Draco vertex attribute");
        }

        std::memcpy(bytes.data() + size_t(index) * components * sizeof(T), values, components * sizeof(T));
    }
}

static void SetDecodedAccessor(fastgltf::Asset& asset, fastgltf::Accessor& accessor, std::vector<std::byte> bytes)
{
    fastgltf::BufferView view;

    view.byteLength          = bytes.size();

    view.bufferIndex         = AddDecodedBuffer(asset, std::move(bytes));

    accessor.bufferViewIndex = asset.bufferViews.size();

    accessor.byteOffset      = 0;

    accessor.sparse.reset();

    asset.bufferViews.push_back(std::move(view));
}

static void DecodeDracoMeshes(fastgltf::Asset& asset)
{
    for (fastgltf::Mesh& sourceMesh : asset.meshes)
    {
        for (fastgltf::Primitive& primitive : sourceMesh.primitives)
        {
            if (primitive.dracoCompression)
            {
                const fastgltf::DracoCompressedPrimitive& compression = *primitive.dracoCompression;

                if (compression.bufferView >= asset.bufferViews.size())
                {
                    LOG_ERROR_AND_THROW("Draco references an invalid buffer view");
                }

                if (primitive.indicesAccessor && *primitive.indicesAccessor >= asset.accessors.size())
                {
                    LOG_ERROR_AND_THROW("Draco references an invalid indices accessor");
                }

                for (const fastgltf::Attribute& encoded : compression.attributes)
                {
                    const fastgltf::Attribute* destination = primitive.findAttribute(encoded.name);

                    if (destination == primitive.attributes.end() || destination->accessorIndex >= asset.accessors.size()
                        || encoded.accessorIndex > UINT32_MAX)
                    {
                        LOG_ERROR_AND_THROW("Invalid Draco attribute mapping");
                    }
                }

                const fastgltf::BufferView& view = asset.bufferViews[compression.bufferView];

                const fastgltf::span<const std::byte> bytes =
                    GetBufferBytes(asset, view.bufferIndex, view.byteOffset, view.byteLength);

                draco::DecoderBuffer buffer;

                buffer.Init(reinterpret_cast<const char*>(bytes.data()), bytes.size());

                draco::Decoder decoder;

                draco::StatusOr<std::unique_ptr<draco::Mesh>> result = decoder.DecodeMeshFromBuffer(&buffer);

                if (!result.ok())
                {
                    LOG_ERROR_AND_THROW(fmt::format("Failed to decode Draco mesh: {}", result.status().error_msg()));
                }

                const std::unique_ptr<draco::Mesh> mesh = std::move(result).value();

                for (const fastgltf::Attribute& encoded : compression.attributes)
                {
                    const fastgltf::Attribute* destination = primitive.findAttribute(encoded.name);

                    const draco::PointAttribute* attribute =
                        mesh->GetAttributeByUniqueId(static_cast<uint32_t>(encoded.accessorIndex));

                    if (destination == primitive.attributes.end() || attribute == nullptr
                        || destination->accessorIndex >= asset.accessors.size())
                    {
                        LOG_ERROR_AND_THROW("Invalid Draco attribute mapping");
                    }

                    fastgltf::Accessor& accessor = asset.accessors[destination->accessorIndex];

                    if (fastgltf::getNumComponents(accessor.type) != attribute->num_components())
                    {
                        LOG_ERROR_AND_THROW("Draco attribute has an incompatible component count");
                    }

                    std::vector<std::byte> decoded;

                    switch (accessor.componentType)
                    {
                        case fastgltf::ComponentType::Byte:
                            DecodeDracoAttributeValues<int8_t>(*mesh, *attribute, decoded);
                            break;
                        case fastgltf::ComponentType::UnsignedByte:
                            DecodeDracoAttributeValues<uint8_t>(*mesh, *attribute, decoded);
                            break;
                        case fastgltf::ComponentType::Short:
                            DecodeDracoAttributeValues<int16_t>(*mesh, *attribute, decoded);
                            break;
                        case fastgltf::ComponentType::UnsignedShort:
                            DecodeDracoAttributeValues<uint16_t>(*mesh, *attribute, decoded);
                            break;
                        case fastgltf::ComponentType::Int:
                            DecodeDracoAttributeValues<int32_t>(*mesh, *attribute, decoded);
                            break;
                        case fastgltf::ComponentType::UnsignedInt:
                            DecodeDracoAttributeValues<uint32_t>(*mesh, *attribute, decoded);
                            break;
                        case fastgltf::ComponentType::Float:
                            DecodeDracoAttributeValues<float>(*mesh, *attribute, decoded);
                            break;
                        case fastgltf::ComponentType::Double:
                            DecodeDracoAttributeValues<double>(*mesh, *attribute, decoded);
                            break;
                        default: LOG_ERROR_AND_THROW("Invalid Draco attribute component type"); break;
                    }

                    accessor.count = mesh->num_points();

                    SetDecodedAccessor(asset, accessor, std::move(decoded));
                }

                if (primitive.indicesAccessor)
                {
                    fastgltf::Accessor& accessor = asset.accessors[*primitive.indicesAccessor];

                    accessor.count               = size_t(mesh->num_faces()) * 3;

                    accessor.componentType       = fastgltf::ComponentType::UnsignedInt;

                    std::vector<std::byte> indices(accessor.count * sizeof(uint32_t));

                    for (uint32_t face = 0; face < mesh->num_faces(); ++face)
                    {
                        const draco::Mesh::Face& triangle = mesh->face(draco::FaceIndex(face));

                        for (uint32_t corner = 0; corner < 3; ++corner)
                        {
                            const uint32_t index = triangle[corner].value();

                            std::memcpy(indices.data() + (size_t(face) * 3 + corner) * sizeof(uint32_t), &index, sizeof(index));
                        }
                    }

                    SetDecodedAccessor(asset, accessor, std::move(indices));
                }

                primitive.type = fastgltf::PrimitiveType::Triangles;

                primitive.dracoCompression.reset();
            }
        }
    }
}

static sg::TextureFilter FromFastGltfFilter(fastgltf::Optional<fastgltf::Filter> filter)
{
    sg::TextureFilter result = sg::TextureFilter::Linear;

    switch (filter.value_or(fastgltf::Filter::Linear))
    {
        case fastgltf::Filter::Nearest:
        case fastgltf::Filter::NearestMipMapNearest:
        case fastgltf::Filter::NearestMipMapLinear:
        {
            result = sg::TextureFilter::Nearest;

            break;
        }
        case fastgltf::Filter::Linear:
        case fastgltf::Filter::LinearMipMapNearest:
        case fastgltf::Filter::LinearMipMapLinear:
        {
            result = sg::TextureFilter::Linear;

            break;
        }
        default: break;
    }

    return result;
}

static sg::SamplerAddressMode FromFastGltfWrap(fastgltf::Wrap wrap)
{
    sg::SamplerAddressMode result = sg::SamplerAddressMode::Repeat;

    switch (wrap)
    {
        case fastgltf::Wrap::Repeat:
        {
            result = sg::SamplerAddressMode::Repeat;

            break;
        }
        case fastgltf::Wrap::ClampToEdge:
        {
            result = sg::SamplerAddressMode::ClampToEdge;

            break;
        }
        case fastgltf::Wrap::MirroredRepeat:
        {
            result = sg::SamplerAddressMode::MirroredRepeat;

            break;
        }
        default: break;
    }

    return result;
}

static fastgltf::Error ValidateSceneAsset(fastgltf::Asset& asset)
{
    bool referencesValid = !asset.defaultScene || *asset.defaultScene < asset.scenes.size();

    for (const fastgltf::Node& node : asset.nodes)
    {
        referencesValid &= !node.lightIndex || *node.lightIndex < asset.lights.size();

        for (size_t child : node.children)
        {
            referencesValid &= child < asset.nodes.size();
        }
    }

    if (referencesValid)
    {
        HeapVector<uint32_t> parents(asset.nodes.size(), 0);

        for (const fastgltf::Node& node : asset.nodes)
        {
            for (size_t child : node.children)
            {
                referencesValid &= ++parents[child] == 1;
            }
        }

        HeapVector<size_t> pending;

        for (size_t index = 0; index < parents.size(); ++index)
        {
            if (parents[index] == 0)
            {
                pending.push_back(index);
            }
        }

        for (size_t head = 0; head < pending.size(); ++head)
        {
            for (size_t child : asset.nodes[pending[head]].children)
            {
                if (--parents[child] == 0)
                {
                    pending.push_back(child);
                }
            }
        }

        referencesValid &= pending.size() == asset.nodes.size();
    }

    for (const fastgltf::Animation& animation : asset.animations)
    {
        for (const fastgltf::AnimationChannel& channel : animation.channels)
        {
            referencesValid &= channel.samplerIndex < animation.samplers.size()
                            && (!channel.nodeIndex || *channel.nodeIndex < asset.nodes.size());
        }

        for (const fastgltf::AnimationSampler& sampler : animation.samplers)
        {
            referencesValid &=
                sampler.inputAccessor < asset.accessors.size() && sampler.outputAccessor < asset.accessors.size();
        }
    }

    for (const fastgltf::Skin& skin : asset.skins)
    {
        for (size_t joint : skin.joints)
        {
            referencesValid &= joint < asset.nodes.size();
        }

        if (skin.inverseBindMatrices && *skin.inverseBindMatrices < asset.accessors.size())
        {
            const fastgltf::Accessor& accessor  = asset.accessors[*skin.inverseBindMatrices];

            referencesValid                    &= accessor.type == fastgltf::AccessorType::Mat4
                            && accessor.componentType == fastgltf::ComponentType::Float && accessor.count >= skin.joints.size();
        }
    }

    for (const fastgltf::Mesh& mesh : asset.meshes)
    {
        for (const fastgltf::Primitive& primitive : mesh.primitives)
        {
            for (const FASTGLTF_FG_PMR_NS::SmallVector<fastgltf::Attribute, 4>& target : primitive.targets)
            {
                for (const fastgltf::Attribute& attribute : target)
                {
                    referencesValid &= attribute.accessorIndex < asset.accessors.size();

                    if (attribute.accessorIndex < asset.accessors.size())
                    {
                        const fastgltf::AccessorType type = asset.accessors[attribute.accessorIndex].type;

                        referencesValid &=
                            attribute.name.starts_with("TEXCOORD_") ? type == fastgltf::AccessorType::Vec2
                            : attribute.name.starts_with("COLOR_")
                                ? type == fastgltf::AccessorType::Vec3 || type == fastgltf::AccessorType::Vec4
                                : (attribute.name != "POSITION" && attribute.name != "NORMAL" && attribute.name != "TANGENT")
                                      || type == fastgltf::AccessorType::Vec3;

                        const fastgltf::Attribute* position = primitive.findAttribute("POSITION");

                        if (position != primitive.attributes.end() && position->accessorIndex < asset.accessors.size())
                        {
                            referencesValid &= asset.accessors[attribute.accessorIndex].count
                                            == asset.accessors[position->accessorIndex].count;
                        }
                    }
                }
            }
        }
    }

    // fastgltf 0.9.1 compares float cone angles with a long-double pi/2. A valid
    // 90-degree angle rounds slightly above that limit. Validate the adjacent float
    // and then restore the exact authored value for the renderer.
    HeapVector<std::pair<size_t, float>> coneLimits;

    for (size_t index = 0; index < asset.lights.size(); ++index)
    {
        fastgltf::Light& light = asset.lights[index];

        if (light.outerConeAngle && *light.outerConeAngle == glm::half_pi<float>())
        {
            coneLimits.push_back({index, *light.outerConeAngle});

            light.outerConeAngle = std::nextafter(glm::half_pi<float>(), 0.0f);
        }
    }

    const fastgltf::Error result = referencesValid ? fastgltf::validate(asset) : fastgltf::Error::InvalidGltf;

    for (const std::pair<size_t, float>& cone : coneLimits)
    {
        asset.lights[cone.first].outerConeAngle = cone.second;
    }

    return result;
}

FastGLTFLoader::FastGLTFLoader()
{
    // TODO(glTF KHR_gaussian_splatting): import splat attributes and add a Gaussian renderer.
    // TODO(glTF KHR_interactivity): execute behavior graphs and connect their scene mutations.
    // TODO(glTF EXT_lights_ies): decode IES profiles and apply angular light distributions.
    // TODO(glTF EXT_mesh_manifold): retain manifold topology and implement its mesh semantics.
    // TODO(glTF vendor systems): add MSFT_lod, MSFT_texture_dds, MSFT packing and remaining
    // vendor-specific runtime handlers. Preserve optional metadata; reject required use by name.
    // TODO(glTF KHR_accessor_float64): add double-precision animation interpolation and
    // high-precision geometry conversion before claiming support for required float64 assets.
    static constexpr fastgltf::Extensions supportedExtensions{
        fastgltf::Extensions::KHR_materials_emissive_strength | fastgltf::Extensions::KHR_lights_punctual
        | fastgltf::Extensions::KHR_mesh_quantization | fastgltf::Extensions::KHR_texture_transform
        | fastgltf::Extensions::KHR_materials_unlit | fastgltf::Extensions::KHR_materials_specular
        | fastgltf::Extensions::KHR_materials_ior | fastgltf::Extensions::KHR_materials_clearcoat
        | fastgltf::Extensions::KHR_materials_sheen | fastgltf::Extensions::KHR_materials_transmission
        | fastgltf::Extensions::KHR_materials_volume | fastgltf::Extensions::KHR_materials_iridescence
        | fastgltf::Extensions::KHR_materials_anisotropy | fastgltf::Extensions::KHR_materials_dispersion
        | fastgltf::Extensions::KHR_materials_diffuse_transmission | fastgltf::Extensions::KHR_materials_pbrSpecularGlossiness
        | fastgltf::Extensions::KHR_draco_mesh_compression | fastgltf::Extensions::EXT_meshopt_compression
        | fastgltf::Extensions::KHR_meshopt_compression | fastgltf::Extensions::KHR_texture_basisu
        | fastgltf::Extensions::EXT_texture_webp | fastgltf::Extensions::EXT_mesh_gpu_instancing
        | fastgltf::Extensions::KHR_materials_variants};

    m_gltfParser = fastgltf::Parser(supportedExtensions);

    m_loadOptions =
        fastgltf::Options::LoadExternalBuffers | fastgltf::Options::LoadExternalImages | fastgltf::Options::GenerateMeshIndices;
}

static void NormalizeWindowsFileUris(const simdjson::dom::element& document, std::string& normalized)
{
#if defined(_WIN32)
    for (const char* collection : {"buffers", "images"})
    {
        simdjson::dom::array entries;

        if (document[collection].get_array().get(entries) == simdjson::SUCCESS)
        {
            for (simdjson::dom::element entry : entries)
            {
                std::string_view uri;

                if (entry["uri"].get_string().get(uri) == simdjson::SUCCESS && uri.size() >= 11 && uri.starts_with("file:///")
                    && std::isalpha(static_cast<unsigned char>(uri[8])) && uri[9] == ':' && uri[10] == '/')
                {
                    if (normalized.empty())
                    {
                        normalized = simdjson::to_string(document);
                    }

                    const std::string original = simdjson::to_string(entry["uri"]);

                    // fastgltf's Windows URI adapter otherwise treats /D:/ as a path
                    // rooted on the current drive rather than the file URI's D: drive.
                    const std::string replacement = "\"file:" + original.substr(9);

                    size_t offset                 = normalized.find(original);

                    while (offset != std::string::npos)
                    {
                        normalized.replace(offset, original.size(), replacement);

                        offset = normalized.find(original, offset + replacement.size());
                    }
                }
            }
        }
    }
#endif
}

static std::string NormalizeIntegralJsonNumber(std::string_view number)
{
    std::string result(number);

    const size_t exponentPosition = number.find_first_of("eE");

    const size_t mantissaEnd      = exponentPosition == std::string_view::npos ? number.size() : exponentPosition;

    const size_t decimalPosition  = number.find('.');

    if (decimalPosition != std::string_view::npos || exponentPosition != std::string_view::npos)
    {
        const bool negative       = number.front() == '-';

        const size_t integerBegin = negative ? 1 : 0;

        const size_t integerEnd   = decimalPosition == std::string_view::npos ? mantissaEnd : decimalPosition;

        std::string digits;

        bool valid =
            integerEnd > integerBegin && integerEnd <= mantissaEnd
            && (integerEnd - integerBegin == 1 || number[integerBegin] != '0')
            && (decimalPosition == std::string_view::npos
                || (decimalPosition + 1 < mantissaEnd && number.find('.', decimalPosition + 1) == std::string_view::npos));

        for (size_t index = negative ? 1 : 0; index < mantissaEnd; ++index)
        {
            if (number[index] != '.')
            {
                valid  &= number[index] >= '0' && number[index] <= '9';

                digits += number[index];
            }
        }

        uint64_t exponent     = 0;

        bool negativeExponent = false;

        bool exponentOverflow = false;

        if (exponentPosition != std::string_view::npos)
        {
            size_t begin = exponentPosition + 1;

            if (begin < number.size() && (number[begin] == '-' || number[begin] == '+'))
            {
                negativeExponent = number[begin] == '-';

                ++begin;
            }

            const std::from_chars_result parsed =
                std::from_chars(number.data() + begin, number.data() + number.size(), exponent);

            exponentOverflow  = parsed.ec == std::errc::result_out_of_range;

            valid            &= begin < number.size() && (parsed.ec == std::errc{} || exponentOverflow)
                  && parsed.ptr == number.data() + number.size();
        }

        const size_t first = digits.find_first_not_of('0');

        if (valid && !digits.empty() && first == std::string::npos)
        {
            result = negative ? "-0" : "0";
        }
        else if (valid && !exponentOverflow && first != std::string::npos)
        {
            digits.erase(0, first);

            const size_t fraction = decimalPosition == std::string_view::npos ? 0 : mantissaEnd - decimalPosition - 1;

            size_t removed        = 0;

            if (negativeExponent)
            {
                valid &= exponent <= digits.size() && fraction <= digits.size() && exponent <= digits.size() - fraction;

                if (valid)
                {
                    removed = fraction + static_cast<size_t>(exponent);
                }
            }
            else if (exponent < fraction)
            {
                removed = fraction - static_cast<size_t>(exponent);
            }
            else
            {
                const uint64_t added  = exponent - fraction;

                valid                &= added <= 20 && digits.size() <= 20 - added;

                if (valid)
                {
                    digits.append(static_cast<size_t>(added), '0');
                }
            }

            valid &= removed <= digits.size();

            if (valid && removed != 0)
            {
                const size_t suffix  = digits.size() - removed;

                valid               &= digits.find_first_not_of('0', suffix) == std::string::npos;

                digits.resize(suffix);
            }

            uint64_t value                       = 0;

            const std::from_chars_result parsed  = std::from_chars(digits.data(), digits.data() + digits.size(), value);

            valid                               &= parsed.ec == std::errc{} && parsed.ptr == digits.data() + digits.size()
                  && (!negative || value <= uint64_t(INT64_MAX) + 1);

            if (valid)
            {
                result = (negative ? "-" : "") + digits;
            }
        }
    }

    return result;
}

static std::string NormalizeIntegralJsonNumbers(const std::string& json)
{
    std::string result;

    result.reserve(json.size());

    bool quoted  = false;

    bool escaped = false;

    for (size_t index = 0; index < json.size(); ++index)
    {
        const char character = json[index];

        if (!quoted && (character == '-' || (character >= '0' && character <= '9')))
        {
            const size_t first = index;

            while (index + 1 < json.size()
                   && std::string_view("0123456789.eE+-").find(json[index + 1]) != std::string_view::npos)
            {
                ++index;
            }

            result += NormalizeIntegralJsonNumber(std::string_view(json).substr(first, index - first + 1));
        }
        else
        {
            result += character;

            if (quoted && escaped)
            {
                escaped = false;
            }
            else if (quoted && character == '\\')
            {
                escaped = true;
            }
            else if (character == '"')
            {
                quoted = !quoted;
            }
        }
    }

    return result;
}

static HeapVector<std::byte> PrepareExtensionParserInput(const std::string&        json,
                                                         const std::string&        sourceJson,
                                                         fastgltf::span<std::byte> fileBytes,
                                                         bool                      binary)
{
    HeapVector<std::byte> result;

    simdjson::dom::parser parser;

    simdjson::dom::element document;

    simdjson::dom::array required;

    std::string normalized = json != sourceJson ? json : std::string{};

    if (parser.parse(json).get(document) == simdjson::SUCCESS)
    {
        if (document["extensionsRequired"].get_array().get(required) == simdjson::SUCCESS)
        {
            static constexpr std::string_view parserExtensions[]{"KHR_materials_emissive_strength",
                                                                 "KHR_lights_punctual",
                                                                 "KHR_mesh_quantization",
                                                                 "KHR_texture_transform",
                                                                 "KHR_materials_unlit",
                                                                 "KHR_materials_specular",
                                                                 "KHR_materials_ior",
                                                                 "KHR_materials_clearcoat",
                                                                 "KHR_materials_sheen",
                                                                 "KHR_materials_transmission",
                                                                 "KHR_materials_volume",
                                                                 "KHR_materials_iridescence",
                                                                 "KHR_materials_anisotropy",
                                                                 "KHR_materials_dispersion",
                                                                 "KHR_materials_diffuse_transmission",
                                                                 "KHR_materials_pbrSpecularGlossiness",
                                                                 "KHR_draco_mesh_compression",
                                                                 "EXT_meshopt_compression",
                                                                 "KHR_meshopt_compression",
                                                                 "KHR_texture_basisu",
                                                                 "EXT_texture_webp",
                                                                 "EXT_mesh_gpu_instancing",
                                                                 "KHR_materials_variants"};

            std::string unsupported;

            bool engineExtension      = false;

            std::string requiredArray = "[";

            for (simdjson::dom::element entry : required)
            {
                std::string_view extension;

                if (entry.get_string().get(extension) == simdjson::SUCCESS)
                {
                    if (extension == "KHR_animation_pointer" || extension == "KHR_xmp_json_ld"
                        || extension == "EXT_lights_image_based" || extension == "KHR_materials_volume_scatter"
                        || extension == "KHR_materials_retroreflection" || extension == "KHR_node_visibility"
                        || extension == "KHR_node_selectability" || extension == "KHR_node_hoverability")
                    {
                        // The engine imports these payloads after fastgltf reads the ordinary asset.
                        engineExtension = true;
                    }
                    else
                    {
                        if (std::find(std::begin(parserExtensions), std::end(parserExtensions), extension)
                            == std::end(parserExtensions))
                        {
                            if (!unsupported.empty())
                            {
                                unsupported += ", ";
                            }

                            unsupported += extension;
                        }

                        if (requiredArray.size() > 1)
                        {
                            requiredArray += ',';
                        }

                        requiredArray += simdjson::to_string(entry);
                    }
                }
            }

            requiredArray += ']';

            if (!unsupported.empty())
            {
                LOG_ERROR_AND_THROW(fmt::format("Unsupported required glTF extension(s): {}", unsupported));
            }

            if (engineExtension)
            {
                // Replace only the root property; extras may contain the same key.
                normalized = "{";

                for (const simdjson::dom::key_value_pair field : document.get_object().value())
                {
                    if (normalized.size() > 1)
                    {
                        normalized += ',';
                    }

                    normalized += field.key == "extensionsRequired" ? "\"extensionsRequired\":" + requiredArray
                                                                    : simdjson::to_string(field);
                }

                normalized += '}';
            }
        }

        // Compose URI normalization with the structural extension rewrite.
        NormalizeWindowsFileUris(document, normalized);

        if (!normalized.empty())
        {
            if (binary)
            {
                while (normalized.size() % 4 != 0)
                {
                    normalized += ' ';
                }

                const size_t tailOffset = 20 + sourceJson.size();

                if (tailOffset > fileBytes.size() || normalized.size() > UINT32_MAX - fileBytes.size())
                {
                    LOG_ERROR_AND_THROW("Invalid glTF binary extension document size");
                }

                const uint32_t jsonLength = static_cast<uint32_t>(normalized.size());

                const uint32_t fileLength = static_cast<uint32_t>(20 + normalized.size() + fileBytes.size() - tailOffset);

                result.resize(fileLength);

                std::memcpy(result.data(), fileBytes.data(), 20);

                std::memcpy(result.data() + 8, &fileLength, sizeof(fileLength));

                std::memcpy(result.data() + 12, &jsonLength, sizeof(jsonLength));

                std::memcpy(result.data() + 20, normalized.data(), normalized.size());

                std::memcpy(result.data() + 20 + normalized.size(), fileBytes.data() + tailOffset,
                            fileBytes.size() - tailOffset);
            }
            else
            {
                result.resize(normalized.size());

                std::memcpy(result.data(), normalized.data(), normalized.size());
            }
        }
    }

    return result;
}

static void LoadNodeProperties(const std::string& json, fastgltf::Asset& asset)
{
    simdjson::dom::parser parser;

    simdjson::dom::element document;

    simdjson::dom::array nodes;

    if (parser.parse(json).get(document) == simdjson::SUCCESS && document["nodes"].get_array().get(nodes) == simdjson::SUCCESS)
    {
        size_t nodeIndex = 0;

        for (simdjson::dom::element node : nodes)
        {
            const char* paths[]{"/extensions/KHR_node_visibility/visible", "/extensions/KHR_node_selectability/selectable",
                                "/extensions/KHR_node_hoverability/hoverable"};

            fastgltf::Node& destination = asset.nodes[nodeIndex++];

            bool* flags[]{&destination.visible, &destination.selectable, &destination.hoverable};

            for (uint32_t index = 0; index < 3; ++index)
            {
                bool value                       = true;

                const simdjson::error_code error = node.at_pointer(paths[index]).get_bool().get(value);

                if (error != simdjson::SUCCESS && error != simdjson::NO_SUCH_FIELD)
                {
                    LOG_ERROR_AND_THROW("Invalid glTF node interaction flag");
                }

                *flags[index] = value;
            }
        }
    }
}

void FastGLTFLoader::LoadFromFile(const std::string& path, sg::Scene* pScene)
{
    if (pScene == nullptr)
    {
        LOG_ERROR_AND_THROW("A glTF import requires a destination scene");
    }

    const std::filesystem::path filePath = std::filesystem::u8path(path);

    std::string extension                = filePath.extension().string();

    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char value) { return static_cast<char>(std::tolower(value)); });

    if (extension != ".gltf" && extension != ".glb")
    {
        LOG_ERROR_AND_THROW(fmt::format("Unsupported scene format '{}'; expected .gltf or .glb", extension));
    }

    const std::u8string name = filePath.stem().generic_u8string();

    m_name.assign(reinterpret_cast<const char*>(name.data()), name.size());

    fastgltf::Expected<fastgltf::MappedGltfFile> gltfFile = fastgltf::MappedGltfFile::FromPath(filePath);

    if (!bool(gltfFile))
    {
        LOG_ERROR_AND_THROW(fmt::format("Failed to open glTF file: {}", fastgltf::getErrorMessage(gltfFile.error())));
    }
    else
    {
        const fastgltf::span<std::byte> fileBytes = static_cast<fastgltf::span<std::byte>>(gltfFile.get());

        size_t jsonOffset                         = 0;

        size_t jsonLength                         = fileBytes.size();

        if (extension == ".glb" && fileBytes.size() >= 20)
        {
            uint32_t length = 0;

            std::memcpy(&length, fileBytes.data() + 12, sizeof(length));

            jsonOffset = 20;

            jsonLength = length;
        }

        if (jsonOffset > fileBytes.size() || jsonLength > fileBytes.size() - jsonOffset)
        {
            LOG_ERROR_AND_THROW("Invalid glTF JSON chunk length");
        }

        m_sourceJson.assign(reinterpret_cast<const char*>(fileBytes.data() + jsonOffset), jsonLength);

        // glTF integer properties may use decimal or exponent notation. fastgltf's
        // uint64 reader requires integer tokens, so normalize exact integral values
        // lexically without rounding fractional values or changing quoted strings.
        m_json                            = NormalizeIntegralJsonNumbers(m_sourceJson);

        HeapVector<std::byte> parserBytes = PrepareExtensionParserInput(m_json, m_sourceJson, fileBytes, extension == ".glb");

        // simdjson reads SIMDJSON_PADDING bytes past the JSON. A mapped file cannot
        // guarantee that padding at a page boundary; copy into fastgltf's padded buffer.
        fastgltf::Expected<fastgltf::GltfDataBuffer> input =
            fastgltf::GltfDataBuffer::FromBytes(parserBytes.empty() ? fileBytes.data() : parserBytes.data(),
                                                parserBytes.empty() ? fileBytes.size() : parserBytes.size());

        if (input.error() != fastgltf::Error::None)
        {
            LOG_ERROR_AND_THROW("Failed to allocate padded glTF parser input");
        }

        fastgltf::GltfDataBuffer parserData = std::move(input.get());

        fastgltf::Expected<fastgltf::Asset> loadedAsset =
            m_gltfParser.loadGltf(parserData, filePath.parent_path(), m_loadOptions);

        if (loadedAsset.error() != fastgltf::Error::None)
        {
            LOG_ERROR_AND_THROW(fmt::format("Failed to load glTF: {}", fastgltf::getErrorMessage(loadedAsset.error())));
        }
        else
        {
            m_gltfAsset = std::move(loadedAsset.get());

            LoadNodeProperties(m_json, m_gltfAsset);

            DecodeMeshoptBuffers(m_gltfAsset);

            DecodeDracoMeshes(m_gltfAsset);

            const fastgltf::Error validation = ValidateSceneAsset(m_gltfAsset);

            if (validation != fastgltf::Error::None)
            {
                LOG_ERROR_AND_THROW(fmt::format("Invalid glTF scene: {}", fastgltf::getErrorMessage(validation)));
            }

            std::vector<Vertex> previousVertices  = std::move(m_vertices);

            std::vector<uint32_t> previousIndices = std::move(m_indices);

            try
            {
                sg::Scene importedScene;

                importedScene.SetName(m_name);

                LoadGltfSamplers(&importedScene);

                LoadGltfTextures(&importedScene);

                LoadGltfMaterials(&importedScene);

                LoadGltfMeshes(&importedScene);

                LoadGltfAssetData(&importedScene);

                LoadGltfRenderableNodes(&importedScene);

                HeapVector<Vertex> posedVertices;

                if (!sg::ApplySceneDeformations(importedScene,
                                                VectorView<const Vertex>(importedScene.GetAssetData().bindVertices.data(),
                                                                         importedScene.GetAssetData().bindVertices.size()),
                                                posedVertices))
                {
                    LOG_ERROR_AND_THROW("Failed to apply glTF initial skin or morph deformation");
                }

                m_vertices.assign(posedVertices.begin(), posedVertices.end());

                importedScene.UpdateAABB();

                *pScene = std::move(importedScene);
            }
            catch (...)
            {
                m_vertices = std::move(previousVertices);

                m_indices  = std::move(previousIndices);

                throw;
            }
        }
    }
}

void FastGLTFLoader::LoadGltfSamplers(sg::Scene* pScene)
{ // Load Samplers
    zen::HeapVector<UniquePtr<sg::Sampler>> samplers;

    samplers.reserve(m_gltfAsset.samplers.size());

    using SamplerKey = std::tuple<sg::TextureFilter, sg::TextureFilter, sg::TextureFilter, sg::SamplerAddressMode,
                                  sg::SamplerAddressMode, bool>;

    std::map<SamplerKey, uint32_t> canonicalSamplers;

    m_samplerIndices.resize(m_gltfAsset.samplers.size());

    uint32_t sourceIndex = 0;

    for (const fastgltf::Sampler& s : m_gltfAsset.samplers)
    {
        UniquePtr<sg::Sampler> sampler = MakeUnique<sg::Sampler>(std::string(s.name));

        sg::Sampler* pSampler          = sampler.Get();

        // set props
        pSampler->minFilter              = FromFastGltfFilter(s.minFilter);

        pSampler->magFilter              = FromFastGltfFilter(s.magFilter);

        const fastgltf::Filter minFilter = s.minFilter.value_or(fastgltf::Filter::Linear);

        pSampler->useMipmaps             = minFilter != fastgltf::Filter::Nearest && minFilter != fastgltf::Filter::Linear;

        pSampler->mipFilter =
            minFilter == fastgltf::Filter::NearestMipMapNearest || minFilter == fastgltf::Filter::LinearMipMapNearest
                ? sg::TextureFilter::Nearest
                : sg::TextureFilter::Linear;

        pSampler->wrapS = FromFastGltfWrap(s.wrapS);

        pSampler->wrapT = FromFastGltfWrap(s.wrapT);

        const SamplerKey key(pSampler->minFilter, pSampler->magFilter, pSampler->mipFilter, pSampler->wrapS, pSampler->wrapT,
                             pSampler->useMipmaps);

        const std::map<SamplerKey, uint32_t>::const_iterator found = canonicalSamplers.find(key);

        if (found == canonicalSamplers.end())
        {
            const uint32_t canonical = static_cast<uint32_t>(samplers.size());

            canonicalSamplers.emplace(key, canonical);

            samplers.push_back(std::move(sampler));

            m_samplerIndices[sourceIndex] = canonical;
        }
        else
        {
            m_samplerIndices[sourceIndex] = found->second;
        }

        ++sourceIndex;
    }

    pScene->SetComponents(std::move(samplers));
}

using TextureRole = std::pair<const fastgltf::TextureInfo*, bool>;

static HeapVector<TextureRole> GetMaterialTextureRoles(const fastgltf::Material& material)
{
    HeapVector<TextureRole> roles;

    roles.push_back({material.pbrData.baseColorTexture ? &*material.pbrData.baseColorTexture : nullptr, true});

    roles.push_back({material.emissiveTexture ? &*material.emissiveTexture : nullptr, true});

    roles.push_back({material.pbrData.metallicRoughnessTexture ? &*material.pbrData.metallicRoughnessTexture : nullptr, false});

    roles.push_back({material.normalTexture ? &*material.normalTexture : nullptr, false});

    roles.push_back({material.occlusionTexture ? &*material.occlusionTexture : nullptr, false});

    if (material.specular)
    {
        roles.push_back({material.specular->specularTexture ? &*material.specular->specularTexture : nullptr, false});

        roles.push_back({material.specular->specularColorTexture ? &*material.specular->specularColorTexture : nullptr, true});
    }

    if (material.specularGlossiness)
    {
        roles.push_back(
            {material.specularGlossiness->diffuseTexture ? &*material.specularGlossiness->diffuseTexture : nullptr, true});

        roles.push_back({material.specularGlossiness->specularGlossinessTexture
                             ? &*material.specularGlossiness->specularGlossinessTexture
                             : nullptr,
                         true});
    }

    if (material.clearcoat)
    {
        roles.push_back({material.clearcoat->clearcoatTexture ? &*material.clearcoat->clearcoatTexture : nullptr, false});

        roles.push_back(
            {material.clearcoat->clearcoatRoughnessTexture ? &*material.clearcoat->clearcoatRoughnessTexture : nullptr, false});

        roles.push_back(
            {material.clearcoat->clearcoatNormalTexture ? &*material.clearcoat->clearcoatNormalTexture : nullptr, false});
    }

    if (material.sheen)
    {
        roles.push_back({material.sheen->sheenColorTexture ? &*material.sheen->sheenColorTexture : nullptr, true});

        roles.push_back({material.sheen->sheenRoughnessTexture ? &*material.sheen->sheenRoughnessTexture : nullptr, false});
    }

    if (material.transmission)
    {
        roles.push_back(
            {material.transmission->transmissionTexture ? &*material.transmission->transmissionTexture : nullptr, false});
    }

    if (material.volume)
    {
        roles.push_back({material.volume->thicknessTexture ? &*material.volume->thicknessTexture : nullptr, false});
    }

    if (material.iridescence)
    {
        roles.push_back(
            {material.iridescence->iridescenceTexture ? &*material.iridescence->iridescenceTexture : nullptr, false});

        roles.push_back(
            {material.iridescence->iridescenceThicknessTexture ? &*material.iridescence->iridescenceThicknessTexture : nullptr,
             false});
    }

    if (material.anisotropy)
    {
        roles.push_back({material.anisotropy->anisotropyTexture ? &*material.anisotropy->anisotropyTexture : nullptr, false});
    }

    if (material.diffuseTransmission)
    {
        roles.push_back({material.diffuseTransmission->diffuseTransmissionTexture
                             ? &*material.diffuseTransmission->diffuseTransmissionTexture
                             : nullptr,
                         false});

        roles.push_back({material.diffuseTransmission->diffuseTransmissionColorTexture
                             ? &*material.diffuseTransmission->diffuseTransmissionColorTexture
                             : nullptr,
                         true});
    }

    return roles;
}

static sg::MaterialTextureBinding LoadTextureBinding(const fastgltf::TextureInfo*         source,
                                                     bool                                 color,
                                                     const zen::HeapVector<sg::Texture*>& textures,
                                                     const HeapVector<uint32_t>&          textureIndices,
                                                     const HeapVector<uint32_t>&          linearIndices,
                                                     const std::map<size_t, uint32_t>&    texCoordIndices)
{
    sg::MaterialTextureBinding binding;

    if (source != nullptr)
    {
        if (source->textureIndex >= linearIndices.size())
        {
            LOG_ERROR_AND_THROW("glTF material references an invalid texture index");
        }

        binding.texture = textures[color ? textureIndices[source->textureIndex] : linearIndices[source->textureIndex]];

        const size_t coordinateSet =
            source->transform && source->transform->texCoordIndex ? *source->transform->texCoordIndex : source->texCoordIndex;

        const std::map<size_t, uint32_t>::const_iterator coordinate = texCoordIndices.find(coordinateSet);

        binding.texCoord = coordinate == texCoordIndices.end() ? 0 : coordinate->second;

        if (source->transform != nullptr)
        {
            const fastgltf::TextureTransform& transform = *source->transform;

            binding.transform.uvOffset                  = glm::make_vec2(transform.uvOffset.data());

            binding.transform.uvScale                   = glm::make_vec2(transform.uvScale.data());

            binding.transform.uvRotation                = transform.rotation;

            const float cosine                          = std::cos(transform.rotation);

            const float sine                            = std::sin(transform.rotation);

            binding.transform.row0 =
                Vec4(cosine * transform.uvScale[0], -sine * transform.uvScale[1], transform.uvOffset[0], 0);

            binding.transform.row1 = Vec4(sine * transform.uvScale[0], cosine * transform.uvScale[1], transform.uvOffset[1], 0);
        }

        binding.transform.row0.w = static_cast<float>(binding.texture->samplerIndex + 1);
    }

    return binding;
}

static void LoadMaterialFeatures(const fastgltf::Material&            source,
                                 sg::Material&                        material,
                                 const zen::HeapVector<sg::Texture*>& textures,
                                 const HeapVector<uint32_t>&          textureIndices,
                                 const HeapVector<uint32_t>&          linearIndices,
                                 const std::map<size_t, uint32_t>&    texCoordIndices)
{
    sg::MaterialFeatures& features = material.features;

    features.ior                   = source.ior;

    features.dispersion            = source.dispersion;

    if (source.specular)
    {
        features.specular      = source.specular->specularFactor;

        features.specularColor = glm::make_vec3(source.specular->specularColorFactor.data());

        features.specularTexture =
            LoadTextureBinding(source.specular->specularTexture ? &*source.specular->specularTexture : nullptr, false, textures,
                               textureIndices, linearIndices, texCoordIndices);

        features.specularColorTexture =
            LoadTextureBinding(source.specular->specularColorTexture ? &*source.specular->specularColorTexture : nullptr, true,
                               textures, textureIndices, linearIndices, texCoordIndices);
    }

    if (source.specularGlossiness)
    {
        material.pbrWorkflows.metallicRoughness  = false;

        material.pbrWorkflows.specularGlossiness = true;

        material.extension.diffuseFactor         = glm::make_vec4(source.specularGlossiness->diffuseFactor.data());

        material.extension.specularFactor        = glm::make_vec3(source.specularGlossiness->specularFactor.data());

        features.glossiness                      = source.specularGlossiness->glossinessFactor;

        features.diffuseTexture                  = LoadTextureBinding(
            source.specularGlossiness->diffuseTexture ? &*source.specularGlossiness->diffuseTexture : nullptr, true, textures,
            textureIndices, linearIndices, texCoordIndices);

        features.specularGlossinessTexture = LoadTextureBinding(source.specularGlossiness->specularGlossinessTexture
                                                                    ? &*source.specularGlossiness->specularGlossinessTexture
                                                                    : nullptr,
                                                                true, textures, textureIndices, linearIndices, texCoordIndices);

        material.extension.pDiffuseTexture            = features.diffuseTexture.texture;

        material.extension.pSpecularGlossinessTexture = features.specularGlossinessTexture.texture;
    }

    if (source.clearcoat)
    {
        features.clearcoat          = source.clearcoat->clearcoatFactor;

        features.clearcoatRoughness = source.clearcoat->clearcoatRoughnessFactor;

        features.clearcoatTexture =
            LoadTextureBinding(source.clearcoat->clearcoatTexture ? &*source.clearcoat->clearcoatTexture : nullptr, false,
                               textures, textureIndices, linearIndices, texCoordIndices);

        features.clearcoatRoughnessTexture = LoadTextureBinding(
            source.clearcoat->clearcoatRoughnessTexture ? &*source.clearcoat->clearcoatRoughnessTexture : nullptr, false,
            textures, textureIndices, linearIndices, texCoordIndices);

        features.clearcoatNormalTexture =
            LoadTextureBinding(source.clearcoat->clearcoatNormalTexture ? &*source.clearcoat->clearcoatNormalTexture : nullptr,
                               false, textures, textureIndices, linearIndices, texCoordIndices);

        features.clearcoatNormalTexture.scale =
            source.clearcoat->clearcoatNormalTexture ? source.clearcoat->clearcoatNormalTexture->scale : 1.0f;
    }

    if (source.sheen)
    {
        features.sheenColor     = glm::make_vec3(source.sheen->sheenColorFactor.data());

        features.sheenRoughness = source.sheen->sheenRoughnessFactor;

        features.sheenColorTexture =
            LoadTextureBinding(source.sheen->sheenColorTexture ? &*source.sheen->sheenColorTexture : nullptr, true, textures,
                               textureIndices, linearIndices, texCoordIndices);

        features.sheenRoughnessTexture =
            LoadTextureBinding(source.sheen->sheenRoughnessTexture ? &*source.sheen->sheenRoughnessTexture : nullptr, false,
                               textures, textureIndices, linearIndices, texCoordIndices);
    }

    if (source.transmission)
    {
        features.transmission = source.transmission->transmissionFactor;

        features.transmissionTexture =
            LoadTextureBinding(source.transmission->transmissionTexture ? &*source.transmission->transmissionTexture : nullptr,
                               false, textures, textureIndices, linearIndices, texCoordIndices);
    }

    if (source.volume)
    {
        features.thickness           = source.volume->thicknessFactor;

        features.attenuationColor    = glm::make_vec3(source.volume->attenuationColor.data());

        features.attenuationDistance = source.volume->attenuationDistance;

        features.thicknessTexture =
            LoadTextureBinding(source.volume->thicknessTexture ? &*source.volume->thicknessTexture : nullptr, false, textures,
                               textureIndices, linearIndices, texCoordIndices);
    }

    if (source.iridescence)
    {
        features.iridescence             = source.iridescence->iridescenceFactor;

        features.iridescenceIor          = source.iridescence->iridescenceIor;

        features.iridescenceThicknessMin = source.iridescence->iridescenceThicknessMinimum;

        features.iridescenceThicknessMax = source.iridescence->iridescenceThicknessMaximum;

        features.iridescenceTexture =
            LoadTextureBinding(source.iridescence->iridescenceTexture ? &*source.iridescence->iridescenceTexture : nullptr,
                               false, textures, textureIndices, linearIndices, texCoordIndices);

        features.iridescenceThicknessTexture = LoadTextureBinding(
            source.iridescence->iridescenceThicknessTexture ? &*source.iridescence->iridescenceThicknessTexture : nullptr,
            false, textures, textureIndices, linearIndices, texCoordIndices);
    }

    if (source.anisotropy)
    {
        features.anisotropy         = source.anisotropy->anisotropyStrength;

        features.anisotropyRotation = source.anisotropy->anisotropyRotation;

        features.anisotropyTexture =
            LoadTextureBinding(source.anisotropy->anisotropyTexture ? &*source.anisotropy->anisotropyTexture : nullptr, false,
                               textures, textureIndices, linearIndices, texCoordIndices);
    }

    if (source.diffuseTransmission)
    {
        features.diffuseTransmission        = source.diffuseTransmission->diffuseTransmissionFactor;

        features.diffuseTransmissionColor   = glm::make_vec3(source.diffuseTransmission->diffuseTransmissionColorFactor.data());

        features.diffuseTransmissionTexture = LoadTextureBinding(
            source.diffuseTransmission->diffuseTransmissionTexture ? &*source.diffuseTransmission->diffuseTransmissionTexture
                                                                   : nullptr,
            false, textures, textureIndices, linearIndices, texCoordIndices);

        features.diffuseTransmissionColorTexture =
            LoadTextureBinding(source.diffuseTransmission->diffuseTransmissionColorTexture
                                   ? &*source.diffuseTransmission->diffuseTransmissionColorTexture
                                   : nullptr,
                               true, textures, textureIndices, linearIndices, texCoordIndices);
    }
}

static HeapVector<uint8_t> DecodeImagePixels(const uint8_t*                   data,
                                             size_t                           size,
                                             uint32_t&                        width,
                                             uint32_t&                        height,
                                             HeapVector<HeapVector<uint8_t>>* mipBytes = nullptr)
{
    HeapVector<uint8_t> pixels;

    static const uint8_t ktx2Identifier[]{0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A};

    if (size >= sizeof(ktx2Identifier) && std::memcmp(data, ktx2Identifier, sizeof(ktx2Identifier)) == 0)
    {
        static std::once_flag initializeBasis;

        std::call_once(initializeBasis, basist::basisu_transcoder_init);

        basist::ktx2_transcoder transcoder;

        if (size > UINT32_MAX || !transcoder.init(data, static_cast<uint32_t>(size)) || !transcoder.start_transcoding())
        {
            LOG_ERROR_AND_THROW("Failed to initialize KTX2 Basis Universal transcoder");
        }

        width  = transcoder.get_width();

        height = transcoder.get_height();

        if (width == 0 || height == 0 || size_t(width) * height > UINT32_MAX / 4)
        {
            LOG_ERROR_AND_THROW("Invalid KTX2 image dimensions");
        }

        pixels.resize(size_t(width) * height * 4);

        if (!transcoder.transcode_image_level(0, 0, 0, pixels.data(), width * height,
                                              basist::transcoder_texture_format::cTFRGBA32))
        {
            LOG_ERROR_AND_THROW("Failed to transcode KTX2 Basis Universal image");
        }

        if (mipBytes != nullptr)
        {
            mipBytes->push_back(pixels);

            for (uint32_t level = 1; level < transcoder.get_levels(); ++level)
            {
                const uint32_t levelWidth  = std::max(1u, width >> level);

                const uint32_t levelHeight = std::max(1u, height >> level);

                HeapVector<uint8_t> mip(size_t(levelWidth) * levelHeight * 4);

                if (!transcoder.transcode_image_level(level, 0, 0, mip.data(), levelWidth * levelHeight,
                                                      basist::transcoder_texture_format::cTFRGBA32))
                {
                    LOG_ERROR_AND_THROW("Failed to transcode KTX2 Basis Universal mip level");
                }

                mipBytes->push_back(std::move(mip));
            }
        }
    }
    else
    {
        int imageWidth  = 0;

        int imageHeight = 0;

        if (WebPGetInfo(data, size, &imageWidth, &imageHeight))
        {
            width  = static_cast<uint32_t>(imageWidth);

            height = static_cast<uint32_t>(imageHeight);

            pixels.resize(size_t(width) * height * 4);

            if (WebPDecodeRGBAInto(data, size, pixels.data(), pixels.size(), imageWidth * 4) == nullptr)
            {
                LOG_ERROR_AND_THROW("Failed to decode WebP glTF image");
            }
        }
        else if (size <= static_cast<size_t>(std::numeric_limits<int>::max()))
        {
            int channels = 0;

            stbi_uc* decoded =
                stbi_load_from_memory(data, static_cast<int>(size), &imageWidth, &imageHeight, &channels, STBI_rgb_alpha);

            if (decoded != nullptr && imageWidth > 0 && imageHeight > 0)
            {
                width  = static_cast<uint32_t>(imageWidth);

                height = static_cast<uint32_t>(imageHeight);

                pixels = HeapVector<uint8_t>(decoded, size_t(width) * height * 4);
            }

            stbi_image_free(decoded);
        }
    }

    return pixels;
}

static fastgltf::span<const std::byte> GetImageBytes(const fastgltf::Asset& asset,
                                                     const fastgltf::Image& gltfImage,
                                                     HeapVector<std::byte>& fileBytes)
{
    fastgltf::span<const std::byte> bytes;

    if (const fastgltf::sources::URI* file = std::get_if<fastgltf::sources::URI>(&gltfImage.data))
    {
        const std::string path(file->uri.path().begin(), file->uri.path().end());

        std::ifstream stream(std::filesystem::u8path(path), std::ios::binary | std::ios::ate);

        if (stream)
        {
            const std::streamoff length = stream.tellg();

            if (length >= 0)
            {
                fileBytes.resize(static_cast<size_t>(length));

                stream.seekg(0);

                stream.read(reinterpret_cast<char*>(fileBytes.data()), length);

                if (stream)
                {
                    bytes = fastgltf::span<const std::byte>(fileBytes.data(), fileBytes.size());
                }
            }
        }
    }
    else if (const fastgltf::sources::Array* array = std::get_if<fastgltf::sources::Array>(&gltfImage.data))
    {
        bytes = fastgltf::span<const std::byte>(array->bytes.data(), array->bytes.size());
    }
    else if (const fastgltf::sources::Vector* vector = std::get_if<fastgltf::sources::Vector>(&gltfImage.data))
    {
        bytes = fastgltf::span<const std::byte>(vector->bytes.data(), vector->bytes.size());
    }
    else if (const fastgltf::sources::ByteView* byteView = std::get_if<fastgltf::sources::ByteView>(&gltfImage.data))
    {
        bytes = byteView->bytes;
    }
    else if (const fastgltf::sources::BufferView* view = std::get_if<fastgltf::sources::BufferView>(&gltfImage.data))
    {
        if (view->bufferViewIndex < asset.bufferViews.size())
        {
            const fastgltf::BufferView& bufferView = asset.bufferViews[view->bufferViewIndex];

            bytes = GetBufferBytes(asset, bufferView.bufferIndex, bufferView.byteOffset, bufferView.byteLength);
        }
    }

    return bytes;
}

sg::Texture* FastGLTFLoader::LoadGltfTextureVisitor(uint32_t textureIndex)
{
    const fastgltf::Texture& gltfTexture    = m_gltfAsset.textures[textureIndex];

    const fastgltf::Optional<size_t> source = gltfTexture.basisuImageIndex ? gltfTexture.basisuImageIndex
                                            : gltfTexture.webpImageIndex   ? gltfTexture.webpImageIndex
                                                                           : gltfTexture.imageIndex;

    if (!source || *source >= m_gltfAsset.images.size())
    {
        LOG_ERROR_AND_THROW("glTF texture has no valid image source");
    }

    const fastgltf::Image& gltfImage = m_gltfAsset.images[*source];

    const std::string textureName    = m_name + "Texture_" + std::to_string(textureIndex);

    const int samplerIndex = gltfTexture.samplerIndex ? static_cast<int>(m_samplerIndices[*gltfTexture.samplerIndex]) : -1;

    HeapVector<std::byte> fileBytes;

    const fastgltf::span<const std::byte> bytes = GetImageBytes(m_gltfAsset, gltfImage, fileBytes);

    HeapVector<HeapVector<uint8_t>> mipBytes;

    uint32_t width  = 0;

    uint32_t height = 0;

    HeapVector<uint8_t> pixels =
        DecodeImagePixels(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), width, height, &mipBytes);

    if (pixels.empty() || width == 0 || height == 0)
    {
        LOG_ERROR_AND_THROW(fmt::format("Failed to decode glTF texture '{}'", textureName));
    }

    TextureInfo info(width, height, m_textureFormats[textureIndex], std::vector<uint8_t>(pixels.begin(), pixels.end()),
                     samplerIndex);

    UniquePtr<sg::Texture> texture = MakeUnique<sg::Texture>(textureName);

    texture->Init(m_textureIndices[textureIndex], info);

    texture->mipBytes   = std::move(mipBytes);

    sg::Texture* result = texture.Get();

    texture.Release();

    return result;
}

HeapVector<UniquePtr<sg::Texture>> FastGLTFLoader::LoadGltfTextureBatch(uint32_t begin, uint32_t end)
{
    HeapVector<UniquePtr<sg::Texture>> textures;

    textures.reserve(end - begin);

    for (uint32_t index = begin; index < end; ++index)
    {
        textures.emplace_back(LoadGltfTextureVisitor(m_uniqueTextureIndices[index]));
    }

    return textures;
}

static HeapVector<uint32_t> GetAdditionalLinearTextures(const std::string& json)
{
    HeapVector<uint32_t> textures;

    simdjson::dom::parser parser;

    simdjson::dom::element document;

    simdjson::dom::array materials;

    if (parser.parse(json).get(document) == simdjson::SUCCESS
        && document["materials"].get_array().get(materials) == simdjson::SUCCESS)
    {
        for (simdjson::dom::element material : materials)
        {
            uint64_t texture = 0;

            if (material.at_pointer("/extensions/KHR_materials_retroreflection/retroreflectionTexture/index")
                    .get_uint64()
                    .get(texture)
                == simdjson::SUCCESS)
            {
                textures.push_back(static_cast<uint32_t>(texture));
            }
        }
    }

    return textures;
}

void FastGLTFLoader::LoadGltfTextures(sg::Scene* pScene)
{
    const size_t numSourceTextures = m_gltfAsset.textures.size();

    HeapVector<uint8_t> colorUsage(numSourceTextures, 0);

    HeapVector<uint8_t> linearUsage(numSourceTextures, 0);

    for (const fastgltf::Material& material : m_gltfAsset.materials)
    {
        const HeapVector<TextureRole> roles = GetMaterialTextureRoles(material);

        for (const TextureRole& role : roles)
        {
            if (role.first != nullptr)
            {
                if (role.first->textureIndex >= numSourceTextures)
                {
                    LOG_ERROR_AND_THROW("glTF material references an invalid texture");
                }

                (role.second ? colorUsage : linearUsage)[role.first->textureIndex] = 1;
            }
        }
    }

    for (uint32_t texture : GetAdditionalLinearTextures(m_json))
    {
        if (texture >= numSourceTextures)
        {
            LOG_ERROR_AND_THROW("glTF material extension references an invalid texture");
        }

        linearUsage[texture] = 1;
    }

    using TextureKey = std::tuple<size_t, int, Format>;

    std::map<TextureKey, uint32_t> canonicalTextures;

    HeapVector<std::pair<uint32_t, uint32_t>> linearClones;

    m_textureIndices.resize(numSourceTextures);

    m_linearTextureIndices.resize(numSourceTextures);

    m_textureFormats.resize(numSourceTextures);

    m_uniqueTextureIndices.clear();

    // glTF texture indices remain source bindings. Decode and upload each image,
    // sampler and color-space interpretation once, then map source bindings to it.
    for (uint32_t index = 0; index < numSourceTextures; ++index)
    {
        const fastgltf::Texture& texture        = m_gltfAsset.textures[index];

        const fastgltf::Optional<size_t> source = texture.basisuImageIndex ? texture.basisuImageIndex
                                                : texture.webpImageIndex   ? texture.webpImageIndex
                                                                           : texture.imageIndex;

        if (!source || *source >= m_gltfAsset.images.size())
        {
            LOG_ERROR_AND_THROW("glTF texture has no valid image source");
        }

        const int sampler   = texture.samplerIndex ? static_cast<int>(m_samplerIndices[*texture.samplerIndex]) : -1;

        const Format format = colorUsage[index] ? Format::R8G8B8A8_SRGB : Format::R8G8B8A8_UNORM;

        const TextureKey key(*source, sampler, format);

        std::map<TextureKey, uint32_t>::const_iterator found = canonicalTextures.find(key);

        if (found == canonicalTextures.end())
        {
            const uint32_t canonical = static_cast<uint32_t>(m_uniqueTextureIndices.size());

            canonicalTextures.emplace(key, canonical);

            m_uniqueTextureIndices.push_back(index);

            m_textureIndices[index] = canonical;
        }
        else
        {
            m_textureIndices[index] = found->second;
        }

        m_textureFormats[index]       = format;

        m_linearTextureIndices[index] = m_textureIndices[index];
    }

    const size_t numTextures = m_uniqueTextureIndices.size();

    for (uint32_t index = 0; index < numSourceTextures; ++index)
    {
        if (colorUsage[index] && linearUsage[index])
        {
            const fastgltf::Texture& texture = m_gltfAsset.textures[index];

            const size_t source              = texture.basisuImageIndex ? *texture.basisuImageIndex
                                             : texture.webpImageIndex   ? *texture.webpImageIndex
                                                                        : *texture.imageIndex;

            const int sampler = texture.samplerIndex ? static_cast<int>(m_samplerIndices[*texture.samplerIndex]) : -1;

            const TextureKey key(source, sampler, Format::R8G8B8A8_UNORM);

            const std::map<TextureKey, uint32_t>::const_iterator found = canonicalTextures.find(key);

            if (found == canonicalTextures.end())
            {
                const uint32_t canonical = static_cast<uint32_t>(numTextures + linearClones.size());

                canonicalTextures.emplace(key, canonical);

                linearClones.push_back({m_textureIndices[index], canonical});

                m_linearTextureIndices[index] = canonical;
            }
            else
            {
                m_linearTextureIndices[index] = found->second;
            }
        }
    }

    const uint32_t groupSize =
        std::max(1u, std::min(rc::RenderConfig::GetInstance().numThreads, static_cast<uint32_t>(numTextures)));

    UniquePtr<ThreadPool<void, uint32_t>> threadPool = MakeUnique<ThreadPool<void, uint32_t>>(groupSize);

    zen::HeapVector<UniquePtr<sg::Texture>> textures;

    textures.resize(numTextures);

    uint32_t groupWorkLoad = numTextures / groupSize;

    uint32_t workRemained  = numTextures % groupSize;

    uint32_t startIdx      = 0;

    HeapVector<std::future<HeapVector<UniquePtr<sg::Texture>>>> futures;

    futures.reserve(groupSize);

    for (size_t i = 0; i < groupSize; ++i)
    {
        uint32_t endIdx = startIdx + groupWorkLoad;

        if (workRemained > 0)
        {
            workRemained--;

            endIdx++;
        }

        std::future<HeapVector<UniquePtr<sg::Texture>>> future =
            threadPool->Push([this, startIdx, endIdx](uint32_t) { return LoadGltfTextureBatch(startIdx, endIdx); });

        futures.emplace_back(std::move(future));

        startIdx = endIdx;
    }

    for (std::future<HeapVector<UniquePtr<sg::Texture>>>& fut : futures)
    {
        HeapVector<UniquePtr<sg::Texture>> batch = fut.get();

        for (UniquePtr<sg::Texture>& texture : batch)
        {
            const uint32_t index = texture->index;

            textures[index]      = std::move(texture);
        }
    }

    for (const std::pair<uint32_t, uint32_t>& clone : linearClones)
    {
        const sg::Texture& source = *textures[clone.first];

        textures.emplace_back(MakeUnique<sg::Texture>(source.GetName() + "_linear", clone.second, source.width, source.height,
                                                      Format::R8G8B8A8_UNORM, source.bytesData, source.samplerIndex));

        textures[clone.second]->mipBytes = source.mipBytes;
    }

    pScene->LoadDefaultTextures(static_cast<uint32_t>(textures.size()));

    sg::Scene::DefaultTextures defaultTextures = pScene->GetDefaultTextures();

    textures.emplace_back(defaultTextures.pBaseColor);

    textures.emplace_back(defaultTextures.pMetallicRoughness);

    textures.emplace_back(defaultTextures.pNormal);

    textures.emplace_back(defaultTextures.pEmissive);

    textures.emplace_back(defaultTextures.pOcclusion);

    pScene->SetComponents(std::move(textures));
}

static void ReadJsonVector(const simdjson::dom::element& object, const char* key, float* values, size_t components)
{
    simdjson::dom::array array;

    if (object[key].get_array().get(array) == simdjson::SUCCESS)
    {
        if (array.size() != components)
        {
            LOG_ERROR_AND_THROW("glTF extension vector has an invalid component count");
        }

        size_t component = 0;

        for (simdjson::dom::element entry : array)
        {
            double value = 0.0;

            if (entry.get_double().get(value) != simdjson::SUCCESS || !std::isfinite(value))
            {
                LOG_ERROR_AND_THROW("glTF extension vector has a nonnumeric or nonfinite value");
            }

            values[component++] = static_cast<float>(value);
        }
    }
}

static void LoadAdditionalMaterialFeatures(const simdjson::dom::element&        document,
                                           sg::Material&                        material,
                                           const zen::HeapVector<sg::Texture*>& textures,
                                           const HeapVector<uint32_t>&          textureIndices,
                                           const HeapVector<uint32_t>&          linearIndices,
                                           const std::map<size_t, uint32_t>&    texCoordIndices)
{
    const std::string path = "/extensions/";

    simdjson::dom::element scatter;

    if (document.at_pointer(path + "KHR_materials_volume_scatter").get(scatter) == simdjson::SUCCESS)
    {
        ReadJsonVector(scatter, "multiscatterColorFactor", &material.features.multiscatterColor.x, 3);
    }

    simdjson::dom::element retro;

    if (document.at_pointer(path + "KHR_materials_retroreflection").get(retro) == simdjson::SUCCESS)
    {
        double factor = 0.0;

        if (retro["retroreflectionFactor"].get_double().get(factor) == simdjson::SUCCESS)
        {
            material.features.retroreflection = static_cast<float>(factor);
        }

        simdjson::dom::element texture;

        if (retro["retroreflectionTexture"].get(texture) == simdjson::SUCCESS)
        {
            uint64_t textureIndex = 0;

            if (texture["index"].get_uint64().get(textureIndex) != simdjson::SUCCESS)
            {
                LOG_ERROR_AND_THROW("Retroreflection texture requires an image texture index");
            }

            fastgltf::TextureInfo binding;

            binding.textureIndex = static_cast<size_t>(textureIndex);

            uint64_t coordinates = 0;

            texture["texCoord"].get_uint64().get(coordinates);

            binding.texCoordIndex = static_cast<size_t>(coordinates);

            simdjson::dom::element transform;

            if (texture.at_pointer("/extensions/KHR_texture_transform").get(transform) == simdjson::SUCCESS)
            {
                binding.transform = std::make_unique<fastgltf::TextureTransform>();

                ReadJsonVector(transform, "offset", binding.transform->uvOffset.data(), 2);

                ReadJsonVector(transform, "scale", binding.transform->uvScale.data(), 2);

                double rotation = 0.0;

                transform["rotation"].get_double().get(rotation);

                binding.transform->rotation = static_cast<float>(rotation);

                if (transform["texCoord"].get_uint64().get(coordinates) == simdjson::SUCCESS)
                {
                    binding.transform->texCoordIndex = static_cast<size_t>(coordinates);
                }
            }

            material.features.retroreflectionTexture =
                LoadTextureBinding(&binding, false, textures, textureIndices, linearIndices, texCoordIndices);
        }
    }
}

void FastGLTFLoader::LoadGltfMaterials(sg::Scene* pScene)
{
    m_texCoordIndices.clear();

    m_texCoordIndices.emplace(0, 0);

    m_texCoordIndices.emplace(1, 1);

    for (const fastgltf::Mesh& mesh : m_gltfAsset.meshes)
    {
        for (const fastgltf::Primitive& primitive : mesh.primitives)
        {
            HeapVector<const fastgltf::Attribute*> coordinateAttributes;

            for (const fastgltf::Attribute& attribute : primitive.attributes)
            {
                coordinateAttributes.push_back(&attribute);
            }

            for (const FASTGLTF_FG_PMR_NS::SmallVector<fastgltf::Attribute, 4>& target : primitive.targets)
            {
                for (const fastgltf::Attribute& attribute : target)
                {
                    coordinateAttributes.push_back(&attribute);
                }
            }

            for (const fastgltf::Attribute* sourceAttribute : coordinateAttributes)
            {
                const fastgltf::Attribute& attribute = *sourceAttribute;

                if (attribute.name.starts_with("TEXCOORD_"))
                {
                    size_t source                       = 0;

                    const char* begin                   = attribute.name.data() + 9;

                    const char* end                     = attribute.name.data() + attribute.name.size();

                    const std::from_chars_result parsed = std::from_chars(begin, end, source);

                    if (parsed.ec != std::errc{} || parsed.ptr != end)
                    {
                        LOG_ERROR_AND_THROW("Invalid glTF texture-coordinate semantic index");
                    }

                    m_texCoordIndices.try_emplace(source, 0);
                }
            }
        }
    }

    HeapVector<uint64_t>& texCoordSets = pScene->GetAssetData().vertexTexCoordSets;

    for (std::map<size_t, uint32_t>::value_type& coordinate : m_texCoordIndices)
    {
        coordinate.second = static_cast<uint32_t>(texCoordSets.size());

        texCoordSets.push_back(static_cast<uint64_t>(coordinate.first));
    }

    simdjson::dom::parser jsonParser;

    simdjson::dom::element document;

    if (jsonParser.parse(m_json).get(document) != simdjson::SUCCESS)
    {
        LOG_ERROR_AND_THROW("Failed to parse glTF material extension metadata");
    }

    std::vector<simdjson::dom::element> sourceMaterials;

    simdjson::dom::array sourceArray;

    if (document["materials"].get_array().get(sourceArray) == simdjson::SUCCESS)
    {
        sourceMaterials.reserve(sourceArray.size());

        for (simdjson::dom::element material : sourceArray)
        {
            sourceMaterials.push_back(material);
        }
    }

    if (sourceMaterials.size() != m_gltfAsset.materials.size())
    {
        LOG_ERROR_AND_THROW("glTF material metadata does not match parsed materials");
    }

    sg::Scene::DefaultTextures defaultTextures = pScene->GetDefaultTextures();

    zen::HeapVector<UniquePtr<sg::Material>> materials;

    materials.resize(m_gltfAsset.materials.size());

    const zen::HeapVector<sg::Texture*> sceneTextures = pScene->GetComponents<sg::Texture>();

    uint32_t currIndex                                = 0;

    for (const fastgltf::Material& mat : m_gltfAsset.materials)
    {
        sg::Material* pSgMat    = new sg::Material(std::string(mat.name));

        pSgMat->index           = static_cast<uint32_t>(currIndex);

        pSgMat->doubleSided     = mat.doubleSided;

        pSgMat->alphaCutoff     = mat.alphaCutoff;

        pSgMat->baseColorFactor = glm::make_vec4(mat.pbrData.baseColorFactor.data());

        pSgMat->roughnessFactor = mat.pbrData.roughnessFactor;

        pSgMat->metallicFactor  = mat.pbrData.metallicFactor;

        if (mat.pbrData.baseColorTexture.has_value())
        {
            const fastgltf::TextureInfo* textureInfo = &mat.pbrData.baseColorTexture.value();

            pSgMat->texCoordSets.baseColor           = textureInfo->texCoordIndex;

            pSgMat->m_pBaseColorTexture              = sceneTextures[m_textureIndices[textureInfo->textureIndex]];
        }
        else
        {
            pSgMat->m_pBaseColorTexture = defaultTextures.pBaseColor;
        }

        if (mat.pbrData.metallicRoughnessTexture.has_value())
        {
            const fastgltf::TextureInfo* textureInfo = &mat.pbrData.metallicRoughnessTexture.value();

            pSgMat->texCoordSets.metallicRoughness   = textureInfo->texCoordIndex;

            pSgMat->m_pMetallicRoughnessTexture      = sceneTextures[m_linearTextureIndices[textureInfo->textureIndex]];
        }
        else
        {
            pSgMat->m_pMetallicRoughnessTexture = defaultTextures.pMetallicRoughness;
        }

        if (mat.normalTexture.has_value())
        {
            const fastgltf::TextureInfo* textureInfo = &mat.normalTexture.value();

            pSgMat->texCoordSets.normal              = textureInfo->texCoordIndex;

            pSgMat->m_pNormalTexture                 = sceneTextures[m_linearTextureIndices[textureInfo->textureIndex]];

            pSgMat->normalScale                      = mat.normalTexture->scale;
        }
        else
        {
            pSgMat->m_pNormalTexture = defaultTextures.pNormal;
        }

        if (mat.emissiveTexture.has_value())
        {
            const fastgltf::TextureInfo* textureInfo = &mat.emissiveTexture.value();

            pSgMat->texCoordSets.emissive            = textureInfo->texCoordIndex;

            pSgMat->m_pEmissiveTexture               = sceneTextures[m_textureIndices[textureInfo->textureIndex]];
        }
        else
        {
            pSgMat->m_pEmissiveTexture = defaultTextures.pEmissive;
        }

        {
            pSgMat->emissiveStrength = mat.emissiveStrength;

            pSgMat->emissiveFactor   = Vec4(glm::make_vec3(mat.emissiveFactor.data()), 0.0f);
        }

        if (mat.occlusionTexture.has_value())
        {
            const fastgltf::TextureInfo* textureInfo = &mat.occlusionTexture.value();

            pSgMat->texCoordSets.occlusion           = textureInfo->texCoordIndex;

            pSgMat->m_pOcclusionTexture              = sceneTextures[m_linearTextureIndices[textureInfo->textureIndex]];
        }
        else
        {
            pSgMat->m_pOcclusionTexture = defaultTextures.pOcclusion;
        }

        if (mat.alphaMode == fastgltf::AlphaMode::Blend)
        {
            pSgMat->alphaMode = sg::AlphaMode::Blend;
        }
        else if (mat.alphaMode == fastgltf::AlphaMode::Mask)
        {
            pSgMat->alphaMode = sg::AlphaMode::Mask;
        }
        else
        {
            pSgMat->alphaMode = sg::AlphaMode::Opaque;
        }

        pSgMat->unlit                               = mat.unlit;

        pSgMat->occlusionStrength                   = mat.occlusionTexture ? mat.occlusionTexture->strength : 1.0f;

        const fastgltf::TextureInfo* coreTextures[] = {
            mat.pbrData.baseColorTexture ? &*mat.pbrData.baseColorTexture : nullptr,
            mat.pbrData.metallicRoughnessTexture ? &*mat.pbrData.metallicRoughnessTexture : nullptr,
            mat.normalTexture ? &*mat.normalTexture : nullptr, mat.occlusionTexture ? &*mat.occlusionTexture : nullptr,
            mat.emissiveTexture ? &*mat.emissiveTexture : nullptr};

        uint32_t* coreTexCoords[] = {&pSgMat->texCoordSets.baseColor, &pSgMat->texCoordSets.metallicRoughness,
                                     &pSgMat->texCoordSets.normal, &pSgMat->texCoordSets.occlusion,
                                     &pSgMat->texCoordSets.emissive};

        for (uint32_t index = 0; index < 5; ++index)
        {
            const sg::MaterialTextureBinding binding =
                LoadTextureBinding(coreTextures[index], index == 0 || index == 4, sceneTextures, m_textureIndices,
                                   m_linearTextureIndices, m_texCoordIndices);

            pSgMat->textureTransforms[index] = binding.transform;

            *coreTexCoords[index]            = binding.texCoord;
        }

        LoadMaterialFeatures(mat, *pSgMat, sceneTextures, m_textureIndices, m_linearTextureIndices, m_texCoordIndices);

        LoadAdditionalMaterialFeatures(sourceMaterials[currIndex], *pSgMat, sceneTextures, m_textureIndices,
                                       m_linearTextureIndices, m_texCoordIndices);

        pSgMat->SetData();

        materials[currIndex] = UniquePtr<sg::Material>(pSgMat);

        currIndex++;
    }

    // Push a default material at the end of the list for meshes with no material assigned
    UniquePtr<sg::Material> defaultMaterial      = sg::Material::CreateDefaultUnique();

    defaultMaterial->m_pBaseColorTexture         = defaultTextures.pBaseColor;

    defaultMaterial->m_pMetallicRoughnessTexture = defaultTextures.pMetallicRoughness;

    defaultMaterial->m_pNormalTexture            = defaultTextures.pNormal;

    defaultMaterial->m_pOcclusionTexture         = defaultTextures.pOcclusion;

    defaultMaterial->m_pEmissiveTexture          = defaultTextures.pEmissive;

    defaultMaterial->index                       = static_cast<uint32_t>(materials.size());

    defaultMaterial->SetData();

    materials.emplace_back(defaultMaterial);

    pScene->SetComponents(std::move(materials));
}

const fastgltf::Accessor* FastGLTFLoader::GetVertexAccessor(const fastgltf::Primitive& primitive,
                                                            const char*                name,
                                                            fastgltf::AccessorType     type,
                                                            size_t                     vertexCount) const
{
    const fastgltf::Accessor* result     = nullptr;

    const fastgltf::Attribute* attribute = primitive.findAttribute(name);

    if (attribute != primitive.attributes.end())
    {
        const fastgltf::Accessor& accessor = m_gltfAsset.accessors[attribute->accessorIndex];

        const std::string_view semantic(name);

        const bool integer = accessor.componentType == fastgltf::ComponentType::UnsignedByte
                          || accessor.componentType == fastgltf::ComponentType::UnsignedShort;

        bool componentValid = accessor.componentType == fastgltf::ComponentType::Float
                           || accessor.componentType == fastgltf::ComponentType::Double;

        if (semantic.starts_with("JOINTS_"))
        {
            componentValid = integer && !accessor.normalized;
        }
        else if (semantic.starts_with("COLOR_") || semantic.starts_with("TEXCOORD_") || semantic.starts_with("WEIGHTS_"))
        {
            componentValid |= integer && accessor.normalized;
        }

        bool quantized = false;

        for (const FASTGLTF_STD_PMR_NS::string& extension : m_gltfAsset.extensionsUsed)
        {
            quantized |= extension == "KHR_mesh_quantization";
        }

        const bool signedInteger =
            accessor.componentType == fastgltf::ComponentType::Byte || accessor.componentType == fastgltf::ComponentType::Short;

        if (quantized)
        {
            if (semantic == "POSITION" || semantic.starts_with("TEXCOORD_"))
            {
                componentValid |= integer || signedInteger;
            }
            else if (semantic == "NORMAL" || semantic == "TANGENT")
            {
                componentValid |= signedInteger && accessor.normalized;
            }
        }

        const bool typeValid =
            accessor.type == type || (semantic.starts_with("COLOR_") && accessor.type == fastgltf::AccessorType::Vec3);

        if (componentValid && typeValid && (vertexCount == 0 || accessor.count == vertexCount))
        {
            result = &accessor;
        }
        else
        {
            LOGE("Invalid {} accessor; using the attribute default", name);
        }
    }

    return result;
}

template <typename T>
static T DecodeAttribute(const fastgltf::Asset& asset, const fastgltf::Accessor* accessor, size_t index, const T& fallback)
{
    T result = fallback;

    if (accessor != nullptr)
    {
        result = fastgltf::getAccessorElement<T>(asset, *accessor, index);
    }

    return result;
}

static bool NormalizeSurfaceNormal(Vec3& normal)
{
    const float lengthSquared = glm::dot(normal, normal);

    const bool valid          = std::isfinite(lengthSquared) && lengthSquared > 1e-12f;

    if (valid)
    {
        normal /= std::sqrt(lengthSquared);
    }

    return valid;
}

static void GenerateFlatNormals(HeapVector<Vertex>&   vertices,
                                HeapVector<uint32_t>& indices,
                                HeapVector<uint32_t>& sourceVertices)
{
    // Indexed vertices can belong to faces with different normals. Split them so both
    // the raster path and voxel attribute resolve receive the same flat-shaded surface.
    HeapVector<Vertex> flatVertices(indices.size());

    HeapVector<uint32_t> flatSourceVertices(indices.size());

    for (size_t first = 0; first < indices.size(); first += 3)
    {
        const Vec3 a(vertices[indices[first]].pos);

        const Vec3 b(vertices[indices[first + 1]].pos);

        const Vec3 c(vertices[indices[first + 2]].pos);

        Vec3 normal = glm::cross(b - a, c - a);

        if (!NormalizeSurfaceNormal(normal))
        {
            normal = Vec3(0, 1, 0);
        }

        for (size_t corner = 0; corner < 3; ++corner)
        {
            const size_t index          = first + corner;

            flatVertices[index]         = vertices[indices[index]];

            flatSourceVertices[index]   = sourceVertices[indices[index]];

            flatVertices[index].normal  = Vec4(normal, 0);

            flatVertices[index].tangent = Vec4(0);

            indices[index]              = static_cast<uint32_t>(index);
        }
    }

    vertices       = std::move(flatVertices);

    sourceVertices = std::move(flatSourceVertices);
}

bool FastGLTFLoader::LoadPrimitive(const fastgltf::Primitive& primitive,
                                   HeapVector<Vertex>&        vertices,
                                   HeapVector<uint32_t>&      indices,
                                   HeapVector<uint32_t>&      sourceVertices,
                                   bool&                      flatNormals) const
{
    vertices.clear();

    indices.clear();

    sourceVertices.clear();

    flatNormals                         = false;

    const fastgltf::Accessor* positions = GetVertexAccessor(primitive, "POSITION", fastgltf::AccessorType::Vec3, 0);

    bool valid                          = positions != nullptr;

    const bool triangles                = primitive.type == fastgltf::PrimitiveType::Triangles
                        || primitive.type == fastgltf::PrimitiveType::TriangleStrip
                        || primitive.type == fastgltf::PrimitiveType::TriangleFan;

    if (valid)
    {
        const size_t count = positions->count;

        if (primitive.materialIndex)
        {
            const fastgltf::Material& material = m_gltfAsset.materials[*primitive.materialIndex];

            // Unlit materials consume only base color; retained lighting-extension maps do
            // not impose coordinate requirements on a primitive that cannot sample them.
            const HeapVector<TextureRole> roles =
                material.unlit
                    ? HeapVector<TextureRole>{{material.pbrData.baseColorTexture ? &*material.pbrData.baseColorTexture
                                                                                 : nullptr,
                                               true}}
                    : GetMaterialTextureRoles(material);

            for (const TextureRole& role : roles)
            {
                if (role.first != nullptr)
                {
                    const fastgltf::TextureInfo& binding = *role.first;

                    const size_t coordinateSet           = binding.transform && binding.transform->texCoordIndex
                                                             ? *binding.transform->texCoordIndex
                                                             : binding.texCoordIndex;

                    const std::string semantic           = "TEXCOORD_" + std::to_string(coordinateSet);

                    if (GetVertexAccessor(primitive, semantic.c_str(), fastgltf::AccessorType::Vec2, count) == nullptr)
                    {
                        LOG_ERROR_AND_THROW(fmt::format("glTF primitive material requires missing or invalid {}", semantic));
                    }
                }
            }
        }

        const fastgltf::Accessor* normals  = GetVertexAccessor(primitive, "NORMAL", fastgltf::AccessorType::Vec3, count);

        const fastgltf::Accessor* tangents = GetVertexAccessor(primitive, "TANGENT", fastgltf::AccessorType::Vec4, count);

        const fastgltf::Accessor* colors   = GetVertexAccessor(primitive, "COLOR_0", fastgltf::AccessorType::Vec4, count);

        const fastgltf::Accessor* uv0      = GetVertexAccessor(primitive, "TEXCOORD_0", fastgltf::AccessorType::Vec2, count);

        const fastgltf::Accessor* uv1      = GetVertexAccessor(primitive, "TEXCOORD_1", fastgltf::AccessorType::Vec2, count);

        const fastgltf::Accessor* joints   = GetVertexAccessor(primitive, "JOINTS_0", fastgltf::AccessorType::Vec4, count);

        const fastgltf::Accessor* weights  = GetVertexAccessor(primitive, "WEIGHTS_0", fastgltf::AccessorType::Vec4, count);

        bool generateNormals               = normals == nullptr;

        vertices.resize(count);

        sourceVertices.resize(count);

        for (size_t index = 0; index < count; ++index)
        {
            Vertex& vertex         = vertices[index];

            sourceVertices[index]  = static_cast<uint32_t>(index);

            vertex.pos             = Vec4(DecodeAttribute(m_gltfAsset, positions, index, Vec3(0)), 1);

            valid                 &= std::isfinite(vertex.pos.x) && std::isfinite(vertex.pos.y) && std::isfinite(vertex.pos.z);

            Vec3 normal            = DecodeAttribute(m_gltfAsset, normals, index, Vec3(0));

            generateNormals       |= !NormalizeSurfaceNormal(normal);

            vertex.normal          = Vec4(normal, 0);

            vertex.tangent         = DecodeAttribute(m_gltfAsset, tangents, index, Vec4(0));

            vertex.uv0             = DecodeAttribute(m_gltfAsset, uv0, index, Vec2(0));

            vertex.uv1             = DecodeAttribute(m_gltfAsset, uv1, index, Vec2(0));

            vertex.joint0          = DecodeAttribute(m_gltfAsset, joints, index, Vec4(0));

            vertex.weight0         = DecodeAttribute(m_gltfAsset, weights, index, Vec4(0));

            vertex.color           = colors != nullptr && colors->type == fastgltf::AccessorType::Vec3
                                       ? Vec4(DecodeAttribute(m_gltfAsset, colors, index, Vec3(1)), 1)
                                       : DecodeAttribute(m_gltfAsset, colors, index, Vec4(1));
        }

        if (primitive.indicesAccessor.has_value())
        {
            const fastgltf::Accessor& accessor  = m_gltfAsset.accessors[*primitive.indicesAccessor];

            valid                              &= accessor.type == fastgltf::AccessorType::Scalar && !accessor.normalized
                  && (accessor.componentType == fastgltf::ComponentType::UnsignedByte
                      || accessor.componentType == fastgltf::ComponentType::UnsignedShort
                      || accessor.componentType == fastgltf::ComponentType::UnsignedInt);

            if (valid)
            {
                indices.resize(accessor.count);

                fastgltf::copyFromAccessor<uint32_t>(m_gltfAsset, accessor, indices.data());
            }
        }
        else
        {
            indices.resize(count);

            for (uint32_t index = 0; index < count; ++index)
            {
                indices[index] = index;
            }
        }

        if (valid && triangles && primitive.type != fastgltf::PrimitiveType::Triangles && indices.size() >= 3)
        {
            HeapVector<uint32_t> triangles;

            triangles.reserve((indices.size() - 2) * 3);

            for (size_t index = 2; index < indices.size(); ++index)
            {
                const uint32_t a =
                    primitive.type == fastgltf::PrimitiveType::TriangleFan ? indices[0] : indices[index - 2 + (index & 1)];

                const uint32_t b = primitive.type == fastgltf::PrimitiveType::TriangleFan ? indices[index - 1]
                                                                                          : indices[index - 1 - (index & 1)];

                triangles.push_back(a);

                triangles.push_back(b);

                triangles.push_back(indices[index]);
            }

            indices = std::move(triangles);
        }

        if (valid
            && (primitive.type == fastgltf::PrimitiveType::LineStrip || primitive.type == fastgltf::PrimitiveType::LineLoop)
            && indices.size() >= 2)
        {
            HeapVector<uint32_t> lines;

            for (size_t index = 1; index < indices.size(); ++index)
            {
                lines.push_back(indices[index - 1]);

                lines.push_back(indices[index]);
            }

            if (primitive.type == fastgltf::PrimitiveType::LineLoop)
            {
                lines.push_back(indices[indices.size() - 1]);

                lines.push_back(indices[0]);
            }

            indices = std::move(lines);
        }

        valid &= !indices.empty()
              && (triangles ? indices.size() % 3 == 0
                            : primitive.type == fastgltf::PrimitiveType::Points || indices.size() % 2 == 0);

        for (uint32_t index : indices)
        {
            valid &= index < count;
        }

        if (valid && triangles && generateNormals)
        {
            GenerateFlatNormals(vertices, indices, sourceVertices);

            flatNormals = true;
        }
    }

    if (!valid)
    {
        LOGE("Skipping invalid glTF primitive");
    }

    return valid;
}

void FastGLTFLoader::LoadGltfMeshes(sg::Scene* pScene)
{
    m_vertices.clear();

    m_indices.clear();

    const zen::HeapVector<sg::Material*> materials = pScene->GetComponents<sg::Material>();

    HeapVector<Vertex> vertices;

    HeapVector<uint32_t> indices;

    HeapVector<uint32_t> sourceVertices;

    m_meshSourceVertices.clear();

    m_meshSourceVertices.resize(m_gltfAsset.meshes.size());

    m_meshFirstVertices.clear();

    m_meshFirstVertices.resize(m_gltfAsset.meshes.size());

    m_meshFlatNormals.clear();

    m_meshFlatNormals.resize(m_gltfAsset.meshes.size());

    uint32_t meshIndex = 0;

    for (const fastgltf::Mesh& gltfMesh : m_gltfAsset.meshes)
    {
        UniquePtr<sg::Mesh> mesh = MakeUnique<sg::Mesh>(std::string(gltfMesh.name));

        uint32_t subMeshIndex    = 0;

        for (const fastgltf::Primitive& primitive : gltfMesh.primitives)
        {
            m_meshSourceVertices[meshIndex].emplace_back();

            m_meshFirstVertices[meshIndex].push_back(static_cast<uint32_t>(m_vertices.size()));

            bool flatNormals = false;

            m_meshFlatNormals[meshIndex].push_back(0);

            if (LoadPrimitive(primitive, vertices, indices, sourceVertices, flatNormals))
            {
                const uint32_t vertexStart = static_cast<uint32_t>(m_vertices.size());

                const uint32_t indexStart  = static_cast<uint32_t>(m_indices.size());

                sg::AABB bounds;

                m_meshSourceVertices[meshIndex][subMeshIndex] = sourceVertices;

                m_meshFlatNormals[meshIndex][subMeshIndex]    = flatNormals;

                HeapVector<std::pair<uint32_t, const fastgltf::Accessor*>> texCoords;

                const uint32_t texCoordCount = static_cast<uint32_t>(m_texCoordIndices.size());

                for (const fastgltf::Attribute& attribute : primitive.attributes)
                {
                    if (attribute.name.starts_with("TEXCOORD_"))
                    {
                        const size_t sourceSet = static_cast<size_t>(std::stoull(std::string(attribute.name.substr(9))));

                        const uint32_t set     = m_texCoordIndices.at(sourceSet);

                        const fastgltf::Accessor* accessor =
                            GetVertexAccessor(primitive, attribute.name.c_str(), fastgltf::AccessorType::Vec2, 0);

                        if (accessor != nullptr)
                        {
                            texCoords.push_back({set, accessor});
                        }
                    }
                }

                size_t vertexIndex = 0;

                for (const Vertex& vertex : vertices)
                {
                    bounds.SetMin(Vec3(vertex.pos));

                    bounds.SetMax(Vec3(vertex.pos));

                    m_vertices.push_back(vertex);

                    HeapVector<Vec2> coordinates(texCoordCount, Vec2(0));

                    for (const std::pair<uint32_t, const fastgltf::Accessor*>& coordinate : texCoords)
                    {
                        coordinates[coordinate.first] =
                            fastgltf::getAccessorElement<Vec2>(m_gltfAsset, *coordinate.second, sourceVertices[vertexIndex]);
                    }

                    pScene->GetAssetData().vertexTexCoords.push_back(std::move(coordinates));

                    ++vertexIndex;
                }

                for (uint32_t index : indices)
                {
                    m_indices.push_back(vertexStart + index);
                }

                const uint32_t materialIndex   = static_cast<uint32_t>(primitive.materialIndex.value_or(materials.size() - 1));

                const std::string name         = fmt::format("Mesh_{}_SubMesh#{}", gltfMesh.name, subMeshIndex);

                UniquePtr<sg::SubMesh> subMesh = MakeUnique<sg::SubMesh>(
                    name, indexStart, static_cast<uint32_t>(indices.size()), static_cast<uint32_t>(vertices.size()));

                subMesh->SetMaterial(materialIndex, materials[materialIndex]);

                subMesh->assetMesh      = meshIndex;

                subMesh->assetPrimitive = subMeshIndex;

                subMesh->topology       = primitive.type == fastgltf::PrimitiveType::Points ? sg::MeshTopology::Points
                                        : primitive.type == fastgltf::PrimitiveType::Lines
                                            || primitive.type == fastgltf::PrimitiveType::LineLoop
                                            || primitive.type == fastgltf::PrimitiveType::LineStrip
                                            ? sg::MeshTopology::Lines
                                            : sg::MeshTopology::Triangles;

                subMesh->SetAABB(bounds.GetMin(), bounds.GetMax());

                mesh->AddSubMesh(subMesh.Get());

                mesh->SetAABB(bounds.GetMin(), bounds.GetMax());

                pScene->AddComponent(std::move(subMesh));
            }

            ++subMeshIndex;
        }

        pScene->AddComponent(std::move(mesh));

        ++meshIndex;
    }
}

void FastGLTFLoader::CloneDeformedMesh(uint32_t meshIndex, sg::Node& node, sg::Scene* pScene)
{
    const fastgltf::Mesh& source = m_gltfAsset.meshes[meshIndex];

    sg::Mesh* original           = m_meshes[meshIndex];

    UniquePtr<sg::Mesh> mesh     = MakeUnique<sg::Mesh>(original->GetName() + "_instance_" + std::to_string(node.GetIndex()));

    sg::SceneAssetData& data     = pScene->GetAssetData();

    uint32_t subMeshIndex        = 0;

    for (uint32_t primitiveIndex = 0; primitiveIndex < source.primitives.size(); ++primitiveIndex)
    {
        const HeapVector<uint32_t>& sourceVertices = m_meshSourceVertices[meshIndex][primitiveIndex];

        if (!sourceVertices.empty())
        {
            const sg::SubMesh* originalSubMesh = original->GetSubMeshes()[subMeshIndex++];

            const uint32_t sourceFirstVertex   = m_meshFirstVertices[meshIndex][primitiveIndex];

            sg::DeformationPrimitiveAsset deformation;

            deformation.node           = node.GetIndex();

            deformation.firstVertex    = static_cast<uint32_t>(m_vertices.size());

            deformation.vertexCount    = static_cast<uint32_t>(sourceVertices.size());

            deformation.subMesh        = static_cast<uint32_t>(pScene->GetComponents<sg::SubMesh>().size());

            deformation.sourceVertices = sourceVertices;

            deformation.flatNormals    = m_meshFlatNormals[meshIndex][primitiveIndex] != 0;

            for (uint32_t morphIndex = 0; morphIndex < data.morphPrimitives.size(); ++morphIndex)
            {
                if (data.morphPrimitives[morphIndex].mesh == meshIndex
                    && data.morphPrimitives[morphIndex].primitive == primitiveIndex)
                {
                    deformation.morphPrimitive = static_cast<int32_t>(morphIndex);
                }
            }

            const fastgltf::Primitive& primitive = source.primitives[primitiveIndex];

            HeapVector<std::pair<const fastgltf::Accessor*, const fastgltf::Accessor*>> influenceSets;

            for (const fastgltf::Attribute& attribute : primitive.attributes)
            {
                if (attribute.name.starts_with("JOINTS_"))
                {
                    const std::string weightsName = "WEIGHTS_" + std::string(attribute.name.substr(7));

                    const fastgltf::Accessor* joints =
                        GetVertexAccessor(primitive, attribute.name.c_str(), fastgltf::AccessorType::Vec4, 0);

                    const fastgltf::Accessor* weights = GetVertexAccessor(
                        primitive, weightsName.c_str(), fastgltf::AccessorType::Vec4, joints != nullptr ? joints->count : 0);

                    if (joints != nullptr && weights != nullptr)
                    {
                        influenceSets.push_back({joints, weights});
                    }
                }
            }

            if (!influenceSets.empty())
            {
                deformation.influences.resize(sourceVertices.size());
            }

            sg::AABB bounds;

            for (uint32_t vertex = 0; vertex < sourceVertices.size(); ++vertex)
            {
                const Vertex originalVertex = m_vertices[sourceFirstVertex + vertex];

                m_vertices.push_back(originalVertex);

                HeapVector<Vec2> coordinates = data.vertexTexCoords[sourceFirstVertex + vertex];

                data.vertexTexCoords.push_back(std::move(coordinates));

                bounds.SetMin(Vec3(originalVertex.pos));

                bounds.SetMax(Vec3(originalVertex.pos));

                for (const std::pair<const fastgltf::Accessor*, const fastgltf::Accessor*>& set : influenceSets)
                {
                    const glm::uvec4 joints =
                        fastgltf::getAccessorElement<glm::uvec4>(m_gltfAsset, *set.first, sourceVertices[vertex]);

                    const Vec4 weights = fastgltf::getAccessorElement<Vec4>(m_gltfAsset, *set.second, sourceVertices[vertex]);

                    for (uint32_t component = 0; component < 4; ++component)
                    {
                        if (weights[component] > 0.0f)
                        {
                            deformation.influences[vertex].push_back({joints[component], weights[component]});
                        }
                    }
                }
            }

            const uint32_t firstIndex = static_cast<uint32_t>(m_indices.size());

            for (uint32_t index = 0; index < originalSubMesh->GetIndexCount(); ++index)
            {
                const uint32_t sourceIndex = m_indices[originalSubMesh->GetFirstIndex() + index];

                m_indices.push_back(deformation.firstVertex + sourceIndex - sourceFirstVertex);
            }

            UniquePtr<sg::SubMesh> subMesh =
                MakeUnique<sg::SubMesh>(originalSubMesh->GetName() + "_instance_" + std::to_string(node.GetIndex()), firstIndex,
                                        originalSubMesh->GetIndexCount(), deformation.vertexCount);

            subMesh->SetMaterial(originalSubMesh->GetMaterialIndex(), originalSubMesh->GetMaterial());

            subMesh->topology       = originalSubMesh->topology;

            subMesh->assetMesh      = originalSubMesh->assetMesh;

            subMesh->assetPrimitive = originalSubMesh->assetPrimitive;

            subMesh->SetAABB(bounds.GetMin(), bounds.GetMax());

            mesh->AddSubMesh(subMesh.Get());

            mesh->SetAABB(bounds.GetMin(), bounds.GetMax());

            pScene->AddComponent(std::move(subMesh));

            data.deformations.push_back(std::move(deformation));
        }
    }

    node.AddComponent(mesh.Get());

    mesh->AddNode(&node);

    pScene->AddComponent(std::move(mesh));
}

static HeapVector<float> DecodeAnimationValues(const fastgltf::Asset& asset, const fastgltf::Accessor& accessor)
{
    const uint32_t components = fastgltf::getNumComponents(accessor.type);

    HeapVector<float> values(accessor.count * components);

    for (size_t index = 0; index < accessor.count; ++index)
    {
        Vec4 value(0.0f);

        switch (accessor.type)
        {
            case fastgltf::AccessorType::Scalar: value.x = fastgltf::getAccessorElement<float>(asset, accessor, index); break;

            case fastgltf::AccessorType::Vec2:
                value = Vec4(fastgltf::getAccessorElement<Vec2>(asset, accessor, index), 0.0f, 0.0f);

                break;

            case fastgltf::AccessorType::Vec3:
                value = Vec4(fastgltf::getAccessorElement<Vec3>(asset, accessor, index), 0.0f);

                break;

            case fastgltf::AccessorType::Vec4: value = fastgltf::getAccessorElement<Vec4>(asset, accessor, index); break;

            default: LOG_ERROR_AND_THROW("Invalid glTF animation output type"); break;
        }

        for (uint32_t component = 0; component < components; ++component)
        {
            values[index * components + component] = value[component];
        }
    }

    return values;
}

static float ImageLightLinearChannel(float channel)
{
    const float result = channel <= 0.04045f ? channel / 12.92f : std::pow((channel + 0.055f) / 1.055f, 2.4f);

    return result;
}

static HeapVector<Vec4> DecodeImageLightFace(const fastgltf::Asset& asset, uint32_t imageIndex, uint32_t size)
{
    if (imageIndex >= asset.images.size())
    {
        LOG_ERROR_AND_THROW("Image based light references an invalid image");
    }

    HeapVector<std::byte> fileBytes;

    const fastgltf::span<const std::byte> bytes = GetImageBytes(asset, asset.images[imageIndex], fileBytes);

    uint32_t width                              = 0;

    uint32_t height                             = 0;

    const HeapVector<uint8_t> pixels =
        DecodeImagePixels(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), width, height);

    if (width != size || height != size || pixels.empty())
    {
        LOG_ERROR_AND_THROW("Image based light cubemap face has invalid dimensions or pixels");
    }

    int sourceWidth  = 0;

    int sourceHeight = 0;

    int channels     = 0;

    const uint8_t pngIdentifier[]{0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};

    const bool png =
        bytes.size() >= sizeof(pngIdentifier) && std::memcmp(bytes.data(), pngIdentifier, sizeof(pngIdentifier)) == 0;

    if (bytes.size() <= INT_MAX)
    {
        stbi_info_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()), static_cast<int>(bytes.size()), &sourceWidth,
                              &sourceHeight, &channels);
    }

    const bool rgbd = png && channels == 4;

    HeapVector<Vec4> radiance(size_t(size) * size);

    for (uint32_t y = 0; y < size; ++y)
    {
        for (uint32_t x = 0; x < size; ++x)
        {
            // EXT_lights_image_based images are flipped about their vertical axis.
            const size_t source = (size_t(y) * size + size - 1 - x) * 4;

            const Vec3 color(float(pixels[source]) / 255.0f, float(pixels[source + 1]) / 255.0f,
                             float(pixels[source + 2]) / 255.0f);

            const Vec3 linear = rgbd ? glm::pow(color, Vec3(2.2f)) / std::max(float(pixels[source + 3]) / 255.0f, 1.0f / 255.0f)
                                     : Vec3(ImageLightLinearChannel(color.r), ImageLightLinearChannel(color.g),
                                            ImageLightLinearChannel(color.b));

            radiance[size_t(y) * size + x] = Vec4(linear, 1.0f);
        }
    }

    return radiance;
}

static void LoadImageBasedLights(const simdjson::dom::element& document, const fastgltf::Asset& asset, sg::SceneAssetData& data)
{
    simdjson::dom::array lights;

    if (document.at_pointer("/extensions/EXT_lights_image_based/lights").get_array().get(lights) == simdjson::SUCCESS)
    {
        for (simdjson::dom::element source : lights)
        {
            sg::ImageBasedLightAsset light;

            std::string_view name;

            if (source["name"].get_string().get(name) == simdjson::SUCCESS)
            {
                light.name = std::string(name);
            }

            double intensity = 1.0;

            source["intensity"].get_double().get(intensity);

            light.intensity = static_cast<float>(intensity);

            float rotation[]{0.0f, 0.0f, 0.0f, 1.0f};

            ReadJsonVector(source, "rotation", rotation, 4);

            light.rotation = Quat(rotation[3], rotation[0], rotation[1], rotation[2]);

            simdjson::dom::array coefficients;

            if (source["irradianceCoefficients"].get_array().get(coefficients) != simdjson::SUCCESS || coefficients.size() != 9)
            {
                LOG_ERROR_AND_THROW("Image based light requires nine irradiance coefficients");
            }

            for (simdjson::dom::element coefficient : coefficients)
            {
                simdjson::dom::array values;

                if (coefficient.get_array().get(values) != simdjson::SUCCESS || values.size() != 3)
                {
                    LOG_ERROR_AND_THROW("Image based light irradiance coefficient requires three channels");
                }

                Vec3 value(0.0f);

                uint32_t channel = 0;

                for (simdjson::dom::element entry : values)
                {
                    double number = 0.0;

                    if (entry.get_double().get(number) != simdjson::SUCCESS || !std::isfinite(number))
                    {
                        LOG_ERROR_AND_THROW("Invalid image based light irradiance coefficient");
                    }

                    value[channel++] = static_cast<float>(number);
                }

                light.irradianceCoefficients.push_back(value);
            }

            uint64_t size = 0;

            if (source["specularImageSize"].get_uint64().get(size) != simdjson::SUCCESS || size == 0 || size > UINT32_MAX
                || (size & (size - 1)) != 0)
            {
                LOG_ERROR_AND_THROW("Image based light requires a power of two cubemap size");
            }

            light.size = static_cast<uint32_t>(size);

            simdjson::dom::array mips;

            if (source["specularImages"].get_array().get(mips) != simdjson::SUCCESS || mips.size() == 0)
            {
                LOG_ERROR_AND_THROW("Image based light requires specular cubemap images");
            }

            for (simdjson::dom::element mip : mips)
            {
                simdjson::dom::array faces;

                if (mip.get_array().get(faces) != simdjson::SUCCESS || faces.size() != 6 || light.mipLevels >= 32)
                {
                    LOG_ERROR_AND_THROW("Image based light requires six faces per specular mip");
                }

                const uint32_t mipSize = std::max(1u, light.size >> light.mipLevels);

                for (simdjson::dom::element face : faces)
                {
                    uint64_t imageIndex = 0;

                    if (face.get_uint64().get(imageIndex) != simdjson::SUCCESS || imageIndex >= asset.images.size())
                    {
                        LOG_ERROR_AND_THROW("Invalid image based light specular image index");
                    }

                    light.specularMipFaces.push_back(DecodeImageLightFace(asset, static_cast<uint32_t>(imageIndex), mipSize));
                }

                ++light.mipLevels;
            }

            data.imageBasedLights.push_back(std::move(light));
        }

        uint64_t selected = 0;

        const std::string pointer =
            "/scenes/" + std::to_string(asset.defaultScene.value_or(0)) + "/extensions/EXT_lights_image_based/light";

        if (document.at_pointer(pointer).get_uint64().get(selected) == simdjson::SUCCESS)
        {
            if (selected >= data.imageBasedLights.size())
            {
                LOG_ERROR_AND_THROW("Scene references an invalid image based light");
            }

            data.imageBasedLight = static_cast<int32_t>(selected);
        }
    }
}

template <typename T> static void CopyRetainedAccessorValues(const fastgltf::Asset&    asset,
                                                             const fastgltf::Accessor& accessor,
                                                             sg::VertexAttributeAsset& retained)
{
    for (size_t index = 0; index < accessor.count; ++index)
    {
        const T value = fastgltf::getAccessorElement<T>(asset, accessor, index);

        if constexpr (std::is_arithmetic_v<T>)
        {
            retained.values[index] = value;
        }
        else
        {
            for (uint32_t component = 0; component < retained.components; ++component)
            {
                retained.values[index * retained.components + component] = value[component];
            }
        }
    }
}

static sg::VertexAttributeAsset DecodeRetainedVertexAttribute(const fastgltf::Asset&     asset,
                                                              const fastgltf::Attribute& attribute,
                                                              uint32_t                   mesh,
                                                              uint32_t                   primitive,
                                                              int32_t                    morphTarget = -1)
{
    const fastgltf::Accessor& accessor = asset.accessors[attribute.accessorIndex];

    sg::VertexAttributeAsset retained;

    retained.mesh          = mesh;

    retained.primitive     = primitive;

    retained.semantic      = std::string(attribute.name);

    retained.morphTarget   = morphTarget;

    retained.components    = static_cast<uint32_t>(fastgltf::getNumComponents(accessor.type));

    retained.componentType = fastgltf::getGLComponentType(accessor.componentType);

    retained.normalized    = accessor.normalized;

    retained.values.resize(accessor.count * retained.components);

    switch (accessor.type)
    {
        case fastgltf::AccessorType::Scalar: CopyRetainedAccessorValues<double>(asset, accessor, retained); break;
        case fastgltf::AccessorType::Vec2: CopyRetainedAccessorValues<fastgltf::math::dvec2>(asset, accessor, retained); break;
        case fastgltf::AccessorType::Vec3: CopyRetainedAccessorValues<fastgltf::math::dvec3>(asset, accessor, retained); break;
        case fastgltf::AccessorType::Vec4: CopyRetainedAccessorValues<fastgltf::math::dvec4>(asset, accessor, retained); break;
        default: LOG_ERROR_AND_THROW("Invalid glTF vertex attribute type"); break;
    }

    return retained;
}

void FastGLTFLoader::LoadGltfAssetData(sg::Scene* pScene)
{
    sg::SceneAssetData& data = pScene->GetAssetData();

    data.sourceDocument      = m_sourceJson;

    simdjson::dom::parser jsonParser;

    simdjson::dom::element document;

    if (jsonParser.parse(m_json).get(document) != simdjson::SUCCESS)
    {
        LOG_ERROR_AND_THROW("Failed to parse glTF extension metadata");
    }

    LoadImageBasedLights(document, m_gltfAsset, data);

    for (const std::string& name : m_gltfAsset.materialVariants)
    {
        data.materialVariants.push_back(name);
    }

    for (const fastgltf::Skin& source : m_gltfAsset.skins)
    {
        sg::SkinAsset skin;

        skin.name     = std::string(source.name);

        skin.skeleton = source.skeleton ? static_cast<int32_t>(*source.skeleton) : -1;

        for (size_t joint : source.joints)
        {
            skin.joints.push_back(static_cast<uint32_t>(joint));
        }

        skin.inverseBindMatrices = HeapVector<Mat4>(skin.joints.size(), Mat4(1.0f));

        if (source.inverseBindMatrices)
        {
            const fastgltf::Accessor& accessor = m_gltfAsset.accessors[*source.inverseBindMatrices];

            if (accessor.count < skin.joints.size())
            {
                LOG_ERROR_AND_THROW("Too few glTF inverse bind matrices");
            }

            for (size_t joint = 0; joint < skin.joints.size(); ++joint)
            {
                skin.inverseBindMatrices[joint] = fastgltf::getAccessorElement<Mat4>(m_gltfAsset, accessor, joint);
            }
        }

        data.skins.push_back(std::move(skin));
    }

    uint32_t animationIndex = 0;

    for (const fastgltf::Animation& source : m_gltfAsset.animations)
    {
        sg::AnimationAsset animation;

        animation.name = std::string(source.name);

        for (const fastgltf::AnimationSampler& sourceSampler : source.samplers)
        {
            sg::AnimationSampler sampler;

            sampler.interpolation            = static_cast<sg::AnimationInterpolation>(sourceSampler.interpolation);

            const fastgltf::Accessor& input  = m_gltfAsset.accessors[sourceSampler.inputAccessor];

            const fastgltf::Accessor& output = m_gltfAsset.accessors[sourceSampler.outputAccessor];

            sampler.components               = fastgltf::getNumComponents(output.type);

            sampler.times.resize(input.count);

            fastgltf::copyFromAccessor<float>(m_gltfAsset, input, sampler.times.data());

            sampler.values = DecodeAnimationValues(m_gltfAsset, output);

            animation.samplers.push_back(std::move(sampler));
        }

        uint32_t channelIndex = 0;

        for (const fastgltf::AnimationChannel& sourceChannel : source.channels)
        {
            const std::string pointerPath = "/animations/" + std::to_string(animationIndex) + "/channels/"
                                          + std::to_string(channelIndex) + "/target/extensions/KHR_animation_pointer/pointer";

            std::string_view pointer;

            const bool hasPointer = document.at_pointer(pointerPath).get_string().get(pointer) == simdjson::SUCCESS;

            if (sourceChannel.nodeIndex)
            {
                sg::AnimationChannel channel;

                channel.sampler = static_cast<uint32_t>(sourceChannel.samplerIndex);

                channel.node    = static_cast<uint32_t>(*sourceChannel.nodeIndex);

                channel.path    = static_cast<sg::AnimationPath>(static_cast<uint32_t>(sourceChannel.path) - 1);

                animation.channels.push_back(channel);
            }
            else if (hasPointer)
            {
                sg::AnimationChannel channel;

                channel.sampler = static_cast<uint32_t>(sourceChannel.samplerIndex);

                channel.path    = sg::AnimationPath::Pointer;

                channel.pointer = std::string(pointer);

                animation.channels.push_back(std::move(channel));
            }

            ++channelIndex;
        }

        data.animations.push_back(std::move(animation));

        ++animationIndex;
    }

    for (uint32_t meshIndex = 0; meshIndex < m_gltfAsset.meshes.size(); ++meshIndex)
    {
        const fastgltf::Mesh& mesh = m_gltfAsset.meshes[meshIndex];

        for (uint32_t primitiveIndex = 0; primitiveIndex < mesh.primitives.size(); ++primitiveIndex)
        {
            const fastgltf::Primitive& primitive = mesh.primitives[primitiveIndex];

            for (const fastgltf::Attribute& attribute : primitive.attributes)
            {
                if (attribute.name != "POSITION" && attribute.name != "NORMAL" && attribute.name != "TANGENT"
                    && attribute.name != "COLOR_0" && !attribute.name.starts_with("TEXCOORD_"))
                {
                    data.extraVertexAttributes.push_back(
                        DecodeRetainedVertexAttribute(m_gltfAsset, attribute, meshIndex, primitiveIndex));
                }
            }

            if (!primitive.mappings.empty())
            {
                sg::MaterialVariantPrimitiveAsset variant;

                variant.mesh      = meshIndex;

                variant.primitive = primitiveIndex;

                for (const fastgltf::Optional<size_t>& material : primitive.mappings)
                {
                    variant.materials.push_back(material ? static_cast<int32_t>(*material) : -1);
                }

                data.variantPrimitives.push_back(std::move(variant));
            }

            if (!primitive.targets.empty())
            {
                sg::MorphPrimitiveAsset morph;

                morph.mesh      = meshIndex;

                morph.primitive = primitiveIndex;

                for (float weight : mesh.weights)
                {
                    morph.weights.push_back(weight);
                }

                for (const FASTGLTF_FG_PMR_NS::SmallVector<fastgltf::Attribute, 4>& attributes : primitive.targets)
                {
                    sg::MorphTargetAsset target;

                    for (const fastgltf::Attribute& attribute : attributes)
                    {
                        const fastgltf::Accessor& accessor = m_gltfAsset.accessors[attribute.accessorIndex];

                        HeapVector<Vec3>* values           = attribute.name == "POSITION" ? &target.positions
                                                           : attribute.name == "NORMAL"   ? &target.normals
                                                           : attribute.name == "TANGENT"  ? &target.tangents
                                                                                          : nullptr;

                        if (values != nullptr)
                        {
                            values->resize(accessor.count);

                            fastgltf::copyFromAccessor<Vec3>(m_gltfAsset, accessor, values->data());
                        }
                        else if (attribute.name.starts_with("TEXCOORD_"))
                        {
                            const size_t sourceSet = static_cast<size_t>(std::stoull(std::string(attribute.name.substr(9))));

                            target.texCoords.resize(m_texCoordIndices.size());

                            HeapVector<Vec2>& coordinates = target.texCoords[m_texCoordIndices.at(sourceSet)];

                            coordinates.resize(accessor.count);

                            fastgltf::copyFromAccessor<Vec2>(m_gltfAsset, accessor, coordinates.data());
                        }
                        else if (attribute.name.starts_with("COLOR_"))
                        {
                            sg::MorphColorAttributeAsset color;

                            color.set    = std::stoull(std::string(attribute.name.substr(6)));

                            color.values = HeapVector<Vec4>(accessor.count, Vec4(0));

                            if (accessor.type == fastgltf::AccessorType::Vec4)
                            {
                                fastgltf::copyFromAccessor<Vec4>(m_gltfAsset, accessor, color.values.data());
                            }
                            else
                            {
                                for (size_t index = 0; index < accessor.count; ++index)
                                {
                                    color.values[index] =
                                        Vec4(fastgltf::getAccessorElement<Vec3>(m_gltfAsset, accessor, index), 0);
                                }
                            }

                            if (color.set == 0)
                            {
                                target.colors = std::move(color.values);
                            }
                            else
                            {
                                target.extraColors.push_back(std::move(color));
                            }
                        }
                        else
                        {
                            data.extraVertexAttributes.push_back(DecodeRetainedVertexAttribute(
                                m_gltfAsset, attribute, meshIndex, primitiveIndex, static_cast<int32_t>(morph.targets.size())));
                        }
                    }

                    morph.targets.push_back(std::move(target));
                }

                data.morphPrimitives.push_back(std::move(morph));
            }
        }
    }
}

void FastGLTFLoader::LoadGltfRenderableNodes(sg::Scene* pScene)
{
    m_meshes = pScene->GetComponents<sg::Mesh>();

    zen::HeapVector<UniquePtr<sg::Node>> sgNodes;

    sgNodes.reserve(m_gltfAsset.nodes.size());

    HeapVector<std::pair<uint32_t, sg::Node*>> pending;

    if (!m_gltfAsset.scenes.empty())
    {
        const fastgltf::Scene& gltfScene = m_gltfAsset.scenes[m_gltfAsset.defaultScene.value_or(0)];

        for (size_t index = gltfScene.nodeIndices.size(); index > 0; --index)
        {
            pending.push_back({static_cast<uint32_t>(gltfScene.nodeIndices[index - 1]), nullptr});
        }
    }
    else
    {
        HeapVector<uint8_t> children(m_gltfAsset.nodes.size(), 0);

        for (const fastgltf::Node& node : m_gltfAsset.nodes)
        {
            for (size_t child : node.children)
            {
                children[child] = 1;
            }
        }

        for (size_t index = children.size(); index > 0; --index)
        {
            if (children[index - 1] == 0)
            {
                pending.push_back({static_cast<uint32_t>(index - 1), nullptr});
            }
        }
    }

    while (!pending.empty())
    {
        const std::pair<uint32_t, sg::Node*> entry = pending.back();

        pending.pop_back();

        const size_t firstNode = sgNodes.size();

        LoadGltfRenderableNodes(entry.first, entry.second, sgNodes, pScene);

        sg::Node* parent             = sgNodes[firstNode].Get();

        const fastgltf::Node& source = m_gltfAsset.nodes[entry.first];

        for (size_t index = source.children.size(); index > 0; --index)
        {
            pending.push_back({static_cast<uint32_t>(source.children[index - 1]), parent});
        }
    }

    pScene->SetNodes(std::move(sgNodes));

    pScene->GetAssetData().bindVertices        = HeapVector<Vertex>(m_vertices.data(), m_vertices.size());

    pScene->GetAssetData().bindVertexTexCoords = pScene->GetAssetData().vertexTexCoords;
}

static Mat4 ImportedWorldMatrix(sg::Transform& transform, const sg::Node* parent)
{
    // Import visits parents before their children, and poses are applied only after
    // the complete hierarchy exists. Reuse each published parent world once.
    const Mat4 world = (parent != nullptr ? parent->GetData().modelMatrix : Mat4(1)) * transform.GetPrefixMatrix()
                     * transform.GetLocalMatrix();

    return world;
}

void FastGLTFLoader::LoadGltfRenderableNodes(uint32_t                              nodeIndex,
                                             sg::Node*                             pParent,
                                             zen::HeapVector<UniquePtr<sg::Node>>& sgNodes,
                                             sg::Scene*                            pScene)
{
    const fastgltf::Node& gltfNode     = m_gltfAsset.nodes[nodeIndex];

    UniquePtr<sg::Node> newNode        = MakeUnique<sg::Node>(nodeIndex, std::string(gltfNode.name));

    UniquePtr<sg::Transform> transform = MakeUnique<sg::Transform>(*newNode);

    newNode->SetParent(pParent);

    newNode->visible    = gltfNode.visible;

    newNode->selectable = gltfNode.selectable;

    newNode->hoverable  = gltfNode.hoverable;

    newNode->skinIndex  = gltfNode.skinIndex.has_value() ? static_cast<int32_t>(*gltfNode.skinIndex) : -1;

    for (float weight : gltfNode.weights)
    {
        newNode->morphWeights.push_back(weight);
    }

    const fastgltf::TRS* trs = std::get_if<fastgltf::TRS>(&gltfNode.transform);

    if (trs != nullptr)
    {
        transform->SetTranslation(glm::make_vec3(trs->translation.data()));

        transform->SetRotation(glm::make_quat(trs->rotation.data()));

        transform->SetScale(glm::make_vec3(trs->scale.data()));
    }
    else
    {
        const fastgltf::math::fmat4x4& matrix = std::get<fastgltf::math::fmat4x4>(gltfNode.transform);

        transform->SetLocalMatrix(glm::make_mat4(matrix.data()));
    }

    newNode->AddComponent(transform.Get());

    pScene->AddComponent(std::move(transform));

    const Mat4 world = ImportedWorldMatrix(*newNode->GetComponent<sg::Transform>(), pParent);

    newNode->SetData(static_cast<uint32_t>(pScene->GetRenderableCount()), world);

    if (gltfNode.lightIndex.has_value())
    {
        const fastgltf::Light& source = m_gltfAsset.lights[*gltfNode.lightIndex];

        UniquePtr<sg::Light> light    = MakeUnique<sg::Light>(std::string(source.name));

        light->sourceIndex            = static_cast<uint32_t>(*gltfNode.lightIndex);

        sg::LightProperties properties;

        properties.position = Vec3(world[3]);

        Vec3 direction      = source.type == fastgltf::LightType::Point ? Vec3(0, 0, -1) : -Vec3(world[2]);

        if (!NormalizeSurfaceNormal(direction))
        {
            direction = Vec3(0, 0, -1);
        }

        properties.direction      = Vec4(direction, 0.0f);

        properties.color          = Vec4(glm::make_vec3(source.color.data()), 1.0f);

        properties.intensity      = source.intensity;

        properties.range          = source.range.value_or(0.0f);

        properties.innerConeAngle = source.innerConeAngle.value_or(0.0f);

        properties.outerConeAngle = source.outerConeAngle.value_or(glm::quarter_pi<float>());

        light->SetProperties(properties);

        light->SetType(source.type == fastgltf::LightType::Directional ? sg::Directional
                       : source.type == fastgltf::LightType::Spot      ? sg::Spot
                                                                       : sg::Point);

        newNode->AddComponent(light.Get());

        pScene->AddComponent(std::move(light));
    }

    if (gltfNode.cameraIndex.has_value())
    {
        const fastgltf::Camera& source                   = m_gltfAsset.cameras[*gltfNode.cameraIndex];

        UniquePtr<sg::SceneCamera> camera                = MakeUnique<sg::SceneCamera>(std::string(source.name));

        camera->sourceIndex                              = static_cast<uint32_t>(*gltfNode.cameraIndex);

        camera->worldMatrix                              = world;

        const fastgltf::Camera::Perspective* perspective = std::get_if<fastgltf::Camera::Perspective>(&source.camera);

        if (perspective != nullptr)
        {
            camera->verticalFov = perspective->yfov;

            camera->nearPlane   = perspective->znear;

            camera->infiniteFar = !perspective->zfar.has_value();

            camera->farPlane    = perspective->zfar.value_or(100.0f);

            camera->fixedAspect = perspective->aspectRatio.has_value();

            camera->aspect      = perspective->aspectRatio.value_or(1.0f);
        }
        else
        {
            const fastgltf::Camera::Orthographic& orthographic = std::get<fastgltf::Camera::Orthographic>(source.camera);

            camera->orthographic                               = true;

            camera->xmag                                       = orthographic.xmag;

            camera->ymag                                       = orthographic.ymag;

            camera->nearPlane                                  = orthographic.znear;

            camera->farPlane                                   = orthographic.zfar;
        }

        newNode->AddComponent(camera.Get());

        pScene->AddComponent(std::move(camera));
    }

    if (gltfNode.meshIndex.has_value())
    {
        sg::Mesh* mesh = m_meshes[*gltfNode.meshIndex];

        newNode->AddComponent(mesh);

        if (newNode->morphWeights.empty())
        {
            for (float weight : m_gltfAsset.meshes[*gltfNode.meshIndex].weights)
            {
                newNode->morphWeights.push_back(weight);
            }
        }

        if (gltfNode.instancingAttributes.empty() && !mesh->GetSubMeshes().empty())
        {
            bool hasMorphTargets = false;

            for (const fastgltf::Primitive& primitive : m_gltfAsset.meshes[*gltfNode.meshIndex].primitives)
            {
                hasMorphTargets |= !primitive.targets.empty();
            }

            if (newNode->skinIndex >= 0 || hasMorphTargets)
            {
                CloneDeformedMesh(static_cast<uint32_t>(*gltfNode.meshIndex), *newNode, pScene);
            }
            else
            {
                mesh->AddNode(newNode.Get());
            }

            pScene->AddRenderableNode(newNode.Get());
        }
    }

    if (pParent != nullptr)
    {
        pParent->AddChild(newNode.Get());
    }

    sg::Node* node = newNode.Get();

    sgNodes.push_back(std::move(newNode));

    if (gltfNode.meshIndex && !gltfNode.instancingAttributes.empty())
    {
        size_t instanceCount                   = 0;

        const fastgltf::Accessor* translations = nullptr;

        const fastgltf::Accessor* rotations    = nullptr;

        const fastgltf::Accessor* scales       = nullptr;

        for (const fastgltf::Attribute& attribute : gltfNode.instancingAttributes)
        {
            if (attribute.accessorIndex >= m_gltfAsset.accessors.size())
            {
                LOG_ERROR_AND_THROW("GPU instancing references an invalid accessor");
            }

            const fastgltf::Accessor& accessor = m_gltfAsset.accessors[attribute.accessorIndex];

            const fastgltf::AccessorType expectedType =
                attribute.name == "ROTATION" ? fastgltf::AccessorType::Vec4 : fastgltf::AccessorType::Vec3;

            if ((attribute.name == "TRANSLATION" || attribute.name == "ROTATION" || attribute.name == "SCALE")
                && accessor.type != expectedType)
            {
                LOG_ERROR_AND_THROW("GPU instancing transform accessor has an invalid type");
            }

            if (instanceCount != 0 && instanceCount != accessor.count)
            {
                LOG_ERROR_AND_THROW("GPU instancing attributes have different instance counts");
            }

            instanceCount = accessor.count;

            if (attribute.name == "TRANSLATION")
            {
                translations = &accessor;
            }
            else if (attribute.name == "ROTATION")
            {
                rotations = &accessor;
            }
            else if (attribute.name == "SCALE")
            {
                scales = &accessor;
            }
        }

        sg::Mesh* mesh       = node->GetComponent<sg::Mesh>();

        bool hasMorphTargets = false;

        for (const fastgltf::Primitive& primitive : m_gltfAsset.meshes[*gltfNode.meshIndex].primitives)
        {
            hasMorphTargets |= !primitive.targets.empty();
        }

        for (uint32_t instance = 0; instance < instanceCount; ++instance)
        {
            UniquePtr<sg::Node> instanceNode =
                MakeUnique<sg::Node>(static_cast<uint32_t>(m_gltfAsset.nodes.size() + sgNodes.size()),
                                     node->GetName() + "_instance_" + std::to_string(instance));

            instanceNode->SetParent(node);

            instanceNode->visible                      = true;

            instanceNode->skinIndex                    = node->skinIndex;

            instanceNode->morphWeights                 = node->morphWeights;

            instanceNode->morphWeightsSourceNode       = static_cast<int32_t>(node->GetIndex());

            UniquePtr<sg::Transform> instanceTransform = MakeUnique<sg::Transform>(*instanceNode);

            instanceTransform->SetTranslation(DecodeAttribute(m_gltfAsset, translations, instance, Vec3(0)));

            instanceTransform->SetScale(DecodeAttribute(m_gltfAsset, scales, instance, Vec3(1)));

            const Vec4 rotation = DecodeAttribute(m_gltfAsset, rotations, instance, Vec4(0, 0, 0, 1));

            instanceTransform->SetRotation(glm::quat(rotation.w, rotation.x, rotation.y, rotation.z));

            instanceNode->AddComponent(instanceTransform.Get());

            pScene->AddComponent(std::move(instanceTransform));

            instanceNode->AddComponent(mesh);

            instanceNode->SetData(static_cast<uint32_t>(pScene->GetRenderableCount()),
                                  ImportedWorldMatrix(*instanceNode->GetComponent<sg::Transform>(), node));

            if (instanceNode->skinIndex >= 0 || hasMorphTargets)
            {
                CloneDeformedMesh(static_cast<uint32_t>(*gltfNode.meshIndex), *instanceNode, pScene);
            }
            else
            {
                mesh->AddNode(instanceNode.Get());
            }

            if (!mesh->GetSubMeshes().empty())
            {
                pScene->AddRenderableNode(instanceNode.Get());
            }

            node->AddChild(instanceNode.Get());

            sgNodes.push_back(std::move(instanceNode));
        }
    }
}
} // namespace zen::asset
