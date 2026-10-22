#include "AssetLib/AssetHash.h"
#include "AssetLib/AssetPaths.h"
#include "AssetLib/FbxSceneImporter.h"
#include "AssetLib/FbxDeformation.h"
#include "AssetLib/FbxAnimation.h"
#include "AssetLib/ImageDecoder.h"
#include "AssetLib/SceneAssetProcessing.h"
#include "AssetLib/SurfaceGeometry.h"
#include "Math/Finite.h"
#include "Utils/ThreadPool.h"
#include <ufbx.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>

namespace zen::asset
{
namespace
{
ufbx_progress_result Progress(void* user, const ufbx_progress*)
{
    const SceneImportControl* control = static_cast<const SceneImportControl*>(user);
    return control != nullptr && control->IsCancelled() ? UFBX_PROGRESS_CANCEL : UFBX_PROGRESS_CONTINUE;
}

struct MeshResult
{
    MeshAsset                            mesh;
    HeapVector<Vertex>                   vertices;
    HeapVector<uint32_t>                 indices;
    HeapVector<VertexTextureCoordinates> coordinates;
    HeapVector<VertexAttributeAsset>     attributes;
    MorphPrimitiveAsset                  morph;
    HeapVector<SceneImportDiagnostic>    diagnostics;
};

struct PackedMaterial
{
    uint32_t               material{0};
    MaterialTextureBinding roughness;
    MaterialTextureBinding metalness;
    MaterialTextureBinding opacity;
    bool                   invertRoughness{false};
};

class FbxReader
{
public:
    FbxReader(const SceneImportRequest& request,
              const SceneImportContext& context,
              const ufbx_scene&         source,
              SceneImportResult&        report) :
        m_request(request), m_pool(context.processing), m_source(source), m_report(report)
    {
        m_errors.Reset(request.control.Get());
    }

    UniquePtr<SceneAsset> Read();

private:
    void                   Warn(const std::string& object, const std::string& message);
    void                   CollectUVSets();
    uint32_t               UVSet(ufbx_string name) const;
    const ufbx_uv_set*     MeshUVSet(const ufbx_mesh& mesh, uint32_t slot) const;
    void                   ValidateMeshMaterial(const ufbx_mesh&                   mesh,
                                                const MaterialAsset&               material,
                                                HeapVector<SceneImportDiagnostic>& diagnostics);
    MaterialTextureBinding Texture(const ufbx_material_map& map, bool color);
    void                   Materials();
    void                   DecodeImage(uint32_t index);
    void                   Images();
    void                   NormalMaps();
    void                   DropSupersededTextures();
    void                   PackMaterial(const PackedMaterial& recipe);
    MeshResult             ProcessMesh(uint32_t index);
    void                   Meshes();
    void                   Nodes();
    void                   CamerasAndLights();
    void                   CheckFeatures();

