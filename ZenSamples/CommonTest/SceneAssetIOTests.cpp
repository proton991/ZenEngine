#include "AssetLib/SceneAssetIO.h"
#include "AssetLib/SceneRuntimeBuilder.h"
#include "AssetLib/AnimatedMaterial.h"
#include "SceneGraph/SceneAnimation.h"
#include "SceneGraph/Transform.h"
#include "../../ZenCore/Source/AssetLib/AssetHash.h"
#include "../../ZenCore/Source/AssetLib/SceneAssetProcessing.h"
#include <gtest/gtest.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>

using namespace zen;

namespace
{
class AssetFiles
{
public:
    AssetFiles()
    {
        root = std::filesystem::temp_directory_path()
             / ("zen_asset_io_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::error_code error;
        std::filesystem::create_directory(root, error);
        EXPECT_FALSE(error);
    }

    ~AssetFiles()
    {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }

    std::string Path(const char* name) const
    {
        return (root / name).string();
    }

    std::filesystem::path root;
};

std::string Fixture(const char* name)
{
    return (std::filesystem::path(__FILE__).parent_path() / "Assets" / name).string();
}

std::string FileBytes(const std::string& path)
{
    std::ifstream input(std::filesystem::u8path(path), std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

std::streamoff WrittenFileBytes(const std::filesystem::path& path)
{
    // Directory enumeration can cache a zero length for an open file on Windows.
    // Observe its current end through a separate handle, closed before cancellation.
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    return input ? static_cast<std::streamoff>(input.tellg()) : std::streamoff(-1);
}

void PutBytes(const std::string& path, const std::string& bytes)
{
    std::ofstream output(std::filesystem::u8path(path), std::ios::binary);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    EXPECT_TRUE(output);
}

void ComparePose(asset::LoadedScene& actual, asset::LoadedScene& expected, uint32_t clip, float time)
{
    ASSERT_TRUE(sg::EvaluateSceneAnimation(*actual.scene, clip, time, false));
    ASSERT_TRUE(sg::EvaluateSceneAnimation(*expected.scene, clip, time, false));
    ASSERT_TRUE(
        sg::ApplySceneDeformations(*actual.scene, MakeVecView(actual.scene->GetAssetData().bindVertices), actual.vertices));
    ASSERT_TRUE(sg::ApplySceneDeformations(*expected.scene, MakeVecView(expected.scene->GetAssetData().bindVertices),
                                           expected.vertices));
    ASSERT_EQ(actual.vertices.size(), expected.vertices.size());
    for (size_t vertex = 0; vertex < actual.vertices.size(); ++vertex)
    {
        EXPECT_EQ(actual.vertices[vertex].pos, expected.vertices[vertex].pos);
        EXPECT_EQ(actual.vertices[vertex].normal, expected.vertices[vertex].normal);
        EXPECT_EQ(actual.vertices[vertex].tangent, expected.vertices[vertex].tangent);
    }
    ASSERT_EQ(actual.scene->GetNodes().size(), expected.scene->GetNodes().size());
    for (size_t node = 0; node < actual.scene->GetNodes().size(); ++node)
    {
        EXPECT_EQ(actual.scene->GetNodes()[node]->GetData().modelMatrix,
                  expected.scene->GetNodes()[node]->GetData().modelMatrix);
    }
    const HeapVector<sg::Material*> actualMaterials   = actual.scene->GetComponents<sg::Material>();
    const HeapVector<sg::Material*> expectedMaterials = expected.scene->GetComponents<sg::Material>();
    ASSERT_EQ(actualMaterials.size(), expectedMaterials.size());
    for (size_t material = 0; material < actualMaterials.size(); ++material)
    {
        EXPECT_EQ(actualMaterials[material]->baseColorFactor, expectedMaterials[material]->baseColorFactor);
        EXPECT_EQ(static_cast<const asset::MaterialFeatureFactors&>(actualMaterials[material]->features),
                  static_cast<const asset::MaterialFeatureFactors&>(expectedMaterials[material]->features));
    }
}

uint64_t LittleU64(const std::string& bytes, size_t offset)
{
    uint64_t result = 0;
    for (size_t byte = 0; byte < 8; ++byte)
    {
        result |= uint64_t(static_cast<uint8_t>(bytes[offset + byte])) << (byte * 8);
    }
    return result;
}

constexpr size_t HeaderBytes = 48;

void RefreshChecksum(std::string& bytes)
{
    asset::AssetHash hash;
    hash.Update(bytes.data() + HeaderBytes, bytes.size() - HeaderBytes);
    const asset::AssetDigest digest = hash.Finish();
    for (size_t byte = 0; byte < 8; ++byte)
    {
        bytes[40 + byte] = static_cast<char>(digest >> (byte * 8));
    }
}

std::string OnlyCache(const AssetFiles& files)
{
    std::string path;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(files.root / "cache"))
    {
        if (entry.path().extension() == ".zenasset")
        {
            EXPECT_TRUE(path.empty());
            path = entry.path().string();
        }
    }
    return path;
}
} // namespace

TEST(SceneAssetIO, Xxh64MatchesKnownAnswersAcrossChunkBoundaries)
{
    asset::AssetHash hash;
    EXPECT_EQ(asset::DigestText(hash.Finish()), "ef46db3751d8e999");
    hash.Update("a", 1);
    hash.Update("bc", 2);
    EXPECT_EQ(asset::DigestText(hash.Finish()), "44bc2cf5ad770999");
    asset::AssetHash  large;
    const std::string block(1000, 'a');
    for (uint32_t i = 0; i < 1000; ++i)
    {
        // Uneven updates cross the 32-byte stripe boundaries.
        large.Update(block.data(), 7);
        large.Update(block.data() + 7, block.size() - 7);
    }
    EXPECT_EQ(asset::DigestText(large.Finish()), "dc483aaa9b4fdc40");
    const AssetFiles files;
    PutBytes(files.Path("input"), std::string(1000000, 'a'));
    asset::AssetDigest digest{};
    ASSERT_TRUE(asset::HashAssetFile(files.Path("input"), digest));
    EXPECT_EQ(digest, large.Finish());
    asset::SceneImportControl cancelled;
    cancelled.Cancel();
    EXPECT_FALSE(asset::HashAssetFile(files.Path("input"), digest, &cancelled));
    EXPECT_FALSE(asset::HashAssetFile(files.Path("missing"), digest));
    EXPECT_FALSE(asset::HashAssetFile(files.root.string(), digest));
}

TEST(SceneAssetIO, RoundTripsSourceIndependentStaticAndAnimatedAssets)
{
    const AssetFiles files;
    for (const char* fixture : {"complete_scene.glb", "typed_animation_targets.gltf",
                                "FBX/blender_293_embedded_textures_7400_binary.fbx", "FBX/combined_morph_skin_ascii.fbx"})
    {
        SCOPED_TRACE(fixture);
        asset::SceneImportResult imported = asset::ImportScene({Fixture(fixture)});
        ASSERT_TRUE(imported.asset) << imported.error;
        asset::SceneAssetFileMetadata metadata;
        metadata.sourceFormat    = imported.format;
        metadata.importerVersion = imported.importerVersion;
        metadata.diagnostics     = imported.diagnostics;
        const asset::SceneImportReport written =
            asset::WriteSceneAssetFile(files.Path("first.zenasset"), *imported.asset, metadata);
        ASSERT_EQ(written.status, asset::SceneImportStatus::Success) << written.error;
        asset::SceneAssetFileResult read = asset::ReadSceneAssetFile(files.Path("first.zenasset"));
        ASSERT_TRUE(read.asset) << read.error;
        EXPECT_EQ(read.metadata.sourceFormat, imported.format);
        ASSERT_EQ(read.asset->images.size(), imported.asset->images.size());
        for (size_t image = 0; image < read.asset->images.size(); ++image)
        {
            const asset::ImagePixels& actual   = *read.asset->images[image].pixels;
            const asset::ImagePixels& expected = *imported.asset->images[image].pixels;
            EXPECT_EQ(actual.width, expected.width);
            EXPECT_EQ(actual.height, expected.height);
            ASSERT_EQ(actual.bytes.size(), expected.bytes.size());
            EXPECT_TRUE(std::equal(actual.bytes.begin(), actual.bytes.end(), expected.bytes.begin()));
        }
        const asset::SceneImportReport rewritten =
            asset::WriteSceneAssetFile(files.Path("second.zenasset"), *read.asset, read.metadata);
        ASSERT_EQ(rewritten.status, asset::SceneImportStatus::Success) << rewritten.error;
        EXPECT_EQ(FileBytes(files.Path("first.zenasset")), FileBytes(files.Path("second.zenasset")));
        asset::SceneLoadResult runtime   = asset::BuildSceneRuntime(read.asset);
        asset::SceneLoadResult reference = asset::BuildSceneRuntime(imported.asset);
        ASSERT_TRUE(runtime.scene) << runtime.error;
        ASSERT_TRUE(reference.scene) << reference.error;
        ASSERT_EQ(runtime.scene->vertices.size(), reference.scene->vertices.size());
        for (size_t vertex = 0; vertex < runtime.scene->vertices.size(); ++vertex)
        {
            EXPECT_EQ(runtime.scene->vertices[vertex].pos, reference.scene->vertices[vertex].pos);
            EXPECT_EQ(runtime.scene->vertices[vertex].normal, reference.scene->vertices[vertex].normal);
        }
        EXPECT_EQ(runtime.scene->scene->GetAABB(), reference.scene->scene->GetAABB());
        for (uint32_t clip = 0; clip < imported.asset->animations.size(); ++clip)
        {
            for (float time : {0.0f, 0.2f, 0.7f, 1.0f})
            {
                ComparePose(*runtime.scene, *reference.scene, clip, time);
            }
        }
    }
}

TEST(SceneAssetIO, LoadsAndAnimatesWithoutSourceFilesOrMetadata)
{
    const AssetFiles  files;
    const std::string source = files.Path("source.fbx");
    PutBytes(source, FileBytes(Fixture("FBX/combined_morph_skin_ascii.fbx")));
    asset::SceneImportResult imported = asset::ImportScene({source});
    ASSERT_TRUE(imported.asset) << imported.error;
    asset::SceneAsset standalone = *imported.asset;
    standalone.sourcePath.clear();
    standalone.sourceDocument.clear();
    standalone.dependencies.clear();
    const std::string file = files.Path("scene.ZENASSET");
    ASSERT_EQ(asset::WriteSceneAssetFile(file, standalone).status, asset::SceneImportStatus::Success);
    std::error_code error;
    ASSERT_TRUE(std::filesystem::remove(std::filesystem::u8path(source), error));
    EXPECT_FALSE(asset::ImportScene({source}).asset);
    EXPECT_EQ(asset::ImportScene({file}).status, asset::SceneImportStatus::UnsupportedFormat);
    asset::SceneLoader                  loader(2, 2);
    std::future<asset::SceneLoadResult> future    = loader.LoadAsync(file);
    asset::SceneLoadResult              loaded    = future.get();
    asset::SceneLoadResult              reference = asset::BuildSceneRuntime(imported.asset);
    ASSERT_TRUE(loaded.scene) << loaded.error;
    ASSERT_TRUE(reference.scene) << reference.error;
    EXPECT_EQ(loaded.format, "zenasset");
    EXPECT_EQ(loaded.scene->path, file);
    ASSERT_FALSE(loaded.asset->animations.empty());
    ComparePose(*loaded.scene, *reference.scene, 0, 0.7f);
}

TEST(SceneAssetIO, RetainsExtendedMaterialsMipLevelsCustomAttributesAndEnvironmentPayloads)
{
    const AssetFiles               files;
    const asset::SceneImportResult imported = asset::ImportScene({Fixture("complete_scene.glb")});
    ASSERT_TRUE(imported.asset) << imported.error;
    asset::SceneAsset source = *imported.asset;
    ASSERT_FALSE(source.materials.empty());
    ASSERT_FALSE(source.textures.empty());
    ASSERT_FALSE(source.images.empty());
    ASSERT_FALSE(source.meshes.empty());
    ASSERT_FALSE(source.meshes[0].primitives.empty());
    asset::MaterialAsset& material = source.materials[0];
    for (uint32_t field = uint32_t(asset::AnimatedProperty::BaseColor);
         field < uint32_t(asset::AnimatedProperty::TextureOffset); ++field)
    {
        VectorView<float> values =
            asset::GetAnimatedMaterialProperty<asset::TextureId>(material, static_cast<asset::AnimatedProperty>(field));
        for (size_t component = 0; component < values.size(); ++component)
        {
            values[component] = 1.25f + field * 0.01f + component * 0.1f;
        }
    }
    for (size_t slot = 0; slot < asset::MaterialFeatures::textureMembers.size(); ++slot)
    {
        asset::MaterialTextureBinding& binding = material.features.*asset::MaterialFeatures::textureMembers[slot];
        binding.texture                        = asset::TextureId{0};
        binding.texCoord                       = 0;
        binding.scale                          = 0.5f + slot;
        binding.transform.uvOffset             = Vec2(0.1f * slot, -0.2f * slot);
        binding.transform.uvScale              = Vec2(2, 3);
        binding.transform.uvRotation           = 0.25f;
    }
    for (size_t slot = 0; slot < 5; ++slot)
    {
        material.textureTransforms[slot].uvRotation = 0.5f + slot;
    }
    SharedPtr<asset::ImagePixels, MultiThreadCounter> pixels(new asset::ImagePixels(*source.images[0].pixels));
    ASSERT_EQ(pixels->width, 1u);
    ASSERT_EQ(pixels->height, 1u);
    pixels->mips.clear();
    pixels->mips.push_back(pixels->bytes);
    source.images[0].pixels = std::move(pixels);
    asset::ImageBasedLightAsset environment;
    environment.size                   = 1;
    environment.mipLevels              = 1;
    environment.intensity              = 2.75f;
    environment.irradianceCoefficients = HeapVector<Vec3>(9, Vec3(0.25f));
    environment.specularMipFaces.resize(6);
    for (size_t face = 0; face < 6; ++face)
    {
        environment.specularMipFaces[face].push_back(Vec4(float(face) + 0.5f));
    }
    source.imageBasedLights.push_back(environment);
    source.imageBasedLight = 0;
    asset::VertexAttributeAsset custom;
    custom.semantic      = "_EXACT_INTEGER";
    custom.componentType = 5125;
    custom.components    = 1;
    custom.domain        = asset::VertexAttributeDomain::PrimitiveVertex;
    custom.values        = HeapVector<double>(source.meshes[0].primitives[0].vertexCount, double(UINT32_MAX));
    source.extraVertexAttributes.push_back(custom);
    const std::string              file    = files.Path("features.zenasset");
    const asset::SceneImportReport written = asset::WriteSceneAssetFile(file, source);
    ASSERT_EQ(written.status, asset::SceneImportStatus::Success) << written.error;
    const asset::SceneAssetFileResult loaded = asset::ReadSceneAssetFile(file);
    ASSERT_TRUE(loaded.asset) << loaded.error;
    const asset::MaterialAsset& actual = loaded.asset->materials[0];
    EXPECT_EQ(actual.features, material.features);
    EXPECT_EQ(actual.baseColorFactor, material.baseColorFactor);
    EXPECT_EQ(actual.extension.diffuseFactor, material.extension.diffuseFactor);
    EXPECT_EQ(actual.extension.specularFactor, material.extension.specularFactor);
    EXPECT_EQ(actual.metallicFactor, material.metallicFactor);
    EXPECT_EQ(actual.roughnessFactor, material.roughnessFactor);
    for (size_t slot = 0; slot < 5; ++slot)
    {
        EXPECT_EQ(actual.textureTransforms[slot], material.textureTransforms[slot]);
    }
    ASSERT_EQ(loaded.asset->images[0].pixels->mips.size(), 1u);
    EXPECT_TRUE(std::equal(loaded.asset->images[0].pixels->mips[0].begin(), loaded.asset->images[0].pixels->mips[0].end(),
                           source.images[0].pixels->mips[0].begin(), source.images[0].pixels->mips[0].end()));
    ASSERT_EQ(loaded.asset->imageBasedLights.size(), 1u);
    EXPECT_EQ(loaded.asset->imageBasedLights[0].intensity, environment.intensity);
    for (size_t face = 0; face < 6; ++face)
    {
        EXPECT_EQ(loaded.asset->imageBasedLights[0].specularMipFaces[face][0], environment.specularMipFaces[face][0]);
    }
    ASSERT_FALSE(loaded.asset->extraVertexAttributes.empty());
    EXPECT_EQ(loaded.asset->extraVertexAttributes.back().values[0], double(UINT32_MAX));
}

TEST(SceneAssetIO, RejectsSemanticCorruptionEvenWithAValidChecksum)
{
    const AssetFiles         files;
    asset::SceneImportResult source = asset::ImportScene({Fixture("complete_scene.glb")});
    ASSERT_TRUE(source.asset) << source.error;
    const std::string file = files.Path("scene.zenasset");
    ASSERT_EQ(asset::WriteSceneAssetFile(file, *source.asset).status, asset::SceneImportStatus::Success);
    std::string bytes = FileBytes(file);
    // Section one starts with the source document, then the node records.
    const size_t document = HeaderBytes + static_cast<size_t>(LittleU64(bytes, 16));
    const size_t records  = document + 8 + static_cast<size_t>(LittleU64(bytes, document));
    ASSERT_GT(LittleU64(bytes, records), 0u);
    const size_t parent = records + 16 + static_cast<size_t>(LittleU64(bytes, records + 8));
    ASSERT_LT(parent + 4, bytes.size());
    bytes.replace(parent, 4, std::string(4, char(0x7f)));
    RefreshChecksum(bytes);
    PutBytes(file, bytes);
    EXPECT_EQ(asset::ReadSceneAssetFile(file).status, asset::SceneImportStatus::InvalidData);
    EXPECT_FALSE(asset::LoadScene(file).scene);

    ASSERT_EQ(asset::WriteSceneAssetFile(file, *source.asset).status, asset::SceneImportStatus::Success);
    std::string invalidText = FileBytes(file);
    ASSERT_GT(LittleU64(invalidText, records + 8), 0u);
    invalidText[records + 16] = char(0xff);
    RefreshChecksum(invalidText);
    PutBytes(file, invalidText);
    EXPECT_FALSE(asset::ReadSceneAssetFile(file).asset);
    PutBytes(file, bytes);

    for (uint32_t mutation = 0; mutation < 8; ++mutation)
    {
        SCOPED_TRACE(mutation);
        asset::SceneAsset invalid = *source.asset;
        switch (mutation)
        {
            case 0: invalid.indices[0] = UINT32_MAX; break;
            case 1: invalid.vertices[0].pos.x = std::numeric_limits<float>::quiet_NaN(); break;
            case 2: invalid.meshes[0].primitives[0].topology = static_cast<asset::MeshTopology>(99); break;
            case 3: invalid.nodes[0].rotation = Quat(0, 0, 0, 0); break;
            case 4: invalid.textures[0].image.value = UINT32_MAX - 1; break;
            case 5: invalid.materials[0].alphaMode = static_cast<asset::AlphaMode>(99); break;
            case 6: invalid.imageBasedLight = INT32_MAX; break;
            case 7:
            {
                SharedPtr<asset::ImagePixels, MultiThreadCounter> pixels(new asset::ImagePixels(*invalid.images[0].pixels));
                pixels->width            = UINT32_MAX;
                invalid.images[0].pixels = std::move(pixels);
                break;
            }
        }
        EXPECT_FALSE(asset::ValidateSceneAsset(invalid));
        EXPECT_EQ(asset::WriteSceneAssetFile(file, invalid).status, asset::SceneImportStatus::InvalidData);
        EXPECT_EQ(FileBytes(file), bytes);
    }
    asset::SceneImportResult animated = asset::ImportScene({Fixture("FBX/combined_morph_skin_ascii.fbx")});
    ASSERT_TRUE(animated.asset) << animated.error;
    for (uint32_t mutation = 0; mutation < 5; ++mutation)
    {
        asset::SceneAsset invalid = *animated.asset;
        switch (mutation)
        {
            case 0: invalid.skins[0].joints[0] = UINT32_MAX; break;
            case 1: invalid.morphPrimitives[0].mesh = UINT32_MAX; break;
            case 2: invalid.animations[0].samplers[0].values.pop_back(); break;
            case 3: invalid.animations[0].channels[0].sampler = UINT32_MAX; break;
            case 4: invalid.animations[0].samplers[0].times[0] = -1; break;
        }
        EXPECT_FALSE(asset::ValidateSceneAsset(invalid));
    }
}

TEST(SceneAssetCache, DefaultRequestsPublishInTheBackgroundAndOptOutSkipsTheCache)
{
    const AssetFiles          files;
    asset::SceneLoader        loader(1, 2);
    asset::SceneImportRequest request{Fixture("FBX/maya_cube_7500_binary.fbx")};
    request.cacheDirectory                    = files.Path("cache");
    asset::SceneImportRequest uncachedRequest = request;
    uncachedRequest.useCache                  = false;
    asset::SceneLoadResult direct             = loader.LoadAsync(uncachedRequest).get();
    ASSERT_TRUE(direct.scene) << direct.error;
    EXPECT_FALSE(direct.cacheHit);
    EXPECT_TRUE(asset::WaitForSceneAssetCacheWrites().empty());
    EXPECT_FALSE(std::filesystem::exists(std::filesystem::u8path(request.cacheDirectory)));

    asset::SceneLoadResult first = loader.LoadAsync(request).get();
    ASSERT_TRUE(first.scene) << first.error;
    EXPECT_FALSE(first.cacheHit);
    EXPECT_TRUE(asset::WaitForSceneAssetCacheWrites().empty());
    const std::string cache = OnlyCache(files);
    EXPECT_TRUE(loader.LoadAsync(request).get().cacheHit);
    const std::filesystem::file_time_type used     = std::filesystem::last_write_time(std::filesystem::u8path(cache));
    asset::SceneLoadResult                uncached = loader.LoadAsync(uncachedRequest).get();
    ASSERT_TRUE(uncached.scene) << uncached.error;
    EXPECT_FALSE(uncached.cacheHit);
    EXPECT_EQ(std::filesystem::last_write_time(std::filesystem::u8path(cache)), used);
}

TEST(SceneAssetCache, EvictsLeastRecentlyUsedFilesBeyondTheBudget)
{
    const AssetFiles  files;
    const std::string cube = files.Path("cube.fbx");
    PutBytes(cube, FileBytes(Fixture("FBX/maya_cube_7500_binary.fbx")));
    asset::SceneImportRequest first{cube};
    first.cacheDirectory = files.Path("cache");
    ASSERT_TRUE(asset::LoadScene(first).scene);
    ASSERT_TRUE(asset::WaitForSceneAssetCacheWrites().empty());
    const std::string                     older = OnlyCache(files);
    const std::filesystem::file_time_type stale = std::filesystem::file_time_type::clock::now() - std::chrono::hours(2);
    std::filesystem::last_write_time(std::filesystem::u8path(older), stale);
    const std::string abandoned = files.Path("cache/stale.zenasset.tmp-1-1");
    PutBytes(abandoned, "left by a crashed writer");
    std::filesystem::last_write_time(std::filesystem::u8path(abandoned), stale);
    asset::SceneImportRequest second{Fixture("complete_scene.glb")};
    second.cacheDirectory   = first.cacheDirectory;
    second.cacheBudgetBytes = 1;
    ASSERT_TRUE(asset::LoadScene(second).scene);
    ASSERT_TRUE(asset::WaitForSceneAssetCacheWrites().empty());
    EXPECT_FALSE(std::filesystem::exists(std::filesystem::u8path(older)));
    EXPECT_FALSE(std::filesystem::exists(std::filesystem::u8path(abandoned)));
    EXPECT_FALSE(OnlyCache(files).empty());
    EXPECT_TRUE(asset::LoadScene(second).cacheHit);
}

TEST(SceneAssetCache, InvalidatesSourceSettingsAndCorruptPayloads)
{
    const AssetFiles  files;
    const std::string source = files.Path("source.fbx");
    PutBytes(source, FileBytes(Fixture("FBX/maya_cube_7500_binary.fbx")));
    asset::SceneImportRequest request{source};
    request.useCache             = true;
    request.cacheDirectory       = files.Path("cache");
    asset::SceneLoadResult first = asset::LoadScene(request);
    ASSERT_TRUE(first.scene) << first.error;
    EXPECT_FALSE(first.cacheHit);
    ASSERT_TRUE(asset::WaitForSceneAssetCacheWrites().empty());
    asset::SceneLoadResult cached = asset::LoadScene(request);
    ASSERT_TRUE(cached.scene) << cached.error;
    EXPECT_TRUE(cached.cacheHit);
    EXPECT_EQ(first.scene->vertices[0].pos, cached.scene->vertices[0].pos);
    const std::string cache  = OnlyCache(files);
    std::string       bytes  = FileBytes(cache);
    bytes.back()            ^= 0x7f;
    PutBytes(cache, bytes);
    EXPECT_FALSE(asset::ReadSceneAssetFile(cache).asset);
    asset::SceneLoadResult rebuilt = asset::LoadScene(request);
    ASSERT_TRUE(rebuilt.scene) << rebuilt.error;
    EXPECT_FALSE(rebuilt.cacheHit);
    ASSERT_TRUE(asset::WaitForSceneAssetCacheWrites().empty());
    EXPECT_TRUE(asset::ReadSceneAssetFile(cache).asset);
    for (bool importerVersion : {true, false})
    {
        asset::SceneAssetFileResult old = asset::ReadSceneAssetFile(cache);
        ASSERT_TRUE(old.asset);
        if (importerVersion)
        {
            ++old.metadata.importerVersion;
        }
        else
        {
            ++old.metadata.processorVersion;
        }
        old.metadata.cacheKey = asset::SceneAssetCacheKey(old.metadata);
        ASSERT_EQ(asset::WriteSceneAssetFile(cache, *old.asset, old.metadata).status, asset::SceneImportStatus::Success);
        asset::SceneLoadResult version = asset::LoadScene(request);
        ASSERT_TRUE(version.scene) << version.error;
        EXPECT_FALSE(version.cacheHit);
        ASSERT_TRUE(asset::WaitForSceneAssetCacheWrites().empty());
    }
    const std::filesystem::file_time_type timestamp = std::filesystem::last_write_time(std::filesystem::u8path(source));
    PutBytes(source, FileBytes(Fixture("FBX/maya_cube_7500_ascii.fbx")));
    std::filesystem::last_write_time(std::filesystem::u8path(source), timestamp);
    asset::SceneLoadResult changed = asset::LoadScene(request);
    ASSERT_TRUE(changed.scene) << changed.error;
    EXPECT_FALSE(changed.cacheHit);
    ASSERT_TRUE(asset::WaitForSceneAssetCacheWrites().empty());
    EXPECT_TRUE(asset::LoadScene(request).cacheHit);
    request.settings.fallbackMissingNamedUVSets = false;
    asset::SceneLoadResult settings             = asset::LoadScene(request);
    ASSERT_TRUE(settings.scene) << settings.error;
    EXPECT_FALSE(settings.cacheHit);
    ASSERT_TRUE(asset::WaitForSceneAssetCacheWrites().empty());
    EXPECT_TRUE(asset::LoadScene(request).cacheHit);
    request.useCache = false;
    EXPECT_FALSE(asset::LoadScene(request).cacheHit);
}

TEST(SceneAssetCache, ParallelRequestsPublishCompleteFilesAndFailuresStillLoadSources)
{
    const AssetFiles          files;
    asset::SceneLoader        loader(4, 4);
    asset::SceneImportRequest request{Fixture("complete_scene.glb")};
    request.useCache       = true;
    request.cacheDirectory = files.Path("cache");
    HeapVector<std::future<asset::SceneLoadResult>> futures;
    for (uint32_t index = 0; index < 4; ++index)
    {
        futures.push_back(loader.LoadAsync(request));
    }
    for (std::future<asset::SceneLoadResult>& future : futures)
    {
        asset::SceneLoadResult result = future.get();
        ASSERT_TRUE(result.scene) << result.error;
    }
    ASSERT_TRUE(asset::WaitForSceneAssetCacheWrites().empty());
    const std::string file = OnlyCache(files);
    EXPECT_TRUE(asset::ReadSceneAssetFile(file).asset);
    EXPECT_TRUE(asset::LoadScene(request).cacheHit);
    asset::SceneImportRequest fbx{Fixture("FBX/combined_morph_skin_ascii.fbx")};
    fbx.useCache       = true;
    fbx.cacheDirectory = request.cacheDirectory;
    asset::SceneImportRequest cancelled{file};
    cancelled.control.Reset(new asset::SceneImportControl);
    cancelled.control->Cancel();
    futures.push_back(loader.LoadAsync(request));
    futures.push_back(loader.LoadAsync(fbx));
    futures.push_back(loader.LoadAsync(file));
    std::future<asset::SceneLoadResult> failure = loader.LoadAsync(cancelled);
    for (std::future<asset::SceneLoadResult>& future : futures)
    {
        if (future.valid())
        {
            asset::SceneLoadResult result = future.get();
            EXPECT_TRUE(result.scene) << result.error;
        }
    }
    EXPECT_EQ(failure.get().status, asset::SceneImportStatus::Cancelled);
    ASSERT_TRUE(asset::WaitForSceneAssetCacheWrites().empty());
    request.cacheDirectory = files.Path("blocked");
    PutBytes(request.cacheDirectory, "A file occupies the requested directory");
    asset::SceneLoadResult unwritable = asset::LoadScene(request);
    ASSERT_TRUE(unwritable.scene) << unwritable.error;
    // Publication runs after the load returns; the writer reports its failure.
    const HeapVector<asset::SceneImportDiagnostic> failures = asset::WaitForSceneAssetCacheWrites();
    ASSERT_EQ(failures.size(), 1u);
    EXPECT_NE(failures[0].message.find("cache could not be saved"), std::string::npos);
}

TEST(SceneAssetCache, TracksExternalTextureAndBufferContentsWithEscapedUris)
{
    const AssetFiles  files;
    const std::string source = files.Path("source.gltf");
    PutBytes(source, R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],
        "buffers":[{"uri":"mesh%20data.bin","byteLength":60}],
        "bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":24}],
        "accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},
                     {"bufferView":1,"componentType":5126,"count":3,"type":"VEC2"}],
        "images":[{"uri":"pixel%20image.tga"}],"textures":[{"source":0}],
        "materials":[{"pbrMetallicRoughness":{"baseColorTexture":{"index":0}}}],
        "meshes":[{"primitives":[{"attributes":{"POSITION":0,"TEXCOORD_0":1},"material":0}]}]})");
    float             geometry[]{0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 1};
    const std::string buffer = files.Path("mesh data.bin");
    PutBytes(buffer, std::string(reinterpret_cast<const char*>(geometry), sizeof(geometry)));
    uint8_t           tga[]{0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 0, 24, 32, 0, 0, 255};
    const std::string texture = files.Path("pixel image.tga");
    PutBytes(texture, std::string(reinterpret_cast<const char*>(tga), sizeof(tga)));
    asset::SceneImportRequest request{source};
    request.useCache             = true;
    request.cacheDirectory       = files.Path("cache");
    asset::SceneLoadResult first = asset::LoadScene(request);
    ASSERT_TRUE(first.scene) << first.error;
    EXPECT_FALSE(first.cacheHit);
    ASSERT_EQ(first.asset->dependencies.size(), 2u);
    ASSERT_TRUE(asset::WaitForSceneAssetCacheWrites().empty());
    EXPECT_TRUE(asset::LoadScene(request).cacheHit);
    const std::filesystem::file_time_type textureTime = std::filesystem::last_write_time(std::filesystem::u8path(texture));
    tga[18]                                           = 255;
    tga[20]                                           = 0;
    PutBytes(texture, std::string(reinterpret_cast<const char*>(tga), sizeof(tga)));
    std::filesystem::last_write_time(std::filesystem::u8path(texture), textureTime);
    asset::SceneLoadResult blue = asset::LoadScene(request);
    ASSERT_TRUE(blue.scene) << blue.error;
    EXPECT_FALSE(blue.cacheHit);
    EXPECT_EQ(blue.asset->images[0].pixels->bytes[0], 0u);
    EXPECT_EQ(blue.asset->images[0].pixels->bytes[2], 255u);
    ASSERT_TRUE(asset::WaitForSceneAssetCacheWrites().empty());
    EXPECT_TRUE(asset::LoadScene(request).cacheHit);
    const std::filesystem::file_time_type bufferTime = std::filesystem::last_write_time(std::filesystem::u8path(buffer));
    geometry[3]                                      = 0.5f;
    PutBytes(buffer, std::string(reinterpret_cast<const char*>(geometry), sizeof(geometry)));
    std::filesystem::last_write_time(std::filesystem::u8path(buffer), bufferTime);
    asset::SceneLoadResult smaller = asset::LoadScene(request);
    ASSERT_TRUE(smaller.scene) << smaller.error;
    EXPECT_FALSE(smaller.cacheHit);
    EXPECT_EQ(smaller.scene->vertices[1].pos.x, 0.5f);
    ASSERT_TRUE(asset::WaitForSceneAssetCacheWrites().empty());
    EXPECT_TRUE(asset::LoadScene(request).cacheHit);
    const std::string cache = OnlyCache(files);
    std::error_code   error;
    ASSERT_TRUE(std::filesystem::remove(std::filesystem::u8path(source), error));
    ASSERT_TRUE(std::filesystem::remove(std::filesystem::u8path(texture), error));
    ASSERT_TRUE(std::filesystem::remove(std::filesystem::u8path(buffer), error));
    EXPECT_FALSE(asset::LoadScene(request).scene);
    asset::SceneLoadResult standalone = asset::LoadScene(cache);
    ASSERT_TRUE(standalone.scene) << standalone.error;
    EXPECT_EQ(standalone.asset->images[0].pixels->bytes[2], 255u);
}