    const SceneImportRequest&               m_request;
    ThreadPool<void, uint32_t>*             m_pool;
    const ufbx_scene&                       m_source;
    SceneImportResult&                      m_report;
    ImportErrors                            m_errors;
    UniquePtr<SceneAsset>                   m_asset;
    HeapVector<std::string>                 m_uvSets;
    HeapVector<uint32_t>                    m_imageFiles;
    HeapVector<SceneDependency>             m_imageDependencies;
    HeapVector<HeapVector<SceneDependency>> m_imageFallbacks;
    HeapVector<PackedMaterial>              m_packing;
};

void FbxReader::Warn(const std::string& object, const std::string& message)
{
    m_report.diagnostics.push_back(
        {SceneDiagnosticSeverity::Warning, SceneImportStatus::UnsupportedFeature, m_request.path, object, message});
}

void FbxReader::CollectUVSets()
{
    // Slot zero is each mesh's default UV set. Named sets have scene-wide slots,
    // so a material can explicitly select a name regardless of a mesh's set order.
    m_uvSets.push_back("");
    for (const ufbx_mesh* mesh : m_source.meshes)
    {
        for (const ufbx_uv_set& set : mesh->uv_sets)
        {
            const std::string name = FbxText(set.name);
            if (std::find(m_uvSets.begin(), m_uvSets.end(), name) == m_uvSets.end())
            {
                m_uvSets.push_back(name);
            }
        }
    }
    for (const ufbx_texture* texture : m_source.textures)
    {
        const std::string name = FbxText(texture->uv_set);
        if (std::find(m_uvSets.begin(), m_uvSets.end(), name) == m_uvSets.end())
        {
            m_uvSets.push_back(name);
        }
    }
    for (uint64_t index = 0; index < m_uvSets.size(); ++index)
    {
        m_asset->vertexTexCoordSets.push_back(index);
    }
}

uint32_t FbxReader::UVSet(ufbx_string name) const
{
    uint32_t          result = 0;
    const std::string text   = FbxText(name);
    for (uint32_t index = 0; index < m_uvSets.size(); ++index)
    {
        if (m_uvSets[index] == text)
        {
            result = index;
        }
    }
    return result;
}

const ufbx_uv_set* FbxReader::MeshUVSet(const ufbx_mesh& mesh, uint32_t slot) const
{
    const ufbx_uv_set* result = slot == 0 && mesh.uv_sets.count > 0 ? &mesh.uv_sets.data[0] : nullptr;
    for (const ufbx_uv_set& set : mesh.uv_sets)
    {
        if (slot > 0 && slot < m_uvSets.size() && FbxText(set.name) == m_uvSets[slot])
        {
            result = &set;
        }
    }
    if (result == nullptr && slot > 0 && m_request.settings.fallbackMissingNamedUVSets && mesh.uv_sets.count > 0)
    {
        result = &mesh.uv_sets.data[0];
    }
    return result;
}

void FbxReader::ValidateMeshMaterial(const ufbx_mesh&                   mesh,
                                     const MaterialAsset&               material,
                                     HeapVector<SceneImportDiagnostic>& diagnostics)
{
    const std::string    meshName = !FbxText(mesh.name).empty() ? FbxText(mesh.name)
                                  : mesh.instances.count > 0    ? FbxText(mesh.instances.data[0]->name)
                                                                : std::to_string(mesh.typed_id);
    HeapVector<uint32_t> warnedSlots;
    const TextureId textures[] = {material.m_pBaseColorTexture, material.m_pMetallicRoughnessTexture, material.m_pNormalTexture,
                                  material.m_pOcclusionTexture, material.m_pEmissiveTexture};
    const uint32_t  slots[] = {material.texCoordSets.baseColor, material.texCoordSets.metallicRoughness,
                               material.texCoordSets.normal, material.texCoordSets.occlusion, material.texCoordSets.emissive};
    for (size_t index = 0; index < std::size(textures); ++index)
    {
        if (textures[index].IsValid())
        {
            const ufbx_uv_set* coordinates = MeshUVSet(mesh, slots[index]);
            if (coordinates == nullptr)
            {
                m_errors.Fail("FBX material " + material.name + " requires a missing UV set on mesh " + meshName);
            }
            else if (slots[index] > 0 && FbxText(coordinates->name) != m_uvSets[slots[index]]
                     && std::find(warnedSlots.begin(), warnedSlots.end(), slots[index]) == warnedSlots.end())
            {
                warnedSlots.push_back(slots[index]);
                diagnostics.push_back({SceneDiagnosticSeverity::Warning, SceneImportStatus::InvalidData, m_request.path,
                                       meshName,
                                       "Material " + material.name + " requests missing UV set '" + m_uvSets[slots[index]]
                                           + "'; using first UV set '" + FbxText(coordinates->name) + "'"});
            }
        }
    }
}

MaterialTextureBinding FbxReader::Texture(const ufbx_material_map& map, bool color)
{
    MaterialTextureBinding result;
    const ufbx_texture*    source = map.texture_enabled ? map.texture : nullptr;
    if (source != nullptr && !m_errors.Failed())
    {
        if (source->type != UFBX_TEXTURE_FILE || !source->has_file)
        {
            m_errors.Fail("Unsupported FBX layered/procedural texture: " + FbxText(source->name),
                          SceneImportStatus::UnsupportedFeature);
        }
        else if (source->file_index >= m_source.texture_files.count)
        {
            m_errors.Fail("Invalid FBX texture file reference: " + FbxText(source->name));
        }
        else
        {
            ImageId image;
            for (uint32_t index = 0; index < m_imageFiles.size(); ++index)
            {
                if (m_imageFiles[index] == source->file_index)
                {
                    image.value = index;
                }
            }
            if (!image.IsValid())
            {
                image.value = static_cast<uint32_t>(m_imageFiles.size());
                m_imageFiles.push_back(source->file_index);
                m_asset->images.push_back({FbxText(source->name), {}});
            }
            SamplerAsset sampler;
            sampler.minFilter = sampler.magFilter = TextureFilter::Linear;
            sampler.wrapS = source->wrap_u == UFBX_WRAP_CLAMP ? SamplerAddressMode::ClampToEdge : SamplerAddressMode::Repeat;
            sampler.wrapT = source->wrap_v == UFBX_WRAP_CLAMP ? SamplerAddressMode::ClampToEdge : SamplerAddressMode::Repeat;
            SamplerId samplerId;
            for (uint32_t index = 0; index < m_asset->samplers.size(); ++index)
            {
                if (m_asset->samplers[index].wrapS == sampler.wrapS && m_asset->samplers[index].wrapT == sampler.wrapT)
                {
                    samplerId.value = index;
                }
            }
            if (!samplerId.IsValid())
            {
                samplerId.value = static_cast<uint32_t>(m_asset->samplers.size());
                m_asset->samplers.push_back(std::move(sampler));
            }
            const Format format = color ? Format::R8G8B8A8_SRGB : Format::R8G8B8A8_UNORM;
            for (uint32_t index = 0; index < m_asset->textures.size(); ++index)
            {
                const TextureAsset& texture = m_asset->textures[index];
                if (texture.image == image && texture.sampler == samplerId && texture.format == format)
                {
                    result.texture.value = index;
                }
            }
            if (!result.texture.IsValid())
            {
                result.texture.value = static_cast<uint32_t>(m_asset->textures.size());
                m_asset->textures.push_back({FbxText(source->name), image, samplerId, {}, format});
            }
            result.texCoord = UVSet(source->uv_set);
            // FBX UVs use a bottom-left origin; engine image rows and UVs use top-left.
            // Conjugate the authored UV transform by V -> 1-V (F * M * F).
            const ufbx_matrix& matrix    = source->uv_to_texture;
            TextureTransform&  transform = result.transform;
            transform.row0               = Vec4(static_cast<float>(matrix.m00), static_cast<float>(-matrix.m01),
                                                static_cast<float>(matrix.m01 + matrix.m03), 0);
            transform.row1               = Vec4(static_cast<float>(-matrix.m10), static_cast<float>(matrix.m11),
                                                static_cast<float>(1.0 - matrix.m11 - matrix.m13), 0);
            transform.uvOffset           = Vec2(transform.row0.z, transform.row1.z);
            transform.uvRotation         = std::atan2(transform.row1.x, transform.row0.x);
            transform.uvScale.x          = glm::length(Vec2(transform.row0.x, transform.row1.x));
            const float determinant      = transform.row0.x * transform.row1.y - transform.row0.y * transform.row1.x;
            transform.uvScale.y          = transform.uvScale.x > 0 ? determinant / transform.uvScale.x : 0;
        }
    }
    return result;
}

void FbxReader::Materials()
{
    for (const ufbx_material* source : m_source.materials)
    {
        MaterialAsset material;
        material.name                     = FbxText(source->name);
        const ufbx_material_pbr_maps& pbr = source->pbr;
        SetFbxMaterialFactors(*source, material);
        MaterialTextureBinding base      = Texture(pbr.base_color, true);
        MaterialTextureBinding normal    = Texture(pbr.normal_map, false);
        MaterialTextureBinding emission  = Texture(pbr.emission_color, true);
        MaterialTextureBinding occlusion = Texture(pbr.ambient_occlusion, false);
        material.m_pBaseColorTexture     = base.texture;
        material.texCoordSets.baseColor  = base.texCoord;
        material.textureTransforms[0]    = base.transform;
        material.m_pNormalTexture        = normal.texture;
        material.texCoordSets.normal     = normal.texCoord;
        material.textureTransforms[2]    = normal.transform;
        material.m_pOcclusionTexture     = occlusion.texture;
        material.texCoordSets.occlusion  = occlusion.texCoord;
        material.textureTransforms[3]    = occlusion.transform;
        material.m_pEmissiveTexture      = emission.texture;
        material.texCoordSets.emissive   = emission.texCoord;
        material.textureTransforms[4]    = emission.transform;
        PackedMaterial recipe;
        recipe.material        = static_cast<uint32_t>(m_asset->materials.size());
        recipe.roughness       = Texture(pbr.roughness, false);
        recipe.metalness       = Texture(pbr.metalness, false);
        recipe.opacity         = Texture(pbr.opacity, false);
        recipe.invertRoughness = source->features.roughness_as_glossiness.enabled;
        if (recipe.roughness.texture.IsValid() || recipe.metalness.texture.IsValid() || recipe.opacity.texture.IsValid())
        {
            m_packing.push_back(recipe);
        }
        if (material.baseColorFactor.a < 1 || recipe.opacity.texture.IsValid())
        {
            material.alphaMode = AlphaMode::Blend;
        }
        const ufbx_material_map* mapped[] = {&pbr.base_color, &pbr.normal_map, &pbr.emission_color, &pbr.ambient_occlusion,
                                             &pbr.roughness,  &pbr.metalness,  &pbr.opacity};
        for (const ufbx_material_map& map : pbr.maps)
        {
            if (map.texture != nullptr && map.texture_enabled
                && std::find(std::begin(mapped), std::end(mapped), &map) == std::end(mapped))
            {
                Warn(material.name,
                     "Unmapped FBX material texture: " + FbxText(map.texture->name) + "; using its scalar factor");
            }
        }
        if (!source->features.pbr.enabled)
        {
            Warn(material.name, "Legacy FBX material approximated with metallic/roughness PBR");
        }
        if (source->fbx.bump.texture != nullptr && source->fbx.bump.texture_enabled)
        {
            Warn(material.name, "FBX height/bump texture is not a tangent-space normal map and is not applied");
        }
        if (!IsFinite(material.baseColorFactor) || !IsFinite(material.emissiveFactor) || !std::isfinite(material.metallicFactor)
            || !std::isfinite(material.roughnessFactor))
        {
            m_errors.Fail("Non-finite FBX material values: " + material.name);
        }
        m_asset->materials.push_back(std::move(material));
    }
}

// The final path component, accepting either separator: FBX files authored on one
// platform are often opened on another.
std::string FileNameOf(const std::string& path)
{
    const size_t separator = path.find_last_of("/\\");
    return separator == std::string::npos ? path : path.substr(separator + 1);
}

// Network and device paths (//server/share, //./pipe, either separator) outside
// the scene's own folder are not opened: an untrusted file could otherwise make
// the importer contact a remote host. Textures beside a scene stored on a share
// still load.
bool OpensOutsideScene(const std::string& path, const std::filesystem::path& sceneDirectory)
{
    const bool network = path.size() >= 2 && (path[0] == '\\' || path[0] == '/') && (path[1] == '\\' || path[1] == '/');
    const std::filesystem::path relative = std::filesystem::u8path(path).lexically_relative(sceneDirectory);
    return network && (relative.empty() || *relative.begin() == "..");
}

// Texture locations in preference order: ufbx's path relative to this file, the
// authored absolute path, then the bare file name beside the FBX, which finds
// textures of relocated assets whose recorded paths are stale.
HeapVector<std::string> TextureCandidates(const ufbx_texture_file& source, const std::string& scenePath)
{
    const std::string relative = FbxText(source.relative_filename);
    const std::string absolute = FbxText(source.absolute_filename);
    const std::string resolved = FbxText(source.filename);
    const std::string name     = FileNameOf(!relative.empty() ? relative : !absolute.empty() ? absolute : resolved);
    const std::string options[]{
        resolved, absolute,
        !name.empty() ? PathToUtf8(std::filesystem::u8path(scenePath).parent_path() / std::filesystem::u8path(name))
                      : std::string()};
    const std::filesystem::path directory =
        std::filesystem::u8path(AbsoluteUtf8Path(std::filesystem::u8path(scenePath))).parent_path();
    HeapVector<std::string> candidates;
    for (const std::string& option : options)
    {
        const std::string path = option.empty() ? option : AbsoluteUtf8Path(std::filesystem::u8path(option));
        if (!path.empty() && !OpensOutsideScene(path, directory)
            && std::find(candidates.begin(), candidates.end(), path) == candidates.end())
        {
            candidates.push_back(path);
        }
    }
    return candidates;
}

bool ReadTextureFile(const std::string& path, HeapVector<uint8_t>& bytes)
{
    std::ifstream        stream(std::filesystem::u8path(path), std::ios::binary | std::ios::ate);
    const std::streamoff length = stream ? static_cast<std::streamoff>(stream.tellg()) : std::streamoff(-1);
    bool                 valid  = length > 0 && static_cast<uint64_t>(length) <= UINT32_MAX;
    if (valid)
    {
        bytes.resize_for_overwrite(static_cast<size_t>(length));
        stream.seekg(0);
        stream.read(reinterpret_cast<char*>(bytes.data()), length);
        valid = static_cast<bool>(stream);
    }
    return valid;
}

void FbxReader::DecodeImage(uint32_t index)
{
    if (!m_errors.Failed())
    {
        const ufbx_texture_file& source = m_source.texture_files.data[m_imageFiles[index]];
        const uint8_t*           data   = static_cast<const uint8_t*>(source.content.data);
        size_t                   size   = source.content.size;
        HeapVector<uint8_t>      bytes;
        if (size == 0)
        {
            bool found = false;
            for (const std::string& candidate : TextureCandidates(source, m_request.path))
            {
                AssetFileStamp stamp;
                if (!found && StampAssetFile(candidate, stamp) && ReadTextureFile(candidate, bytes))
                {
                    found                      = true;
                    m_imageDependencies[index] = {candidate, true, static_cast<uint64_t>(bytes.size()), stamp.modified, 0};
                }
                else if (!found)
                {
                    // A preferred file appearing later changes which texture loads.
                    m_imageFallbacks[index].push_back({candidate, false});
                }
            }
            if (!found)
            {
                m_errors.Fail("Missing, empty or oversized FBX texture: " + FbxText(source.filename),
                              SceneImportStatus::SourceUnavailable);
            }
            data = bytes.data();
            size = bytes.size();
        }
        if (!m_errors.Failed())
        {
            SharedPtr<ImagePixels, MultiThreadCounter> pixels(new ImagePixels);
            ImportErrors                               decoding;
            decoding.Reset(m_request.control.Get());
            pixels->bytes = DecodeImagePixels(data, size, pixels->width, pixels->height, decoding, &pixels->mips);
            if (decoding.Failed() || pixels->bytes.empty() || pixels->width == 0 || pixels->height == 0)
            {
                m_errors.Fail("FBX texture " + FbxText(source.filename) + ": "
                                  + (decoding.Failed() ? decoding.Message() : std::string("decoded no pixels")),
                              decoding.Failed() ? decoding.Status() : SceneImportStatus::InvalidData);
            }
            else
            {
                m_asset->images[index].pixels = pixels;
            }
        }
    }
}

void FbxReader::Images()
{
    m_errors.SetStage(SceneImportStage::DecodingTextures);
    m_imageDependencies.resize(m_imageFiles.size());
    m_imageFallbacks.resize(m_imageFiles.size());
    HeapVector<std::future<void>> pending;
    for (uint32_t index = 0; !m_errors.Failed() && index < m_imageFiles.size(); ++index)
    {
        if (m_pool != nullptr && m_pool->GetSize() > 0)
        {
            pending.push_back(m_pool->Push(&FbxReader::DecodeImage, this, index));
        }
        else
        {
            DecodeImage(index);
        }
    }
    for (std::future<void>& job : pending)
    {
        job.get();
    }
    for (const SceneDependency& dependency : m_imageDependencies)
    {
        if (!dependency.path.empty())
        {
            m_asset->dependencies.push_back(dependency);
        }
    }
    for (const HeapVector<SceneDependency>& fallbacks : m_imageFallbacks)
    {
        for (const SceneDependency& dependency : fallbacks)
        {
            m_asset->dependencies.push_back(dependency);
        }
    }
    for (const PackedMaterial& recipe : m_packing)
    {
        if (!m_errors.Failed())
        {
            PackMaterial(recipe);
        }
    }
    if (!m_errors.Failed())
    {
        NormalMaps();
        DropSupersededTextures();
    }
    for (TextureAsset& color : m_asset->textures)
    {
        if (color.format == Format::R8G8B8A8_SRGB)
        {
            for (uint32_t index = 0; index < m_asset->textures.size(); ++index)
            {
                const TextureAsset& linear = m_asset->textures[index];
                if (linear.image == color.image && linear.sampler == color.sampler && linear.format == Format::R8G8B8A8_UNORM)
                {
                    color.linearSource.value = index;
                }
            }
        }
    }
}

void FbxReader::NormalMaps()
{
    HeapVector<TextureId> converted(m_asset->textures.size());
    HeapVector<ImageId>   convertedImages(m_asset->images.size());
    for (MaterialAsset& material : m_asset->materials)
    {
        TextureId& binding = material.m_pNormalTexture;
        if (binding.IsValid())
        {
            if (!converted[binding.value].IsValid())
            {
                TextureAsset texture = m_asset->textures[binding.value];
                ImageId&     image   = convertedImages[texture.image.value];
                if (!image.IsValid())
                {
                    SharedPtr<ImagePixels, MultiThreadCounter> pixels(
                        new ImagePixels(*m_asset->images[texture.image.value].pixels));
                    // V reversal also reverses the tangent bitangent. FBX normal maps
                    // are interpreted in the authored positive-Y tangent space.
                    for (size_t channel = 1; channel < pixels->bytes.size(); channel += 4)
                    {
                        pixels->bytes[channel] = 255 - pixels->bytes[channel];
                    }
                    for (HeapVector<uint8_t>& mip : pixels->mips)
                    {
                        for (size_t channel = 1; channel < mip.size(); channel += 4)
                        {
                            mip[channel] = 255 - mip[channel];
                        }
                    }
                    image.value = static_cast<uint32_t>(m_asset->images.size());
                    m_asset->images.push_back({texture.name + "_normal", pixels});
                }
                texture.image                   = image;
                texture.name                   += "_normal";
                converted[binding.value].value  = static_cast<uint32_t>(m_asset->textures.size());
                m_asset->textures.push_back(std::move(texture));
            }
            binding = converted[binding.value];
        }
    }
}

// Renumbers the textures materials bind, keeping each texture and its image on
// first use; the linear views are re-derived afterwards.
struct KeepTexture
{
    const SceneAsset&         asset;
    HeapVector<TextureId>&    textures;
    HeapVector<ImageId>&      images;
    HeapVector<TextureAsset>& keptTextures;
    HeapVector<ImageAsset>&   keptImages;