TEST(SceneAssetIO, CancellationDuringBulkWriteKeepsPreviousAssetAndCleansTemporaryFile)
{
    const AssetFiles         files;
    asset::SceneImportResult imported = asset::ImportScene({Fixture("complete_scene.glb")});
    ASSERT_TRUE(imported.asset) << imported.error;
    const std::string destination = files.Path("saved.zenasset");
    ASSERT_EQ(asset::WriteSceneAssetFile(destination, *imported.asset).status, asset::SceneImportStatus::Success);
    const std::string                                 previous = FileBytes(destination);
    asset::SceneAsset                                 large    = *imported.asset;
    SharedPtr<asset::ImagePixels, MultiThreadCounter> pixels(new asset::ImagePixels);
    pixels->width  = 8192;
    pixels->height = 4096;
    pixels->bytes.resize(size_t(pixels->width) * pixels->height * 4);
    large.images[0].pixels = std::move(pixels);
    asset::SceneImportControl             control;
    std::future<asset::SceneImportReport> pending = std::async(std::launch::async, &asset::WriteSceneAssetFile, destination,
                                                               std::cref(large), asset::SceneAssetFileMetadata{}, &control);
    const std::chrono::steady_clock::time_point deadline    = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    bool                                        interrupted = false;
    while (!interrupted && pending.wait_for(std::chrono::seconds(0)) != std::future_status::ready
           && std::chrono::steady_clock::now() < deadline)
    {
        for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(files.root))
        {
            if (entry.path().filename().string().find(".tmp-") != std::string::npos
                && WrittenFileBytes(entry.path()) > 1024 * 1024)
            {
                interrupted = true;
                control.Cancel();
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const asset::SceneImportReport result = pending.get();
    ASSERT_TRUE(interrupted);
    EXPECT_EQ(result.status, asset::SceneImportStatus::Cancelled);
    EXPECT_EQ(FileBytes(destination), previous);
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(files.root))
    {
        EXPECT_EQ(entry.path().filename().string().find(".tmp-"), std::string::npos);
    }
}

TEST(SceneAssetIO, RejectsTruncationBadHeadersCountsChecksumAndBudgets)
{
    const AssetFiles         files;
    asset::SceneImportResult source = asset::ImportScene({Fixture("complete_scene.glb")});
    ASSERT_TRUE(source.asset) << source.error;
    ASSERT_EQ(asset::WriteSceneAssetFile(files.Path("good.zenasset"), *source.asset).status, asset::SceneImportStatus::Success);
    const std::string original = FileBytes(files.Path("good.zenasset"));
    ASSERT_GT(original.size(), 80u);
    for (size_t size : {size_t(0), size_t(7), HeaderBytes - 1, original.size() - 1})
    {
        PutBytes(files.Path("bad.zenasset"), original.substr(0, size));
        EXPECT_FALSE(asset::ReadSceneAssetFile(files.Path("bad.zenasset")).asset);
    }
    for (size_t offset : {size_t(0), size_t(8), size_t(12), size_t(16), size_t(40), HeaderBytes, original.size() - 1})
    {
        SCOPED_TRACE(offset);
        std::string corrupt  = original;
        corrupt[offset]     ^= 0x7f;
        PutBytes(files.Path("bad.zenasset"), corrupt);
        EXPECT_FALSE(asset::ReadSceneAssetFile(files.Path("bad.zenasset")).asset);
    }
    std::string excessiveCount = original;
    excessiveCount.replace(HeaderBytes, 8, std::string(8, char(0xff)));
    PutBytes(files.Path("bad.zenasset"), excessiveCount);
    EXPECT_FALSE(asset::ReadSceneAssetFile(files.Path("bad.zenasset")).asset);
    asset::SceneAssetReadLimits limits;
    limits.maxAllocationBytes = 1;
    EXPECT_EQ(asset::ReadSceneAssetFile(files.Path("good.zenasset"), limits).status, asset::SceneImportStatus::ResourceLimit);
    limits              = {};
    limits.maxFileBytes = 1;
    EXPECT_EQ(asset::ReadSceneAssetFile(files.Path("good.zenasset"), limits).status, asset::SceneImportStatus::ResourceLimit);
    limits                         = {};
    limits.maxRuntimeGeometryBytes = 1;
    EXPECT_FALSE(asset::ReadSceneAssetFile(files.Path("good.zenasset"), limits).asset);
}

TEST(SceneAssetIO, CancelledAndFailedWritesPreserveTheDestination)
{
    const AssetFiles         files;
    asset::SceneImportResult source = asset::ImportScene({Fixture("complete_scene.glb")});
    ASSERT_TRUE(source.asset) << source.error;
    ASSERT_EQ(asset::WriteSceneAssetFile(files.Path("saved.zenasset"), *source.asset).status,
              asset::SceneImportStatus::Success);
    const std::string         before = FileBytes(files.Path("saved.zenasset"));
    asset::SceneImportControl cancelled;
    cancelled.Cancel();
    EXPECT_EQ(asset::WriteSceneAssetFile(files.Path("saved.zenasset"), *source.asset, {}, &cancelled).status,
              asset::SceneImportStatus::Cancelled);
    EXPECT_EQ(FileBytes(files.Path("saved.zenasset")), before);
    EXPECT_EQ(asset::ReadSceneAssetFile(files.Path("saved.zenasset"), {}, &cancelled).status,
              asset::SceneImportStatus::Cancelled);
    std::error_code error;
    std::filesystem::create_directory(files.root / "directory.zenasset", error);
    EXPECT_EQ(asset::WriteSceneAssetFile(files.Path("directory.zenasset"), *source.asset).status,
              asset::SceneImportStatus::IOError);
    EXPECT_TRUE(std::filesystem::is_directory(files.root / "directory.zenasset"));
    EXPECT_EQ(asset::WriteSceneAssetFile(files.Path("missing/asset.zenasset"), *source.asset).status,
              asset::SceneImportStatus::IOError);
    asset::SceneAsset documented = *source.asset;
    documented.sourceDocument.assign(4096, ' ');
    ASSERT_EQ(asset::WriteSceneAssetFile(files.Path("documented.zenasset"), documented).status,
              asset::SceneImportStatus::Success);
    asset::SceneAssetReadLimits strings;
    strings.maxStringBytes = 1024;
    EXPECT_EQ(asset::ReadSceneAssetFile(files.Path("documented.zenasset"), strings).status,
              asset::SceneImportStatus::ResourceLimit);
    // The source document lives outside the metadata section that cache lookups read.
    EXPECT_EQ(asset::ReadSceneAssetMetadata(files.Path("documented.zenasset")).status, asset::SceneImportStatus::Success);
    EXPECT_EQ(FileBytes(files.Path("saved.zenasset")), before);
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(files.root))
    {
        EXPECT_EQ(entry.path().filename().string().find(".tmp-"), std::string::npos);
    }
}

TEST(SceneAssetProcessing, DropsUnplayableClipsAndMismatchedWeightsInsteadOfRejectingTheScene)
{
    asset::SceneImportResult imported = asset::ImportScene({Fixture("FBX/combined_morph_skin_ascii.fbx")});
    ASSERT_TRUE(imported.asset) << imported.error;
    asset::SceneAsset processed = *imported.asset;
    ASSERT_FALSE(processed.animations.empty());
    const size_t clips           = processed.animations.size();
    processed.animations[0].name = "Broken";
    ASSERT_GT(processed.animations[0].samplers[0].times.size(), 1u);
    processed.animations[0].samplers[0].times[1] = processed.animations[0].samplers[0].times[0];
    asset::NodeAsset* morphed                    = nullptr;
    for (asset::NodeAsset& node : processed.nodes)
    {
        morphed = node.mesh.IsValid() && morphed == nullptr ? &node : morphed;
    }
    ASSERT_NE(morphed, nullptr);
    morphed->morphWeights = HeapVector<float>(17, 0.25f);
    asset::SceneImportResult report;
    ASSERT_TRUE(asset::ProcessImportedAsset(processed, report, "scene.fbx")) << report.error;
    EXPECT_EQ(processed.animations.size(), clips - 1);
    EXPECT_TRUE(morphed->morphWeights.empty());
    bool skipped = false;
    for (const asset::SceneImportDiagnostic& diagnostic : report.diagnostics)
    {
        skipped |= diagnostic.object == "Broken" && diagnostic.message.find("strictly increasing") != std::string::npos;
    }
    EXPECT_TRUE(skipped);
    EXPECT_TRUE(
        asset::BuildSceneRuntime(SharedPtr<const asset::SceneAsset, MultiThreadCounter>(new asset::SceneAsset(processed)))
            .scene);

    asset::SceneAsset invalid = *imported.asset;
    invalid.nodes[0].rotation = Quat(0, 0, 0, 0);
    asset::SceneImportResult failed;
    EXPECT_FALSE(asset::ProcessImportedAsset(invalid, failed, "scene.fbx"));
    EXPECT_NE(failed.error.find("node transforms"), std::string::npos) << failed.error;
    std::string reason;
    EXPECT_FALSE(asset::ValidateSceneAsset(invalid, 16ull * 1024 * 1024 * 1024, &reason));
    EXPECT_NE(reason.find("node transforms"), std::string::npos) << reason;
}