    TextureId operator()(TextureId id) const
    {
        TextureId result = id;
        if (id.IsValid())
        {
            if (!textures[id.value].IsValid())
            {
                TextureAsset texture = asset.textures[id.value];
                ImageId&     image   = images[texture.image.value];
                if (!image.IsValid())
                {
                    image.value = static_cast<uint32_t>(keptImages.size());
                    keptImages.push_back(asset.images[texture.image.value]);
                }
                texture.image            = image;
                texture.linearSource     = {};
                textures[id.value].value = static_cast<uint32_t>(keptTextures.size());
                keptTextures.push_back(std::move(texture));
            }
            result = textures[id.value];
        }
        return result;
    }
};

// Channel packing and normal-map conversion replace their source textures. Only
// textures a material still binds, and their images, stay in the asset, so the
// superseded pixels are neither cached nor uploaded.
void FbxReader::DropSupersededTextures()
{
    HeapVector<TextureId>    textures(m_asset->textures.size());
    HeapVector<ImageId>      images(m_asset->images.size());
    HeapVector<TextureAsset> keptTextures;
    HeapVector<ImageAsset>   keptImages;
    for (MaterialAsset& material : m_asset->materials)
    {
        const MaterialAsset source = material;
        CopyMaterialParameters(material, source, KeepTexture{*m_asset, textures, images, keptTextures, keptImages});
    }
    m_asset->textures = std::move(keptTextures);
    m_asset->images   = std::move(keptImages);
}

bool CompatibleBindings(const MaterialTextureBinding& a, const MaterialTextureBinding& b, const SceneAsset& asset)
{
    return !a.texture.IsValid() || !b.texture.IsValid()
        || (a.texCoord == b.texCoord && a.transform == b.transform
            && asset.textures[a.texture.value].sampler == asset.textures[b.texture.value].sampler);
}

const ImagePixels* Pixels(const SceneAsset& asset, TextureId texture)
{
    return texture.IsValid() ? asset.images[asset.textures[texture.value].image.value].pixels.Get() : nullptr;
}

uint8_t Sample(const ImagePixels* image, uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t channel)
{
    uint8_t result = 255;
    if (image != nullptr)
    {
        const uint32_t sx = static_cast<uint32_t>(static_cast<uint64_t>(x) * image->width / width);
        const uint32_t sy = static_cast<uint32_t>(static_cast<uint64_t>(y) * image->height / height);
        result            = image->bytes[(static_cast<size_t>(sy) * image->width + sx) * 4 + channel];
    }
    return result;
}

// Opacity maps keep coverage in alpha when they have one (a cut-out color image),
// otherwise in their gray level.
bool HasTransparentTexels(const ImagePixels* image)
{
    bool transparent = false;
    for (size_t i = 3; image != nullptr && !transparent && i < image->bytes.size(); i += 4)
    {
        transparent = image->bytes[i] < 255;
    }
    return transparent;
}

void FbxReader::PackMaterial(const PackedMaterial& recipe)
{
    MaterialAsset&               material = m_asset->materials[recipe.material];
    const MaterialTextureBinding base{material.m_pBaseColorTexture, material.texCoordSets.baseColor,
                                      material.textureTransforms[0]};
    if (!CompatibleBindings(recipe.roughness, recipe.metalness, *m_asset)
        || !CompatibleBindings(base, recipe.opacity, *m_asset))
    {
        m_errors.Fail("FBX textures needing channel packing have different UV transforms, UV sets or samplers: "
                          + material.name,
                      SceneImportStatus::UnsupportedFeature);
    }
    else
    {
        for (uint32_t pass = 0; pass < 2; ++pass)
        {
            const bool                    alpha  = pass == 1;
            const MaterialTextureBinding& first  = alpha ? base : recipe.roughness;
            const MaterialTextureBinding& second = alpha ? recipe.opacity : recipe.metalness;
            if ((!alpha && (first.texture.IsValid() || second.texture.IsValid())) || (alpha && second.texture.IsValid()))
            {
                const ImagePixels*                         a              = Pixels(*m_asset, first.texture);
                const ImagePixels*                         b              = Pixels(*m_asset, second.texture);
                const bool                                 opacityInAlpha = alpha && HasTransparentTexels(b);
                SharedPtr<ImagePixels, MultiThreadCounter> pixels(new ImagePixels);
                pixels->width  = std::max(a != nullptr ? a->width : 1u, b != nullptr ? b->width : 1u);
                pixels->height = std::max(a != nullptr ? a->height : 1u, b != nullptr ? b->height : 1u);
                pixels->bytes.resize(static_cast<size_t>(pixels->width) * pixels->height * 4);
                for (uint32_t y = 0; y < pixels->height; ++y)
                {
                    for (uint32_t x = 0; x < pixels->width; ++x)
                    {
                        uint8_t* pixel = &pixels->bytes[(static_cast<size_t>(y) * pixels->width + x) * 4];
                        if (alpha)
                        {
                            for (uint32_t channel = 0; channel < 3; ++channel)
                            {
                                pixel[channel] = Sample(a, x, y, pixels->width, pixels->height, channel);
                            }
                            uint32_t opacity = Sample(b, x, y, pixels->width, pixels->height, 3);
                            if (!opacityInAlpha)
                            {
                                opacity = (static_cast<uint32_t>(Sample(b, x, y, pixels->width, pixels->height, 0))
                                           + Sample(b, x, y, pixels->width, pixels->height, 1)
                                           + Sample(b, x, y, pixels->width, pixels->height, 2))
                                        / 3;
                            }
                            pixel[3] = static_cast<uint8_t>(
                                static_cast<uint32_t>(Sample(a, x, y, pixels->width, pixels->height, 3)) * opacity / 255);
                        }
                        else
                        {
                            const uint8_t roughness = Sample(a, x, y, pixels->width, pixels->height, 0);
                            pixel[0] = pixel[3] = 255;
                            pixel[1] = a != nullptr && recipe.invertRoughness
                                         ? static_cast<uint8_t>(
                                               std::clamp(255.0f - roughness * material.roughnessFactor, 0.0f, 255.0f))
                                         : roughness;
                            pixel[2] = Sample(b, x, y, pixels->width, pixels->height, 0);
                        }
                    }
                }
                const MaterialTextureBinding& binding = first.texture.IsValid() ? first : second;
                TextureAsset                  texture = m_asset->textures[binding.texture.value];
                texture.name                          = material.name + (alpha ? "_base_opacity" : "_metallic_roughness");
                texture.image.value                   = static_cast<uint32_t>(m_asset->images.size());
                texture.format                        = alpha ? Format::R8G8B8A8_SRGB : Format::R8G8B8A8_UNORM;
                texture.linearSource                  = {};
                const TextureId id{static_cast<uint32_t>(m_asset->textures.size())};
                m_asset->images.push_back({texture.name, pixels});
                m_asset->textures.push_back(std::move(texture));
                if (alpha)
                {
                    material.m_pBaseColorTexture    = id;
                    material.texCoordSets.baseColor = binding.texCoord;
                    material.textureTransforms[0]   = binding.transform;
                }
                else
                {
                    material.m_pMetallicRoughnessTexture    = id;
                    material.texCoordSets.metallicRoughness = binding.texCoord;
                    material.textureTransforms[1]           = binding.transform;
                    if (recipe.invertRoughness && a != nullptr)
                    {
                        material.roughnessFactor = 1;
                    }
                }
            }
        }
    }
}

MeshResult FbxReader::ProcessMesh(uint32_t index)
{
    MeshResult       result;
    const ufbx_mesh& source = *m_source.meshes.data[index];
    result.mesh.name        = FbxText(source.name);
    if (source.num_indices > UINT32_MAX || source.num_triangles > UINT32_MAX / 3)
    {
        m_errors.Fail("FBX mesh exceeds 32-bit geometry limits: " + result.mesh.name, SceneImportStatus::ResourceLimit);
    }
    HeapVector<uint32_t>           triangle;
    HeapVector<const ufbx_uv_set*> coordinateSets;
    for (uint32_t slot = 0; slot < m_uvSets.size(); ++slot)
    {
        coordinateSets.push_back(MeshUVSet(source, slot));
    }
    // Corner -> emitted vertex. Every corner belongs to one face and every face to
    // one material part, so one table serves all parts without resets.
    HeapVector<uint32_t> corners;
    if (!m_errors.Failed())
    {
        triangle.resize(source.max_face_triangles * 3);
        corners = HeapVector<uint32_t>(source.num_indices, UINT32_MAX);
    }
    for (const ufbx_mesh_part& part : source.material_parts)
    {
        // FBX material tables can include slots used only by other meshes. Such
        // slots impose no UV requirements on this mesh and produce no primitive.
        if (!m_errors.Failed() && part.num_faces > 0)
        {
            MeshPrimitiveAsset primitive;
            primitive.name         = result.mesh.name + "_" + std::to_string(part.index);
            primitive.firstVertex  = static_cast<uint32_t>(result.vertices.size());
            primitive.firstIndex   = static_cast<uint32_t>(result.indices.size());
            primitive.materialSlot = part.index;
            primitive.flatNormals  = !source.vertex_normal.exists;
            if (part.index < source.materials.count)
            {
                primitive.material.value = source.materials.data[part.index]->typed_id;
            }
            const ufbx_uv_set* normalUV      = nullptr;
            bool               mixedNormalUV = false;
            for (const ufbx_node* instance : source.instances)
            {
                if (part.index < instance->materials.count)
                {
                    const MaterialAsset& material = m_asset->materials[instance->materials.data[part.index]->typed_id];
                    ValidateMeshMaterial(source, material, result.diagnostics);
                    if (material.m_pNormalTexture.IsValid())
                    {
                        const ufbx_uv_set* coordinates  = MeshUVSet(source, material.texCoordSets.normal);
                        mixedNormalUV                  |= normalUV != nullptr && normalUV != coordinates;
                        normalUV                        = coordinates;
                    }
                }
            }
            const ufbx_vertex_vec3* tangents   = normalUV != nullptr ? &normalUV->vertex_tangent : &source.vertex_tangent;
            const ufbx_vertex_vec3* bitangents = normalUV != nullptr ? &normalUV->vertex_bitangent : &source.vertex_bitangent;
            const size_t            attributeBase = result.attributes.size();
            for (size_t set = 1; set < source.color_sets.count; ++set)
            {
                VertexAttributeAsset attribute;
                attribute.mesh          = index;
                attribute.primitive     = static_cast<uint32_t>(result.mesh.primitives.size());
                attribute.semantic      = "COLOR_" + std::to_string(set);
                attribute.components    = 4;
                attribute.componentType = 5126;
                attribute.domain        = VertexAttributeDomain::PrimitiveVertex;
                result.attributes.push_back(std::move(attribute));
            }
            for (uint32_t faceIndex : part.face_indices)
            {
                if (!m_errors.Failed())
                {
                    const ufbx_face face      = source.faces.data[faceIndex];
                    const uint32_t  triangles = ufbx_triangulate_face(triangle.data(), triangle.size(), &source, face);
                    for (uint32_t faceTriangle = 0; faceTriangle < triangles; ++faceTriangle)
                    {
                        const uint32_t* indices = &triangle[faceTriangle * 3];
                        Vec3            flatNormal(0);
                        if (primitive.flatNormals)
                        {
                            const Vec3 p0 = FbxVector(ufbx_get_vertex_vec3(&source.vertex_position, indices[0]));
                            const Vec3 p1 = FbxVector(ufbx_get_vertex_vec3(&source.vertex_position, indices[1]));
                            const Vec3 p2 = FbxVector(ufbx_get_vertex_vec3(&source.vertex_position, indices[2]));
                            flatNormal    = glm::cross(p1 - p0, p2 - p0);
                            NormalizeSurfaceNormal(flatNormal);
                        }
                        for (uint32_t cornerIndex = 0; cornerIndex < 3; ++cornerIndex)
                        {
                            const uint32_t corner = indices[cornerIndex];
                            if (corners[corner] == UINT32_MAX || primitive.flatNormals)
                            {
                                Vertex vertex{};
                                vertex.pos  = Vec4(FbxVector(ufbx_get_vertex_vec3(&source.vertex_position, corner)), 1);
                                Vec3 normal = primitive.flatNormals
                                                ? flatNormal
                                                : FbxVector(ufbx_get_vertex_vec3(&source.vertex_normal, corner));
                                NormalizeSurfaceNormal(normal);
                                vertex.normal = Vec4(normal, 0);
                                vertex.color  = Vec4(1);
                                if (source.vertex_color.exists)
                                {
                                    const ufbx_vec4 color = ufbx_get_vertex_vec4(&source.vertex_color, corner);
                                    vertex.color          = Vec4(color.x, color.y, color.z, color.w);
                                }
                                if (!mixedNormalUV && tangents->exists && bitangents->exists)
                                {
                                    Vec3 tangent = FbxVector(ufbx_get_vertex_vec3(tangents, corner));
                                    NormalizeSurfaceNormal(tangent);
                                    const Vec3 bitangent = FbxVector(ufbx_get_vertex_vec3(bitangents, corner));
                                    vertex.tangent =
                                        Vec4(tangent, glm::dot(glm::cross(normal, tangent), bitangent) < 0 ? 1 : -1);
                                }
                                VertexTextureCoordinates coordinates;
                                coordinates.resize(m_uvSets.size());
                                for (uint32_t slot = 0; slot < coordinateSets.size(); ++slot)
                                {
                                    if (coordinateSets[slot] != nullptr)
                                    {
                                        const ufbx_vec2 uv = ufbx_get_vertex_vec2(&coordinateSets[slot]->vertex_uv, corner);
                                        coordinates[slot]  = Vec2(static_cast<float>(uv.x), static_cast<float>(1.0 - uv.y));
                                        if (!std::isfinite(uv.x) || !std::isfinite(uv.y))
                                        {
                                            m_errors.Fail("Non-finite FBX texture coordinates: " + result.mesh.name);
                                        }
                                    }
                                }
                                vertex.uv0 = coordinates[0];
                                vertex.uv1 = coordinates.size() > 1 ? coordinates[1] : Vec2(0);
                                if (!IsFinite(vertex.pos) || !IsFinite(vertex.normal) || !IsFinite(vertex.color)
                                    || !IsFinite(vertex.tangent))
                                {
                                    m_errors.Fail("Non-finite FBX corner attributes: " + result.mesh.name);
                                }
                                corners[corner] = static_cast<uint32_t>(result.vertices.size());
                                primitive.sourceVertices.push_back(source.vertex_indices.data[corner]);
                                primitive.bounds.SetMin(Vec3(vertex.pos));
                                primitive.bounds.SetMax(Vec3(vertex.pos));
                                result.vertices.push_back(vertex);
                                result.coordinates.push_back(std::move(coordinates));
                                for (size_t set = 1; set < source.color_sets.count; ++set)
                                {
                                    const ufbx_vec4 color =
                                        ufbx_get_vertex_vec4(&source.color_sets.data[set].vertex_color, corner);
                                    VertexAttributeAsset& attribute = result.attributes[attributeBase + set - 1];
                                    for (double component : color.v)
                                    {
                                        if (!std::isfinite(component))
                                        {
                                            m_errors.Fail("Non-finite FBX color attribute: " + result.mesh.name);
                                        }
                                        attribute.values.push_back(component);
                                    }
                                }
                            }
                            result.indices.push_back(corners[corner]);
                        }
                    }
                }
            }
            primitive.vertexCount = static_cast<uint32_t>(result.vertices.size()) - primitive.firstVertex;
            primitive.indexCount  = static_cast<uint32_t>(result.indices.size()) - primitive.firstIndex;
            result.mesh.primitives.push_back(std::move(primitive));
        }
    }
    ConvertFbxMeshDeformation(source, result.mesh, result.vertices, result.morph, m_errors);
    return result;
}

void FbxReader::Meshes()
{
    m_errors.SetStage(SceneImportStage::ProcessingMeshes);
    const bool                          parallel = m_pool != nullptr && m_pool->GetSize() > 0;
    const size_t                        window   = parallel ? m_pool->GetSize() * 2 : 1;
    HeapVector<std::future<MeshResult>> pending(window);
    size_t                              queued = 0;
    for (size_t index = 0; index < m_source.meshes.count; ++index)
    {
        while (parallel && !m_errors.Failed() && queued < m_source.meshes.count && queued < index + window)
        {
            pending[queued % window] = m_pool->Push(&FbxReader::ProcessMesh, this, static_cast<uint32_t>(queued));
            ++queued;
        }
        MeshResult mesh;
        if (parallel && index < queued)
        {
            mesh = pending[index % window].get();
        }
        else if (!parallel && !m_errors.Failed())
        {
            mesh = ProcessMesh(static_cast<uint32_t>(index));
        }
        if (!m_errors.Failed())
        {
            for (SceneImportDiagnostic& diagnostic : mesh.diagnostics)
            {
                m_report.diagnostics.push_back(std::move(diagnostic));
            }
            if (mesh.vertices.size() > UINT32_MAX - m_asset->vertices.size()
                || mesh.indices.size() > UINT32_MAX - m_asset->indices.size())
            {
                m_errors.Fail("FBX scene exceeds 32-bit geometry limits", SceneImportStatus::ResourceLimit);
            }
            else
            {
                const uint32_t vertexBase = static_cast<uint32_t>(m_asset->vertices.size());
                const uint32_t indexBase  = static_cast<uint32_t>(m_asset->indices.size());
                for (MeshPrimitiveAsset& primitive : mesh.mesh.primitives)
                {
                    primitive.firstVertex += vertexBase;
                    primitive.firstIndex  += indexBase;
                }
                for (uint32_t vertex : mesh.indices)
                {
                    m_asset->indices.push_back(vertexBase + vertex);
                }
                for (Vertex& vertex : mesh.vertices)
                {
                    m_asset->vertices.push_back(std::move(vertex));
                }
                for (VertexTextureCoordinates& coordinates : mesh.coordinates)
                {
                    m_asset->vertexTexCoords.push_back(std::move(coordinates));
                }
                m_asset->meshes.push_back(std::move(mesh.mesh));
                if (!mesh.morph.targets.empty())
                {
                    m_asset->morphPrimitives.push_back(std::move(mesh.morph));
                }
                for (VertexAttributeAsset& attribute : mesh.attributes)
                {
                    m_asset->extraVertexAttributes.push_back(std::move(attribute));
                }
            }
        }
    }
}

void FbxReader::Nodes()
{
    for (const ufbx_node* source : m_source.nodes)
    {
        NodeAsset node;
        node.name    = FbxText(source->name);
        node.visible = source->visible;
        if (source->parent != nullptr)
        {
            node.parent.value = source->parent->typed_id;
        }
        else
        {
            m_asset->roots.push_back({source->typed_id});
        }
        for (const ufbx_node* child : source->children)
        {
            node.children.push_back({child->typed_id});
        }
        node.translation         = FbxVector(source->local_transform.translation);
        node.scale               = FbxVector(source->local_transform.scale);
        const ufbx_quat rotation = source->local_transform.rotation;
        node.rotation            = Quat(rotation.w, rotation.x, rotation.y, rotation.z);
        if (!IsFinite(node.translation) || !IsFinite(node.scale) || !IsFinite(node.rotation))
        {
            m_errors.Fail("Non-finite FBX node transform: " + node.name);
        }
        if (source->mesh != nullptr)
        {
            node.mesh.value = source->mesh->typed_id;
            // Only create per-instance bindings when they differ from the mesh defaults.
            bool overrides = source->materials.count != source->mesh->materials.count;
            for (size_t index = 0; !overrides && index < source->materials.count; ++index)
            {
                overrides = source->materials.data[index] != source->mesh->materials.data[index];
            }
            if (overrides)
            {
                for (const ufbx_material* material : source->materials)
                {
                    node.materials.push_back({material->typed_id});
                }
                while (node.materials.size() < source->mesh->material_parts.count)
                {
                    node.materials.push_back(MaterialId{});
                }
            }
        }
        if (source->camera != nullptr)
        {
            node.camera.value = source->camera->typed_id;
        }
        if (source->light != nullptr)
        {
            node.light.value = source->light->typed_id;
        }
        m_asset->nodes.push_back(std::move(node));
    }
}

void FbxReader::CamerasAndLights()
{
    for (const ufbx_camera* source : m_source.cameras)
    {
        CameraAsset camera;
        camera.name         = FbxText(source->name);
        camera.orthographic = source->projection_mode == UFBX_PROJECTION_MODE_ORTHOGRAPHIC;
        camera.verticalFov  = glm::radians(static_cast<float>(source->field_of_view_deg.y));
        camera.aspect       = static_cast<float>(source->aspect_ratio);
        camera.fixedAspect  = true;
        camera.nearPlane    = static_cast<float>(source->near_plane);
        camera.farPlane     = static_cast<float>(source->far_plane);
        camera.infiniteFar  = false;
        camera.xmag         = static_cast<float>(source->orthographic_size.x * 0.5);
        camera.ymag         = static_cast<float>(source->orthographic_size.y * 0.5);
        if (!std::isfinite(camera.nearPlane) || !std::isfinite(camera.farPlane) || camera.nearPlane <= 0
            || camera.farPlane <= camera.nearPlane || !std::isfinite(camera.aspect) || camera.aspect <= 0
            || (!camera.orthographic
                && (!std::isfinite(camera.verticalFov) || camera.verticalFov <= 0 || camera.verticalFov >= glm::pi<float>()))
            || (camera.orthographic
                && (!std::isfinite(camera.xmag) || !std::isfinite(camera.ymag) || camera.xmag <= 0 || camera.ymag <= 0)))
        {
            m_errors.Fail("Invalid FBX camera projection: " + camera.name);
        }
        m_asset->cameras.push_back(std::move(camera));
    }
    for (const ufbx_light* source : m_source.lights)
    {
        LightAsset light;
        light.name           = FbxText(source->name);
        light.kind           = source->type == UFBX_LIGHT_DIRECTIONAL ? LightKind::Directional
                             : source->type == UFBX_LIGHT_SPOT        ? LightKind::Spot
                                                                      : LightKind::Point;
        light.color          = FbxVector(source->color);
        light.intensity      = source->cast_light ? static_cast<float>(source->intensity) : 0;
        light.innerConeAngle = glm::radians(static_cast<float>(source->inner_angle * 0.5));
        light.outerConeAngle = glm::radians(static_cast<float>(source->outer_angle * 0.5));
        if (!IsFinite(light.color) || !std::isfinite(light.intensity) || light.intensity < 0
            || (light.kind == LightKind::Spot
                && (!std::isfinite(light.innerConeAngle) || !std::isfinite(light.outerConeAngle) || light.innerConeAngle < 0
                    || light.outerConeAngle <= 0 || light.innerConeAngle > light.outerConeAngle
                    || light.outerConeAngle > glm::half_pi<float>())))
        {
            m_errors.Fail("Invalid or unrepresentable FBX light values: " + light.name);
        }
        if (source->type == UFBX_LIGHT_AREA || source->type == UFBX_LIGHT_VOLUME)
        {
            m_errors.Fail("Unsupported FBX area/volume light: " + light.name, SceneImportStatus::UnsupportedFeature);
        }
        else
        {
            Warn(light.name, "FBX relative light intensity mapped numerically to engine photometric intensity");
            if (source->type != UFBX_LIGHT_DIRECTIONAL && source->decay != UFBX_LIGHT_DECAY_QUADRATIC)
            {
                Warn(light.name, "FBX light decay approximated with inverse-square falloff");
            }
        }
        m_asset->lights.push_back(std::move(light));
    }
}

void FbxReader::CheckFeatures()
{
    ValidateFbxDeformers(m_source, m_errors);
    if (m_source.constraints.count > 0)
    {
        m_errors.Fail("Unsupported FBX constraints; bake them to node animation before export",
                      SceneImportStatus::UnsupportedFeature);
    }
    for (const ufbx_warning& warning : m_source.metadata.warnings)
    {
        Warn("ufbx element " + std::to_string(warning.element_id), FbxText(warning.description));
    }
    for (const ufbx_mesh* mesh : m_source.meshes)
    {
        if (mesh->num_point_faces > 0 || mesh->num_line_faces > 0)
        {
            m_errors.Fail("Unsupported FBX point/line polygons: " + FbxText(mesh->name), SceneImportStatus::UnsupportedFeature);
        }
        for (bool hole : mesh->face_hole)
        {
            if (hole)
            {
                m_errors.Fail("Unsupported FBX polygon holes: " + FbxText(mesh->name), SceneImportStatus::UnsupportedFeature);
            }
        }
        if (mesh->subdivision_render_levels > 0)
        {
            Warn(FbxText(mesh->name), "FBX subdivision surface imported as its authored control cage");
        }
    }
    for (const ufbx_node* node : m_source.nodes)
    {
        if (node->all_attribs.count > 1)
        {
            m_errors.Fail("Unsupported FBX node with multiple attributes: " + FbxText(node->name),
                          SceneImportStatus::UnsupportedFeature);
        }
        if (node->attrib != nullptr && node->mesh == nullptr && node->camera == nullptr && node->light == nullptr
            && node->bone == nullptr && node->attrib_type != UFBX_ELEMENT_EMPTY)
        {
            m_errors.Fail("Unsupported FBX node attribute: " + FbxText(node->name), SceneImportStatus::UnsupportedFeature);
        }
    }
}

UniquePtr<SceneAsset> FbxReader::Read()
{
    m_asset                 = MakeUnique<SceneAsset>();
    m_asset->name           = PathToUtf8(std::filesystem::u8path(m_request.path).stem());
    m_asset->sourcePath     = m_request.path;
    m_asset->sourceDocument = "{\"sourceUnitMeters\":" + std::to_string(m_source.settings.unit_meters)
                            + ",\"sourceRightAxis\":" + std::to_string(m_source.settings.axes.right)
                            + ",\"sourceUpAxis\":" + std::to_string(m_source.settings.axes.up)
                            + ",\"sourceFrontAxis\":" + std::to_string(m_source.settings.axes.front) + "}";
    CheckFeatures();
    if (!m_errors.Failed())
    {
        CollectUVSets();
        Materials();
        Images();
        Meshes();
    }
    if (!m_errors.Failed())
    {
        Nodes();
        CamerasAndLights();
        BuildFbxSkins(m_source, *m_asset, m_errors);
        BakeFbxAnimations(m_source, *m_asset, m_request.settings.animationBake, m_errors, m_report.diagnostics);
    }
    if (m_errors.Failed())
    {
        const SceneImportStatus status =
            m_request.control && m_request.control->IsCancelled() ? SceneImportStatus::Cancelled : m_errors.Status();
        SetSceneImportFailure(m_report, status, m_request.path,
                              status == SceneImportStatus::Cancelled ? "Scene import cancelled" : m_errors.Message());
        m_asset.Reset();
    }
    return std::move(m_asset);
}

ufbx_scene* LoadFbxScene(const SceneImportRequest& request, bool retainDom, ufbx_error& error)
{
    ufbx_load_opts options{};
    options.target_axes                   = ufbx_axes_right_handed_y_up;
    options.target_camera_axes            = ufbx_axes_right_handed_y_up;
    options.target_light_axes             = ufbx_axes_right_handed_y_up;
    options.retain_dom                    = retainDom;
    options.target_unit_meters            = 1;
    options.space_conversion              = UFBX_SPACE_CONVERSION_MODIFY_GEOMETRY;
    options.geometry_transform_handling   = UFBX_GEOMETRY_TRANSFORM_HANDLING_HELPER_NODES;
    options.inherit_mode_handling         = UFBX_INHERIT_MODE_HANDLING_HELPER_NODES;
    options.use_blender_pbr_material      = true;
    options.index_error_handling          = UFBX_INDEX_ERROR_HANDLING_ABORT_LOADING;
    options.node_depth_limit              = 4096;
    options.temp_allocator.memory_limit   = size_t(8) << 30;
    options.result_allocator.memory_limit = size_t(8) << 30;
    options.progress_cb.fn                = Progress;
    options.progress_cb.user              = request.control.Get();
    return ufbx_load_file(request.path.c_str(), &options, &error);
}
} // namespace

SceneImportResult FbxSceneImporter::Import(const SceneImportRequest& request, const SceneImportContext& context) const
{
    SceneImportResult result;
    if (request.settings.sceneIndex != UINT32_MAX && request.settings.sceneIndex != 0)
    {
        SetSceneImportFailure(result, SceneImportStatus::InvalidRequest, request.path,
                              "FBX contains one scene; select scene zero");
    }
    else
    {
        ufbx_error  error{};
        ufbx_scene* source = LoadFbxScene(request, false, error);
        // Only skin clusters need the raw document (their link mode is not parsed),
        // and retaining it copies every array; reparse just those files with it.
        if (source != nullptr && source->skin_clusters.count > 0)
        {
            ufbx_free_scene(source);
            source = LoadFbxScene(request, true, error);
        }
        if (source != nullptr)
        {
            FbxReader             reader(request, context, *source, result);
            UniquePtr<SceneAsset> asset = reader.Read();
            if (asset && ProcessImportedAsset(*asset, result, request.path))
            {
                result.asset.Reset(asset.Get());
                asset.Release();
                result.status = SceneImportStatus::Success;
            }
            ufbx_free_scene(source);
        }
        else
        {
            char description[1024]{};
            ufbx_format_error(description, sizeof(description), &error);
            const SceneImportStatus status = error.type == UFBX_ERROR_CANCELLED    ? SceneImportStatus::Cancelled
                                           : error.type == UFBX_ERROR_MEMORY_LIMIT ? SceneImportStatus::ResourceLimit
                                                                                   : SceneImportStatus::InvalidData;
            SetSceneImportFailure(result, status, request.path, description);
        }
    }
    return result;
}
} // namespace zen::asset
