#include "AssetTools/SceneTools.h"
#include "AssetLib/SceneAssetIO.h"
#include "AssetLib/SceneRuntimeBuilder.h"
#include "AssetLib/AnimatedMaterial.h"
#include "AssetLib/ProcessedSceneAdapter.h"
#include "AssetLib/ProcessedSceneIO.h"
#include "AssetLib/SceneAssetIOInternal.h"
#include "ZenAsset/Processor/Cook.h"
#include "ZenAsset/Processor/Import.h"
#include "ZenAsset/Processor/Write.h"
#include "SceneGraph/SceneAnimation.h"
#include "SceneGraph/Transform.h"
#include "AssetLib/AssetHash.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <thread>
#if defined(_WIN32)
#    include <Windows.h>
#endif

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
    return (std::filesystem::path(ZEN_ASSET_TEST_CORPUS) / name).string();
}

std::string FileBytes(const std::string& path)
{
    std::ifstream input(std::filesystem::u8path(path), std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

std::streamoff WrittenFileBytes(const std::filesystem::path& path)
{
    // Directory enumeration can cache a zero length for an open file on Windows.
    // Query attributes without requesting access to the exclusive writer's bytes.
    std::streamoff bytes = -1;
#if defined(_WIN32)
    const HANDLE handle =
        CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle != INVALID_HANDLE_VALUE)
    {
        LARGE_INTEGER size{};
        if (GetFileSizeEx(handle, &size))
        {
            bytes = static_cast<std::streamoff>(size.QuadPart);
        }
        CloseHandle(handle);
    }
#else
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    bytes = input ? static_cast<std::streamoff>(input.tellg()) : std::streamoff(-1);
#endif
    return bytes;
}

bool WaitForCacheTemporaryFile(const std::string& directory, std::future<asset::SceneLoadResult>& pending)
{
    // Compressed padding can remain tiny; temporary-file creation marks the
    // serialization window before atomic publication regardless of payload size.
    const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    bool                                        writing  = false;
    while (!writing && pending.wait_for(std::chrono::seconds(0)) != std::future_status::ready
           && std::chrono::steady_clock::now() < deadline)
    {
        std::error_code                           error;
        std::filesystem::directory_iterator       entry(directory, error);
        const std::filesystem::directory_iterator end;
        while (!error && entry != end)
        {
            if (entry->path().filename().string().find(".zenasset.tmp.") != std::string::npos
                && WrittenFileBytes(entry->path()) >= 0)
            {
                writing = true;
            }
            entry.increment(error);
        }
        std::this_thread::yield();
    }
    return writing;
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

void WriteComparableScene(const std::string& path, const asset::SceneAsset& source)
{
    // The package reader can omit a UV stream whose two sets already live in every vertex.
    // Expand that representation before comparing the complete serialized scene.
    asset::SceneAsset scene = source;
    if (scene.vertexTexCoords.empty())
    {
        scene.vertexTexCoords.resize(scene.vertices.size());
        for (size_t i = 0; i < scene.vertices.size(); ++i)
        {
            scene.vertexTexCoords[i].push_back(scene.vertices[i].uv0);
            scene.vertexTexCoords[i].push_back(scene.vertices[i].uv1);
        }
    }
    ASSERT_EQ(asset::WriteSceneAssetFile(path, scene).status, asset::SceneImportStatus::Success);
}

void CompareSceneRecords(const AssetFiles& files, const asset::SceneAsset& actual, const asset::SceneAsset& expected)
{
    const std::string actualPath   = files.Path("actual.zenasset");
    const std::string expectedPath = files.Path("expected.zenasset");
    ASSERT_NO_FATAL_FAILURE(WriteComparableScene(actualPath, actual));
    ASSERT_NO_FATAL_FAILURE(WriteComparableScene(expectedPath, expected));
    EXPECT_TRUE(FileBytes(actualPath) == FileBytes(expectedPath));
}

void CompareCompressedEnvironmentScene(const AssetFiles&        files,
                                       const asset::SceneAsset& actual,
                                       const asset::SceneAsset& expected)
{
    ASSERT_EQ(actual.imageBasedLights.size(), expected.imageBasedLights.size());
    EXPECT_EQ(actual.imageBasedLight, expected.imageBasedLight);
    for (size_t index = 0; index < actual.imageBasedLights.size(); ++index)
    {
        const asset::ImageBasedLightAsset& a = actual.imageBasedLights[index];
        const asset::ImageBasedLightAsset& b = expected.imageBasedLights[index];
        EXPECT_EQ(a.name, b.name);
        EXPECT_EQ(a.intensity, b.intensity);
        EXPECT_EQ(a.rotation, b.rotation);
        EXPECT_EQ(a.size, b.size);
        EXPECT_EQ(a.mipLevels, b.mipLevels);
        EXPECT_EQ(a.specularFormat, b.specularFormat);
        EXPECT_TRUE(a.specularMipFaces.empty());
        EXPECT_TRUE(b.specularMipFaces.empty());
        ASSERT_EQ(a.irradianceCoefficients.size(), b.irradianceCoefficients.size());
        EXPECT_TRUE(
            std::equal(a.irradianceCoefficients.begin(), a.irradianceCoefficients.end(), b.irradianceCoefficients.begin()));
        ASSERT_TRUE(a.specularBlockMipFaces);
        ASSERT_TRUE(b.specularBlockMipFaces);
        ASSERT_EQ(a.specularBlockMipFaces->size(), b.specularBlockMipFaces->size());
        for (size_t face = 0; face < a.specularBlockMipFaces->size(); ++face)
        {
            const HeapVector<uint8_t>& aBytes = (*a.specularBlockMipFaces)[face];
            const HeapVector<uint8_t>& bBytes = (*b.specularBlockMipFaces)[face];
            ASSERT_EQ(aBytes.size(), bBytes.size());
            EXPECT_TRUE(std::equal(aBytes.begin(), aBytes.end(), bBytes.begin()));
        }
    }
    // The legacy serializer provides an exact comparison for all other records;
    // compressed environment records are compared explicitly above.
    asset::SceneAsset actualRemainder   = actual;
    asset::SceneAsset expectedRemainder = expected;
    actualRemainder.imageBasedLights.clear();
    expectedRemainder.imageBasedLights.clear();
    actualRemainder.imageBasedLight = expectedRemainder.imageBasedLight = -1;
    CompareSceneRecords(files, actualRemainder, expectedRemainder);
}

std::string ReorderedSource(const AssetFiles& files)
{
    const float    positions[]{0, 0, 0, 1, 0, 0, 0, 1, 0, 7, 7, 7};
    const float    normals[]{0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1};
    const uint16_t indices[]{2, 0, 1};
    std::string    bytes(reinterpret_cast<const char*>(positions), sizeof(positions));
    bytes.append(reinterpret_cast<const char*>(normals), sizeof(normals));
    bytes.append(reinterpret_cast<const char*>(indices), sizeof(indices));
    PutBytes(files.Path("geometry.bin"), bytes);
    const std::string source = files.Path("reordered.gltf");
    PutBytes(source, R"({"asset":{"version":"2.0"},"buffers":[{"uri":"geometry.bin","byteLength":102}],
"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":48},{"buffer":0,"byteOffset":48,"byteLength":48},
{"buffer":0,"byteOffset":96,"byteLength":6}],
"accessors":[{"bufferView":0,"componentType":5126,"count":4,"type":"VEC3","min":[0,0,0],"max":[7,7,7]},
{"bufferView":1,"componentType":5126,"count":4,"type":"VEC3"},
{"bufferView":2,"componentType":5123,"count":3,"type":"SCALAR"}],
"meshes":[{"primitives":[{"attributes":{"POSITION":0,"NORMAL":1},"indices":2}]}],
"nodes":[{"mesh":0}],"scenes":[{"nodes":[0]}],"scene":0})");
    return source;
}

void ExpectFileProgress(const asset::SceneImportControl& control,
                        uint64_t                         processedFiles,
                        uint64_t                         totalFiles,
                        bool                             totalKnown = true)
{
    const asset::SceneImportFileProgress progress = control.GetFileProgress();
    EXPECT_EQ(progress.processedFiles, processedFiles);
    EXPECT_EQ(progress.totalFiles, totalFiles);
    EXPECT_EQ(progress.totalKnown, totalKnown);
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
    EXPECT_FALSE(asset::HashAssetFile(files.Path("input"), digest, nullptr, &cancelled));
    EXPECT_FALSE(asset::HashAssetFile(files.Path("missing"), digest));
    EXPECT_FALSE(asset::HashAssetFile(files.root.string(), digest));
}

TEST(SceneAssetIO, FileProgressTracksSourceFilesAndCachedPackages)
{
    const AssetFiles          files;
    asset::SceneImportRequest request{ReorderedSource(files)};
    request.cacheDirectory = files.Path("cache");
    request.control.Reset(new asset::SceneImportControl);
    const asset::SceneLoadResult source = asset::LoadSourceScene(request);
    ASSERT_TRUE(source.scene) << source.error;
    EXPECT_FALSE(source.cacheHit);
    // One glTF root and one external geometry buffer, regardless of mesh count.
    ExpectFileProgress(*request.control, 2, 2);
    ASSERT_TRUE(asset::WaitForSceneAssetCacheWrites().empty());
    const std::string cache = OnlyCache(files);
    ASSERT_FALSE(cache.empty());

    asset::SceneImportControl packageControl;
    packageControl.SetFileProgress({3, 7, false});
    const asset::SceneAssetFileResult metadata = asset::ReadSceneAssetMetadata(cache, &packageControl);
    ASSERT_EQ(metadata.status, asset::SceneImportStatus::Success) << metadata.error;
    // Cache inspection is not a completed input-file import.
    ExpectFileProgress(packageControl, 3, 7, false);
    const asset::SceneAssetFileResult package = asset::ReadSceneAssetFile(cache, {}, &packageControl);
    ASSERT_TRUE(package.asset) << package.error;
    ExpectFileProgress(packageControl, 1, 1);
    const asset::SceneImportCacheProgress resources = packageControl.GetCacheProgress();
    EXPECT_GT(resources.totalResources, 1u);
    EXPECT_EQ(resources.completedResources, resources.totalResources);

    request.control.Reset(new asset::SceneImportControl);
    const asset::SceneLoadResult cached = asset::LoadSourceScene(request);
    ASSERT_TRUE(cached.scene) << cached.error;
    EXPECT_TRUE(cached.cacheHit);
    EXPECT_EQ(request.control->GetCacheProgress().completedResources, resources.totalResources);
    // Dependency freshness checks do not inflate the single package count.
    ExpectFileProgress(*request.control, 1, 1);
}

TEST(SceneAssetIO, FileProgressDoesNotCompleteFailedOrCancelledPackageReads)
{
    const AssetFiles          files;
    asset::SceneImportRequest request{Fixture("complete_scene.glb")};
    request.control.Reset(new asset::SceneImportControl);
    const asset::SceneImportResult source = asset::ImportScene(request);
    ASSERT_TRUE(source.asset) << source.error;
    // Embedded buffers and images belong to this one physical input file.
    ExpectFileProgress(*request.control, 1, 1);
    const std::string package = files.Path("valid.zenasset");
    ASSERT_EQ(asset::WriteSceneAssetFile(package, *source.asset).status, asset::SceneImportStatus::Success);
    PutBytes(files.Path("invalid.zenasset"), "not a scene package");
    for (const char* filename : {"missing.zenasset", "invalid.zenasset"})
    {
        SCOPED_TRACE(filename);
        asset::SceneImportControl         control;
        const asset::SceneAssetFileResult failed = asset::ReadSceneAssetFile(files.Path(filename), {}, &control);
        EXPECT_FALSE(failed.asset);
        EXPECT_NE(failed.status, asset::SceneImportStatus::Success);
        ExpectFileProgress(control, 0, 1);
    }
    asset::SceneImportControl cancelled;
    cancelled.Cancel();
    const asset::SceneAssetFileResult stopped = asset::ReadSceneAssetFile(package, {}, &cancelled);
    EXPECT_FALSE(stopped.asset);
    EXPECT_EQ(stopped.status, asset::SceneImportStatus::Cancelled);
    ExpectFileProgress(cancelled, 0, 1);
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
    asset::SourceSceneLoader            loader(2, 2);
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
    EXPECT_FALSE(asset::LoadSourceScene(file).scene);

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
    asset::SourceSceneLoader  loader(1, 2);
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

TEST(SceneAssetCache, MigratesCurrentFlatGenerationAndPreservesDirectOpens)
{
    const AssetFiles          files;
    asset::SceneImportRequest request{Fixture("complete_scene.glb")};
    request.cacheDirectory = files.Path("cache");
    request.forceRecook    = true;
    ASSERT_TRUE(asset::LoadSourceScene(request).scene);
    const std::string flat          = OnlyCache(files);
    request.forceRecook             = false;
    request.directoryCache          = true;
    asset::SceneLoadResult migrated = asset::LoadSourceScene(request);
    ASSERT_TRUE(migrated.scene) << migrated.error;
    EXPECT_TRUE(migrated.cacheHit);
    EXPECT_TRUE(asset::LoadScene(flat).scene);
    const std::filesystem::path model = std::filesystem::u8path(request.cacheDirectory) / "complete_scene";
    std::string                 manifest;
    for (const std::filesystem::directory_entry& file : std::filesystem::directory_iterator(model))
    {
        if (file.path().extension() == ".zenasset")
        {
            manifest = file.path().string();
        }
    }
    ASSERT_FALSE(manifest.empty());
    EXPECT_TRUE(asset::LoadScene(manifest).scene);
    request.forceRecook             = true;
    asset::SceneLoadResult recooked = asset::LoadSourceScene(request);
    ASSERT_TRUE(recooked.scene) << recooked.error;
    request.forceRecook = false;
    EXPECT_TRUE(asset::LoadSourceScene(request).cacheHit);
}

TEST(SceneAssetCache, EvictsLeastRecentlyUsedFilesBeyondTheBudget)
{
    const AssetFiles  files;
    const std::string cube = files.Path("cube.fbx");
    PutBytes(cube, FileBytes(Fixture("FBX/maya_cube_7500_binary.fbx")));
    asset::SceneImportRequest first{cube};
    first.cacheDirectory = files.Path("cache");
    ASSERT_TRUE(asset::LoadSourceScene(first).scene);
    ASSERT_TRUE(asset::WaitForSceneAssetCacheWrites().empty());
    const std::string                     older = OnlyCache(files);
    const std::filesystem::file_time_type stale = std::filesystem::file_time_type::clock::now() - std::chrono::hours(2);
    std::filesystem::last_write_time(std::filesystem::u8path(older), stale);
    const std::string abandoned = files.Path("cache/stale.zenasset.tmp-1-1");
    PutBytes(abandoned, "left by a crashed writer");
    std::filesystem::last_write_time(std::filesystem::u8path(abandoned), stale);
    const std::string processorAbandoned = files.Path("cache/stale.zenasset.tmp.1.1");
    PutBytes(processorAbandoned, "left by the processor writer");
    std::filesystem::last_write_time(std::filesystem::u8path(processorAbandoned), stale);
    asset::SceneImportRequest second{Fixture("complete_scene.glb")};
    second.cacheDirectory   = first.cacheDirectory;
    second.cacheBudgetBytes = 1;
    ASSERT_TRUE(asset::LoadSourceScene(second).scene);
    ASSERT_TRUE(asset::WaitForSceneAssetCacheWrites().empty());
    EXPECT_FALSE(std::filesystem::exists(std::filesystem::u8path(older)));
    EXPECT_FALSE(std::filesystem::exists(std::filesystem::u8path(abandoned)));
    EXPECT_FALSE(std::filesystem::exists(std::filesystem::u8path(processorAbandoned)));
    EXPECT_FALSE(OnlyCache(files).empty());
    EXPECT_TRUE(asset::LoadSourceScene(second).cacheHit);
}

TEST(SceneAssetCache, InvalidatesSourceSettingsAndCorruptPayloads)
{
    const AssetFiles  files;
    const std::string source = files.Path("source.fbx");
    PutBytes(source, FileBytes(Fixture("FBX/maya_cube_7500_binary.fbx")));
    asset::SceneImportRequest request{source};
    request.useCache             = true;
    request.cacheDirectory       = files.Path("cache");
    asset::SceneLoadResult first = asset::LoadSourceScene(request);
    ASSERT_TRUE(first.scene) << first.error;
    EXPECT_FALSE(first.cacheHit);
    ASSERT_TRUE(asset::WaitForSceneAssetCacheWrites().empty());
    asset::SceneLoadResult cached = asset::LoadSourceScene(request);
    ASSERT_TRUE(cached.scene) << cached.error;
    EXPECT_TRUE(cached.cacheHit);
    EXPECT_EQ(first.scene->vertices[0].pos, cached.scene->vertices[0].pos);
    const std::string cache  = OnlyCache(files);
    std::string       bytes  = FileBytes(cache);
    bytes.back()            ^= 0x7f;
    PutBytes(cache, bytes);
    EXPECT_FALSE(asset::ReadSceneAssetFile(cache).asset);
    asset::SceneLoadResult rebuilt = asset::LoadSourceScene(request);
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
        asset::SceneLoadResult version = asset::LoadSourceScene(request);
        ASSERT_TRUE(version.scene) << version.error;
        EXPECT_FALSE(version.cacheHit);
        ASSERT_TRUE(asset::WaitForSceneAssetCacheWrites().empty());
    }
    const std::filesystem::file_time_type timestamp = std::filesystem::last_write_time(std::filesystem::u8path(source));
    PutBytes(source, FileBytes(Fixture("FBX/maya_cube_7500_ascii.fbx")));
    std::filesystem::last_write_time(std::filesystem::u8path(source), timestamp);
    asset::SceneLoadResult changed = asset::LoadSourceScene(request);
    ASSERT_TRUE(changed.scene) << changed.error;
    EXPECT_FALSE(changed.cacheHit);
    ASSERT_TRUE(asset::WaitForSceneAssetCacheWrites().empty());
    EXPECT_TRUE(asset::LoadSourceScene(request).cacheHit);
    request.settings.fallbackMissingNamedUVSets = false;
    asset::SceneLoadResult settings             = asset::LoadSourceScene(request);
    ASSERT_TRUE(settings.scene) << settings.error;
    EXPECT_FALSE(settings.cacheHit);
    ASSERT_TRUE(asset::WaitForSceneAssetCacheWrites().empty());
    EXPECT_TRUE(asset::LoadSourceScene(request).cacheHit);
    request.useCache = false;
    EXPECT_FALSE(asset::LoadSourceScene(request).cacheHit);
}

TEST(SceneAssetCache, ParallelRequestsPublishCompleteFilesAndFailuresStillLoadSources)
{
    const AssetFiles          files;
    asset::SourceSceneLoader  loader(4, 4);
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
    EXPECT_TRUE(asset::LoadSourceScene(request).cacheHit);
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
    asset::SceneLoadResult unwritable = asset::LoadSourceScene(request);
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
    asset::SceneLoadResult first = asset::LoadSourceScene(request);
    ASSERT_TRUE(first.scene) << first.error;
    EXPECT_FALSE(first.cacheHit);
    ASSERT_EQ(first.asset->dependencies.size(), 2u);
    ASSERT_TRUE(asset::WaitForSceneAssetCacheWrites().empty());
    EXPECT_TRUE(asset::LoadSourceScene(request).cacheHit);
    const std::filesystem::file_time_type textureTime = std::filesystem::last_write_time(std::filesystem::u8path(texture));
    tga[18]                                           = 255;
    tga[20]                                           = 0;
    PutBytes(texture, std::string(reinterpret_cast<const char*>(tga), sizeof(tga)));
    std::filesystem::last_write_time(std::filesystem::u8path(texture), textureTime);
    asset::SceneLoadResult blue = asset::LoadSourceScene(request);
    ASSERT_TRUE(blue.scene) << blue.error;
    EXPECT_FALSE(blue.cacheHit);
    EXPECT_EQ(blue.asset->images[0].pixels->bytes[0], 0u);
    EXPECT_EQ(blue.asset->images[0].pixels->bytes[2], 255u);
    ASSERT_TRUE(asset::WaitForSceneAssetCacheWrites().empty());
    EXPECT_TRUE(asset::LoadSourceScene(request).cacheHit);
    const std::filesystem::file_time_type bufferTime = std::filesystem::last_write_time(std::filesystem::u8path(buffer));
    geometry[3]                                      = 0.5f;
    PutBytes(buffer, std::string(reinterpret_cast<const char*>(geometry), sizeof(geometry)));
    std::filesystem::last_write_time(std::filesystem::u8path(buffer), bufferTime);
    asset::SceneLoadResult smaller = asset::LoadSourceScene(request);
    ASSERT_TRUE(smaller.scene) << smaller.error;
    EXPECT_FALSE(smaller.cacheHit);
    EXPECT_EQ(smaller.scene->vertices[1].pos.x, 0.5f);
    ASSERT_TRUE(asset::WaitForSceneAssetCacheWrites().empty());
    EXPECT_TRUE(asset::LoadSourceScene(request).cacheHit);
    const std::string cache = OnlyCache(files);
    std::error_code   error;
    ASSERT_TRUE(std::filesystem::remove(std::filesystem::u8path(source), error));
    ASSERT_TRUE(std::filesystem::remove(std::filesystem::u8path(texture), error));
    ASSERT_TRUE(std::filesystem::remove(std::filesystem::u8path(buffer), error));
    EXPECT_FALSE(asset::LoadSourceScene(request).scene);
    asset::SceneLoadResult standalone = asset::LoadSourceScene(cache);
    ASSERT_TRUE(standalone.scene) << standalone.error;
    ASSERT_TRUE(standalone.asset);
    SharedPtr<const asset::ImagePixels, MultiThreadCounter> resolved;
    ASSERT_TRUE(asset::ResolveImagePixels(standalone.asset->images[0].pixels, resolved));
    ASSERT_GE(asset::GetImageBaseBytes(*resolved).size(), 3u);
    EXPECT_EQ(asset::GetImageBaseBytes(*resolved)[2], 255u);
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
        std::this_thread::yield();
    }
    const asset::SceneImportReport result = pending.get();
    ASSERT_TRUE(interrupted) << result.error;
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
    EXPECT_EQ(FileBytes(files.Path("saved.zenasset")), before);
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(files.root))
    {
        EXPECT_EQ(entry.path().filename().string().find(".tmp-"), std::string::npos);
    }
}

TEST(SceneAssetIO, SourceDocumentReadLimitAppliesToLegacyAndProcessedPackages)
{
    const AssetFiles files;
    zenasset::Scene  documented;
    documented.sourceDocument.assign(4096, ' ');
    const asset::SceneImportResult adapted = asset::AdaptProcessedScene(documented);
    ASSERT_TRUE(adapted.asset) << adapted.error;
    const std::string packages[]{files.Path("documented-v2.zenasset"), files.Path("documented-v3.zenasset")};
    ASSERT_EQ(asset::WriteSceneAssetFile(packages[0], *adapted.asset).status, asset::SceneImportStatus::Success);
    const zenasset::Status written = zenasset::WriteScene(packages[1], documented);
    ASSERT_TRUE(written.Succeeded()) << written.message;
    for (const std::string& package : packages)
    {
        SCOPED_TRACE(package);
        asset::SceneAssetReadLimits limits;
        limits.maxStringBytes                      = documented.sourceDocument.size() - 1;
        const asset::SceneAssetFileResult rejected = asset::ReadSceneAssetFile(package, limits);
        EXPECT_EQ(rejected.status, asset::SceneImportStatus::ResourceLimit) << rejected.error;
        EXPECT_FALSE(rejected.asset);
        limits.maxStringBytes                      = documented.sourceDocument.size();
        const asset::SceneAssetFileResult accepted = asset::ReadSceneAssetFile(package, limits);
        ASSERT_EQ(accepted.status, asset::SceneImportStatus::Success) << accepted.error;
        ASSERT_TRUE(accepted.asset);
        EXPECT_EQ(accepted.asset->sourceDocument, documented.sourceDocument);
        // Cache metadata inspection does not decode the source document.
        const asset::SceneAssetFileResult metadata = asset::ReadSceneAssetMetadata(package);
        EXPECT_EQ(metadata.status, asset::SceneImportStatus::Success) << metadata.error;
        EXPECT_FALSE(metadata.asset);
    }
}

TEST(SceneAssetIO, ReadsProcessorPackagesAfterRemovingSources)
{
    EXPECT_EQ(asset::ReadSceneAssetFile(std::string(1, char(0xff))).status, asset::SceneImportStatus::InvalidRequest);
    EXPECT_EQ(asset::ReadSceneAssetMetadata(std::string("bad\0path", 8)).status, asset::SceneImportStatus::InvalidRequest);
    const AssetFiles  files;
    const std::string source = files.Path("source.glb");
    std::error_code   error;
    ASSERT_TRUE(std::filesystem::copy_file(Fixture("complete_scene.glb"), source, error));
    const zenasset::SceneImportResult imported = zenasset::ImportScene({source});
    ASSERT_TRUE(imported.asset) << imported.error;
    zenasset::SceneWriteOptions options;
    options.metadata.sourceFormat                        = imported.format;
    options.metadata.importerVersion                     = imported.importerVersion;
    options.metadata.buildFingerprint                    = "processor-package-test";
    options.metadata.settings.fallbackMissingNamedUVSets = false;
    options.metadata.settings.animationBake.sampleRate   = 60;
    options.metadata.inputs.push_back({source, true, 123, 456, 789});
    options.metadata.diagnostics.push_back({zenasset::DiagnosticSeverity::Warning, zenasset::StatusCode::UnsupportedFeature,
                                            source, "test-object", "retained package diagnostic"});
    const std::string      package = files.Path("processed.zenasset");
    const zenasset::Status written = zenasset::WriteScene(package, *imported.asset, options);
    ASSERT_TRUE(written.Succeeded()) << written.message;
    ASSERT_TRUE(std::filesystem::remove(source, error));
    const asset::SceneAssetFileResult metadata = asset::ReadSceneAssetMetadata(package);
    ASSERT_EQ(metadata.status, asset::SceneImportStatus::Success) << metadata.error;
    EXPECT_FALSE(metadata.asset);
    EXPECT_EQ(metadata.importerVersion, zenasset::PackageSchemaVersion);
    EXPECT_EQ(metadata.metadata.profile, "reference");
    EXPECT_EQ(metadata.metadata.buildFingerprint, options.metadata.buildFingerprint);
    ASSERT_EQ(metadata.metadata.inputs.size(), 1u);
    EXPECT_EQ(metadata.metadata.inputs[0].contentHash, 789u);
    EXPECT_FALSE(metadata.metadata.settings.fallbackMissingNamedUVSets);
    EXPECT_EQ(metadata.metadata.settings.animationBake.sampleRate, 60u);
    asset::SceneLoadResult loaded = asset::LoadScene(package);
    ASSERT_TRUE(loaded.scene) << loaded.error;
    EXPECT_EQ(loaded.asset->sourceDocument, imported.asset->sourceDocument);
    EXPECT_EQ(loaded.asset->dependencies.size(), imported.asset->dependencies.size());
    ASSERT_EQ(loaded.diagnostics.size(), 1u);
    EXPECT_EQ(loaded.diagnostics[0].message, options.metadata.diagnostics[0].message);
    EXPECT_EQ(loaded.diagnostics[0].status, asset::SceneImportStatus::UnsupportedFeature);
    asset::SceneImportResult adapted = asset::AdaptProcessedScene(*imported.asset);
    ASSERT_TRUE(adapted.asset) << adapted.error;
    asset::SceneLoadResult expected = asset::BuildSceneRuntime(adapted.asset);
    ASSERT_TRUE(expected.scene) << expected.error;
    ComparePose(*loaded.scene, *expected.scene, 0, 0.5f);
    asset::SceneAssetReadLimits limited;
    limited.maxAllocationBytes = 1;
    EXPECT_EQ(asset::ReadSceneAssetFile(package, limited).status, asset::SceneImportStatus::ResourceLimit);
    asset::SceneImportControl cancelled;
    cancelled.Cancel();
    EXPECT_EQ(asset::ReadSceneAssetFile(package, {}, &cancelled).status, asset::SceneImportStatus::Cancelled);
}

TEST(SceneAssetCache, PublishesProcessorV3AndRetainsImportDependencySemantics)
{
    const AssetFiles          files;
    asset::SceneImportRequest request{Fixture("texture_roles.gltf")};
    request.cacheDirectory                = (files.root / "cache").string();
    const asset::SceneLoadResult imported = asset::LoadSourceScene(request);
    ASSERT_TRUE(imported.asset) << imported.error;
    ASSERT_TRUE(asset::WaitForSceneAssetCacheWrites().empty());
    const std::string package = OnlyCache(files);
    const std::string bytes   = FileBytes(package);
    ASSERT_GT(bytes.size(), 12u);
    EXPECT_EQ(static_cast<uint8_t>(bytes[8]), zenasset::PackageSchemaVersion);
    const zenasset::SceneMetadataResult metadata = zenasset::ReadSceneMetadata(package);
    ASSERT_TRUE(metadata.status.Succeeded()) << metadata.status.message;
    EXPECT_EQ(metadata.metadata.compression.transport, zenasset::TransportCodec::Zstandard);
    EXPECT_EQ(metadata.metadata.compression.zstandardLevel, 3);
    EXPECT_TRUE(metadata.metadata.compression.meshoptimizer);
    {
        const zenasset::PackageResult opened = zenasset::OpenPackage(package);
        ASSERT_TRUE(opened.status.Succeeded()) << opened.status.message;
        uint64_t storedBytes  = 0;
        uint64_t decodedBytes = 0;
        bool     compressed   = false;
        for (const zenasset::ResourceRecord& resource : opened.package->Resources())
        {
            storedBytes  += resource.storedBytes;
            decodedBytes += resource.decodedBytes;
            compressed   |= resource.codec == zenasset::TransportCodec::Zstandard;
        }
        EXPECT_TRUE(compressed);
        EXPECT_LT(storedBytes, decodedBytes);
    }
    ASSERT_EQ(metadata.metadata.inputs.size(), imported.asset->dependencies.size() + 1);
    EXPECT_EQ(metadata.dependencies.size(), imported.asset->dependencies.size());
    EXPECT_NE(metadata.metadata.inputs[0].contentHash, 0u);
    const asset::SceneLoadResult cached = asset::LoadSourceScene(request);
    ASSERT_TRUE(cached.scene) << cached.error;
    EXPECT_TRUE(cached.cacheHit);
    EXPECT_EQ(cached.asset->sourceDocument, imported.asset->sourceDocument);
    EXPECT_EQ(cached.asset->dependencies.size(), imported.asset->dependencies.size());
    CompareSceneRecords(files, *cached.asset, *imported.asset);
}

TEST(SceneAssetCache, CookedSourceMatchesExplicitPackageAndCachedScene)
{
    const AssetFiles  generated;
    const std::string reordered = ReorderedSource(generated);
    for (const std::string& source :
         {reordered, Fixture("complete_scene.glb"), Fixture("all_primitive_modes.gltf"), Fixture("vertex_attributes.gltf"),
          Fixture("FBX/combined_morph_skin_ascii.fbx"), Fixture("texture_roles.gltf")})
    {
        SCOPED_TRACE(source);
        const AssetFiles          files;
        asset::SceneImportRequest request{source};
        request.profile                                 = "desktop";
        request.processing.geometry.optimizeVertexFetch = true;
        request.processing.textures.compressBC7         = source == Fixture("texture_roles.gltf");
        request.processing.textures.generateMipmaps     = request.processing.textures.compressBC7;
        request.cacheDirectory                          = files.Path("cache");
        const zenasset::SceneImportResult normalized    = zenasset::ImportScene({source});
        ASSERT_TRUE(normalized.asset) << normalized.error;
        zenasset::SceneCookOptions cookOptions;
        cookOptions.settings                   = request.processing;
        const zenasset::SceneCookResult cooked = zenasset::CookScene(normalized.asset, cookOptions);
        ASSERT_TRUE(cooked.scene) << cooked.status.message;
        const asset::SceneImportResult adapted = asset::AdaptProcessedScene(*cooked.scene);
        ASSERT_TRUE(adapted.asset) << adapted.error;
        const asset::SceneImportResult direct = asset::ImportScene(request);
        ASSERT_TRUE(direct.asset) << direct.error;
        CompareSceneRecords(files, *direct.asset, *adapted.asset);
        if (request.processing.textures.compressBC7)
        {
            uint32_t encodedImages = 0;
            for (const asset::ImageAsset& image : direct.asset->images)
            {
                ASSERT_TRUE(image.pixels);
                if (image.pixels->format == asset::Format::BC7_UNORM_BLOCK
                    || image.pixels->format == asset::Format::BC7_SRGB_BLOCK)
                {
                    ++encodedImages;
                    EXPECT_TRUE(image.pixels->bytes.empty());
                    EXPECT_EQ(asset::GetImageBaseBytes(*image.pixels).size(), 16u);
                    ASSERT_EQ(image.pixels->mips.size(), 1u);
                    EXPECT_EQ(image.pixels->mips.front().size(), 16u);
                }
            }
            EXPECT_GT(encodedImages, 0u);
            EXPECT_EQ(normalized.asset->images.front().pixels->format, zenasset::PixelFormat::RGBA8Unorm);
            EXPECT_EQ(normalized.asset->images.front().pixels->bytes.size(), 4u);
        }
        if (source == reordered)
        {
            ASSERT_EQ(normalized.asset->indices.front(), 2u);
            EXPECT_EQ(direct.asset->indices.front(), 0u);
            EXPECT_EQ(direct.asset->vertices.front().pos.x, 0.0f);
            EXPECT_EQ(direct.asset->vertices.front().pos.y, 1.0f);
        }
        zenasset::SceneWriteOptions writeOptions;
        writeOptions.metadata.profile    = request.profile;
        writeOptions.metadata.processing = request.processing;
        const std::string package        = files.Path("explicit.zenasset");
        ASSERT_TRUE(zenasset::WriteScene(package, *cooked.scene, writeOptions).Succeeded());
        const asset::SceneAssetFileResult packaged = asset::ReadSceneAssetFile(package);
        ASSERT_TRUE(packaged.asset) << packaged.error;
        CompareSceneRecords(files, *direct.asset, *packaged.asset);
        EXPECT_EQ(packaged.metadata.processing, request.processing);
        const asset::SceneLoadResult miss = asset::LoadSourceScene(request);
        ASSERT_TRUE(miss.scene) << miss.error;
        EXPECT_FALSE(miss.cacheHit);
        CompareSceneRecords(files, *direct.asset, *miss.asset);
        ASSERT_TRUE(asset::WaitForSceneAssetCacheWrites().empty());
        const asset::SceneLoadResult hit = asset::LoadSourceScene(request);
        ASSERT_TRUE(hit.scene) << hit.error;
        EXPECT_TRUE(hit.cacheHit);
        CompareSceneRecords(files, *direct.asset, *hit.asset);
        const zenasset::SceneMetadataResult metadata = zenasset::ReadSceneMetadata(OnlyCache(files));
        ASSERT_TRUE(metadata.status.Succeeded()) << metadata.status.message;
        EXPECT_EQ(metadata.metadata.profile, request.profile);
        EXPECT_EQ(metadata.metadata.processing, request.processing);
    }
}

TEST(SceneAssetCache, Bc6EnvironmentSourceCookPackageAndCacheAgree)
{
    const AssetFiles  files;
    const std::string source      = ReorderedSource(files);
    std::string       document    = FileBytes(source);
    const std::string sceneField  = "\"scenes\":[{\"nodes\":[0]}]";
    const size_t      sceneOffset = document.find(sceneField);
    ASSERT_NE(sceneOffset, std::string::npos);
    document.replace(sceneOffset, sceneField.size(),
                     R"("scenes":[{"nodes":[0],"extensions":{"EXT_lights_image_based":{"light":0}}}])");
    const size_t end = document.rfind('}');
    ASSERT_NE(end, std::string::npos);
    document.insert(end, R"(,"extensionsUsed":["EXT_lights_image_based"],
"images":[{"uri":"data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+jR1cAAAAASUVORK5CYII="}],
"extensions":{"EXT_lights_image_based":{"lights":[{"name":"authored cube","intensity":2.5,
"irradianceCoefficients":[[1,0.5,0.25],[0,0,0],[0,0,0],[0,0,0],[0,0,0],[0,0,0],[0,0,0],[0,0,0],[0,0,0]],
"specularImageSize":1,"specularImages":[[0,0,0,0,0,0]]}]}})");
    PutBytes(source, document);
    asset::SceneImportRequest request{source};
    request.profile                                     = "desktop";
    request.processing.textures.compressBC6HEnvironment = true;
    request.cacheDirectory                              = files.Path("cache");
    const zenasset::SceneImportResult normalized        = zenasset::ImportScene({source});
    ASSERT_TRUE(normalized.asset) << normalized.error;
    ASSERT_EQ(normalized.asset->imageBasedLights.size(), 1u);
    zenasset::SceneCookOptions options;
    options.settings                       = request.processing;
    const zenasset::SceneCookResult cooked = zenasset::CookScene(normalized.asset, options);
    ASSERT_TRUE(cooked.scene) << cooked.status.message;
    ASSERT_EQ(cooked.scene->imageBasedLights[0].specular->format, zenasset::PixelFormat::BC6HUfloat);
    const asset::SceneImportResult expected = asset::AdaptProcessedScene(*cooked.scene);
    ASSERT_TRUE(expected.asset) << expected.error;
    const asset::SceneImportResult direct = asset::ImportScene(request);
    ASSERT_TRUE(direct.asset) << direct.error;
    CompareCompressedEnvironmentScene(files, *direct.asset, *expected.asset);
    EXPECT_EQ(normalized.asset->imageBasedLights[0].specular->format, zenasset::PixelFormat::RGBA32Float);
    EXPECT_EQ(normalized.asset->imageBasedLights[0].specular->rgba32FloatMipFaces.size(), 6u);
    zenasset::SceneWriteOptions write;
    write.metadata.profile      = request.profile;
    write.metadata.processing   = request.processing;
    write.compression.transport = zenasset::TransportCodec::Zstandard;
    const std::string package   = files.Path("environment.zenasset");
    ASSERT_TRUE(zenasset::WriteScene(package, *cooked.scene, write).Succeeded());
    const asset::SceneAssetFileResult explicitPackage = asset::ReadSceneAssetFile(package);
    ASSERT_TRUE(explicitPackage.asset) << explicitPackage.error;
    CompareCompressedEnvironmentScene(files, *direct.asset, *explicitPackage.asset);
    EXPECT_EQ(explicitPackage.metadata.processing, request.processing);
    const asset::SceneLoadResult miss = asset::LoadSourceScene(request);
    ASSERT_TRUE(miss.scene) << miss.error;
    EXPECT_FALSE(miss.cacheHit);
    CompareCompressedEnvironmentScene(files, *direct.asset, *miss.asset);
    ASSERT_TRUE(asset::WaitForSceneAssetCacheWrites().empty());
    const asset::SceneLoadResult hit = asset::LoadSourceScene(request);
    ASSERT_TRUE(hit.scene) << hit.error;
    EXPECT_TRUE(hit.cacheHit);
    CompareCompressedEnvironmentScene(files, *direct.asset, *hit.asset);
    const zenasset::SceneMetadataResult metadata = zenasset::ReadSceneMetadata(OnlyCache(files));
    ASSERT_TRUE(metadata.status.Succeeded()) << metadata.status.message;
    EXPECT_EQ(metadata.metadata.processing, request.processing);
    std::error_code error;
    ASSERT_TRUE(std::filesystem::remove(source, error));
    ASSERT_FALSE(error);
    const asset::SceneAssetFileResult withoutSource = asset::ReadSceneAssetFile(package);
    ASSERT_TRUE(withoutSource.asset) << withoutSource.error;
    CompareCompressedEnvironmentScene(files, *direct.asset, *withoutSource.asset);
}

TEST(SceneAssetCache, ProcessingIdentityInvalidatesV3AndPreservesLegacyV2)
{
    const AssetFiles          files;
    const std::string         source = ReorderedSource(files);
    asset::SceneImportRequest request{source};
    request.cacheDirectory                       = files.Path("cache");
    const zenasset::SceneImportResult normalized = zenasset::ImportScene({source});
    ASSERT_TRUE(normalized.asset) << normalized.error;
    const asset::SceneImportResult adapted = asset::AdaptProcessedScene(*normalized.asset);
    ASSERT_TRUE(adapted.asset) << adapted.error;
    asset::SceneAssetFileMetadata reference;
    const std::string             legacyKey    = asset::SceneAssetCacheKey(reference);
    const std::string             referenceKey = asset::SceneAssetCacheKeyForVersion(reference, zenasset::PackageSchemaVersion);
    const std::string             legacy       = files.Path("legacy.zenasset");
    ASSERT_EQ(asset::WriteSceneAssetFile(legacy, *adapted.asset, reference).status, asset::SceneImportStatus::Success);
    const std::string       legacyBytes = FileBytes(legacy);
    HeapVector<std::string> keys;
    for (uint32_t variant = 0; variant < 6; ++variant)
    {
        SCOPED_TRACE(variant);
        request.profile                        = variant == 0 ? "reference" : "desktop";
        request.processing                     = {};
        zenasset::GeometrySettings& geometry   = request.processing.geometry;
        geometry.optimizeVertexFetch           = variant >= 2;
        geometry.positionQuantizationBits      = variant >= 3 ? 16 : 0;
        geometry.positionMaxRelativeError      = variant >= 4 ? 0.001f : 0.00001f;
        geometry.positionMaxAbsoluteError      = variant >= 5 ? 0.001f : 0.000001f;
        asset::SceneAssetFileMetadata identity = reference;
        identity.profile                       = request.profile;
        identity.processing                    = request.processing;
        const std::string key                  = asset::SceneAssetCacheKeyForVersion(identity, zenasset::PackageSchemaVersion);
        EXPECT_EQ(asset::SceneAssetCacheKey(identity), legacyKey);
        EXPECT_EQ(key == referenceKey, variant == 0);
        EXPECT_EQ(std::find(keys.begin(), keys.end(), key), keys.end());
        keys.push_back(key);
        ASSERT_EQ(asset::WriteSceneAssetFile(legacy, *adapted.asset, identity).status, asset::SceneImportStatus::Success);
        EXPECT_EQ(FileBytes(legacy), legacyBytes);
        const asset::SceneAssetFileResult legacyRead = asset::ReadSceneAssetFile(legacy);
        ASSERT_TRUE(legacyRead.asset) << legacyRead.error;
        EXPECT_EQ(legacyRead.metadata.profile, "reference");
        EXPECT_EQ(legacyRead.metadata.processing, zenasset::ProcessingSettings{});
        const asset::SceneLoadResult miss = asset::LoadSourceScene(request);
        ASSERT_TRUE(miss.asset) << miss.error;
        EXPECT_FALSE(miss.cacheHit);
        if (variant == 0)
        {
            CompareSceneRecords(files, *miss.asset, *adapted.asset);
        }
        ASSERT_TRUE(asset::WaitForSceneAssetCacheWrites().empty());
        const asset::SceneLoadResult hit = asset::LoadSourceScene(request);
        ASSERT_TRUE(hit.asset) << hit.error;
        EXPECT_TRUE(hit.cacheHit);
        CompareSceneRecords(files, *miss.asset, *hit.asset);
    }
}

TEST(SceneAssetCache, TexturePolicyParticipatesInV3IdentityAndMetadata)
{
    asset::SceneAssetFileMetadata reference;
    const std::string             legacy = asset::SceneAssetCacheKey(reference);
    HeapVector<std::string>       keys;
    keys.push_back(asset::SceneAssetCacheKeyForVersion(reference, zenasset::PackageSchemaVersion));
    for (uint32_t field = 0; field < 14; ++field)
    {
        SCOPED_TRACE(field);
        asset::SceneAssetFileMetadata metadata = reference;
        zenasset::TextureSettings&    textures = metadata.processing.textures;
        switch (field)
        {
            case 0: textures.generateMipmaps = true; break;
            case 1: textures.compressBC7 = true; break;
            case 2: textures.bc7Quality = 3; break;
            case 3: textures.maxDimension = 8192; break;
            case 4: textures.colorMinPsnr = 41.0f; break;
            case 5: textures.colorMaxRmse = 0.02f; break;
            case 6: textures.scalarMaxError = 0.04f; break;
            case 7: textures.normalRmsAngleDegrees = 2.0f; break;
            case 8: textures.normalMaxAngleDegrees = 6.0f; break;
            case 9: textures.alphaCoverageMaxError = 0.02f; break;
            case 10: textures.compressBC5Normals = true; break;
            case 11: textures.compressBC4Occlusion = true; break;
            case 12: textures.compressBC6HEnvironment = true; break;
            case 13: textures.hdrMaxRelativeRmse = 0.01f; break;
        }
        const std::string key = asset::SceneAssetCacheKeyForVersion(metadata, zenasset::PackageSchemaVersion);
        EXPECT_EQ(std::find(keys.begin(), keys.end(), key), keys.end());
        keys.push_back(key);
        EXPECT_EQ(asset::SceneAssetCacheKey(metadata), legacy);
        const zenasset::BuildMetadata converted = asset::MakeProcessedMetadata(metadata);
        EXPECT_EQ(converted.processing, metadata.processing);
        EXPECT_EQ(asset::AdaptProcessedMetadata(converted).processing, metadata.processing);
    }
}

TEST(SceneAssetCache, RejectedAndCancelledProcessingDoesNotPublish)
{
    const AssetFiles          files;
    asset::SceneImportRequest request{ReorderedSource(files)};
    request.cacheDirectory = files.Path("cache");
    ASSERT_TRUE(asset::LoadSourceScene(request).asset);
    ASSERT_TRUE(asset::WaitForSceneAssetCacheWrites().empty());
    const std::string cache    = OnlyCache(files);
    const std::string previous = FileBytes(cache);
    for (uint32_t variant = 0; variant < 10; ++variant)
    {
        SCOPED_TRACE(variant);
        asset::SceneImportRequest invalid = request;
        invalid.control = SharedPtr<asset::SceneImportControl, MultiThreadCounter>(new asset::SceneImportControl);
        if (variant == 0)
        {
            invalid.profile = "unknown";
        }
        else if (variant == 1)
        {
            invalid.processing.geometry.optimizeVertexFetch = true;
        }
        else if (variant == 2)
        {
            invalid.profile                                      = "desktop";
            invalid.processing.geometry.positionQuantizationBits = 25;
        }
        else if (variant == 3)
        {
            invalid.processing.geometry.positionMaxRelativeError = std::numeric_limits<float>::quiet_NaN();
        }
        else if (variant == 4)
        {
            invalid.processing.textures.generateMipmaps = true;
        }
        else if (variant == 5)
        {
            invalid.processing.textures.compressBC7 = true;
        }
        else if (variant == 6)
        {
            invalid.processing.textures.compressBC5Normals = true;
        }
        else if (variant == 7)
        {
            invalid.processing.textures.compressBC4Occlusion = true;
        }
        else if (variant == 8)
        {
            invalid.processing.textures.compressBC6HEnvironment = true;
        }
        else
        {
            invalid.control->Cancel();
        }
        const asset::SceneImportStatus expected =
            variant == 9 ? asset::SceneImportStatus::Cancelled : asset::SceneImportStatus::InvalidRequest;
        const asset::SceneImportResult direct = asset::ImportScene(invalid);
        EXPECT_FALSE(direct.asset);
        EXPECT_EQ(direct.status, expected) << direct.error;
        const asset::SceneLoadResult cached = asset::LoadSourceScene(invalid);
        EXPECT_FALSE(cached.asset);
        EXPECT_FALSE(cached.scene);
        EXPECT_FALSE(cached.cacheHit);
        EXPECT_EQ(cached.status, expected) << cached.error;
        EXPECT_EQ(invalid.control->GetStage(),
                  variant == 9 ? asset::SceneImportStage::Cancelled : asset::SceneImportStage::Failed);
        ASSERT_TRUE(asset::WaitForSceneAssetCacheWrites().empty());
        EXPECT_EQ(OnlyCache(files), cache);
        EXPECT_EQ(FileBytes(cache), previous);
    }
    request.path = files.Path("broken.gltf");
    PutBytes(request.path, "not a glTF document");
    const asset::SceneLoadResult failed = asset::LoadSourceScene(request);
    EXPECT_FALSE(failed.asset);
    EXPECT_FALSE(failed.scene);
    EXPECT_FALSE(failed.cacheHit);
    EXPECT_NE(failed.status, asset::SceneImportStatus::Success);
    ASSERT_TRUE(asset::WaitForSceneAssetCacheWrites().empty());
    EXPECT_EQ(OnlyCache(files), cache);
    EXPECT_EQ(FileBytes(cache), previous);
}

TEST(SceneAssetCache, CookingDiagnosticsSurvivePublication)
{
    const AssetFiles          files;
    asset::SceneImportRequest request{Fixture("FBX/combined_morph_skin_ascii.fbx")};
    request.profile                                      = "desktop";
    request.processing.geometry.positionQuantizationBits = 12;
    request.cacheDirectory                               = files.Path("cache");
    const asset::SceneLoadResult imported                = asset::LoadSourceScene(request);
    ASSERT_TRUE(imported.asset) << imported.error;
    ASSERT_FALSE(imported.diagnostics.empty());
    bool cookingWarning = false;
    for (const asset::SceneImportDiagnostic& diagnostic : imported.diagnostics)
    {
        cookingWarning |= diagnostic.message.find("quantization") != std::string::npos;
    }
    EXPECT_TRUE(cookingWarning);
    ASSERT_TRUE(asset::WaitForSceneAssetCacheWrites().empty());
    const asset::SceneLoadResult cached = asset::LoadSourceScene(request);
    ASSERT_TRUE(cached.asset) << cached.error;
    EXPECT_TRUE(cached.cacheHit);
    ASSERT_EQ(cached.diagnostics.size(), imported.diagnostics.size());
    for (size_t index = 0; index < imported.diagnostics.size(); ++index)
    {
        EXPECT_EQ(cached.diagnostics[index].message, imported.diagnostics[index].message);
        EXPECT_EQ(cached.diagnostics[index].object, imported.diagnostics[index].object);
    }
}

TEST(SceneAssetCache, ForcedRecookBypassesHitsAndWaitsForItsOwnPublication)
{
    const AssetFiles          files;
    asset::SceneImportRequest request{ReorderedSource(files)};
    request.cacheDirectory                          = files.Path("cache");
    request.profile                                 = "desktop";
    request.processing.geometry.optimizeVertexFetch = true;
    ASSERT_TRUE(asset::LoadSourceScene(request).asset);
    ASSERT_TRUE(asset::WaitForSceneAssetCacheWrites().empty());
    const std::string               cache = OnlyCache(files);
    const zenasset::SceneReadResult saved = zenasset::ReadScene(cache);
    ASSERT_TRUE(saved.scene) << saved.status.message;
    zenasset::Scene altered = *saved.scene;
    altered.name            = "valid cache marker";
    zenasset::SceneWriteOptions write;
    write.metadata = saved.metadata;
    ASSERT_TRUE(zenasset::WriteScene(cache, altered, write).Succeeded());
    const asset::SceneLoadResult hit = asset::LoadSourceScene(request);
    ASSERT_TRUE(hit.asset) << hit.error;
    ASSERT_TRUE(hit.cacheHit);
    EXPECT_EQ(hit.asset->name, altered.name);
    request.forceRecook                   = true;
    const asset::SceneLoadResult recooked = asset::LoadSourceScene(request);
    ASSERT_TRUE(recooked.asset) << recooked.error;
    EXPECT_FALSE(recooked.cacheHit);
    EXPECT_EQ(recooked.asset->name, saved.scene->name);
    // No global drain: successful recook already published its own result.
    const zenasset::SceneReadResult refreshed = zenasset::ReadScene(cache);
    ASSERT_TRUE(refreshed.scene) << refreshed.status.message;
    EXPECT_EQ(refreshed.scene->name, saved.scene->name);
    EXPECT_EQ(refreshed.metadata.cacheKey, saved.metadata.cacheKey);
    EXPECT_EQ(OnlyCache(files), cache);
    request.forceRecook = false;
    EXPECT_TRUE(asset::LoadSourceScene(request).cacheHit);

    const std::string previous = FileBytes(cache);
    request.forceRecook        = true;
    request.cacheDirectory     = files.Path("blocked");
    PutBytes(request.cacheDirectory, "regular file, not a cache directory");
    const asset::SceneLoadResult failed = asset::LoadSourceScene(request);
    EXPECT_FALSE(failed.scene);
    EXPECT_FALSE(failed.asset);
    EXPECT_EQ(failed.status, asset::SceneImportStatus::IOError);
    EXPECT_FALSE(failed.diagnostics.empty());
    EXPECT_EQ(FileBytes(cache), previous);
    // Explicit failures are delivered to that request, not retained globally.
    EXPECT_TRUE(asset::WaitForSceneAssetCacheWrites().empty());
    request.path = cache;
    EXPECT_EQ(asset::LoadSourceScene(request).status, asset::SceneImportStatus::InvalidRequest);
    request.path     = ReorderedSource(files);
    request.useCache = false;
    EXPECT_EQ(asset::LoadSourceScene(request).status, asset::SceneImportStatus::InvalidRequest);
}

TEST(SceneAssetCache, ForcedRecookCancellationBeforePublicationPreservesPreviousPackage)
{
    const AssetFiles          files;
    asset::SceneImportRequest request{ReorderedSource(files)};
    request.cacheDirectory = files.Path("cache");
    request.forceRecook    = true;
    std::string source     = FileBytes(request.path);
    source.insert(1, "\"extras\":{\"padding\":\"" + std::string(64ull << 20, ' ') + "\"},");
    PutBytes(request.path, source);
    ASSERT_TRUE(asset::LoadSourceScene(request).asset);
    const std::string cache    = OnlyCache(files);
    const std::string previous = FileBytes(cache);
    request.control            = SharedPtr<asset::SceneImportControl, MultiThreadCounter>(new asset::SceneImportControl);
    asset::SceneLoadResult (*load)(const asset::SceneImportRequest&, ThreadPool<void, uint32_t>*) = &asset::LoadSourceScene;
    std::future<asset::SceneLoadResult> pending     = std::async(std::launch::async, load, std::cref(request), nullptr);
    const bool                          interrupted = WaitForCacheTemporaryFile(request.cacheDirectory, pending);
    if (interrupted)
    {
        request.control->Cancel();
    }
    const asset::SceneLoadResult cancelled = pending.get();
    ASSERT_TRUE(interrupted);
    EXPECT_EQ(cancelled.status, asset::SceneImportStatus::Cancelled);
    EXPECT_FALSE(cancelled.scene);
    EXPECT_FALSE(cancelled.asset);
    EXPECT_EQ(FileBytes(cache), previous);
    EXPECT_EQ(OnlyCache(files), cache);
    EXPECT_TRUE(asset::WaitForSceneAssetCacheWrites().empty());
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(request.cacheDirectory))
    {
        EXPECT_EQ(entry.path().filename().string().find(".zenasset.tmp."), std::string::npos);
    }
}

TEST(SceneAssetCache, ForcedRecookRejectsInputChangesDuringTemporaryWrite)
{
    const AssetFiles          files;
    asset::SceneImportRequest request{ReorderedSource(files)};
    request.cacheDirectory = files.Path("cache");
    request.forceRecook    = true;
    std::string source     = FileBytes(request.path);
    source.insert(1, "\"extras\":{\"padding\":\"" + std::string(64ull << 20, ' ') + "\"},");
    PutBytes(request.path, source);
    ASSERT_TRUE(asset::LoadSourceScene(request).asset);
    const std::string                     cache    = OnlyCache(files);
    const std::string                     previous = FileBytes(cache);
    const std::filesystem::file_time_type stamp    = std::filesystem::last_write_time(request.path);
    asset::SceneLoadResult (*load)(const asset::SceneImportRequest&, ThreadPool<void, uint32_t>*) = &asset::LoadSourceScene;
    std::future<asset::SceneLoadResult> pending = std::async(std::launch::async, load, std::cref(request), nullptr);
    // Wait until serialization is under way, then change one harmless JSON-string
    // byte. Preserve size and timestamp so only the final content check detects it.
    const bool writing = WaitForCacheTemporaryFile(request.cacheDirectory, pending);
    if (writing)
    {
        std::fstream changed(request.path, std::ios::binary | std::ios::in | std::ios::out);
        changed.seekp(static_cast<std::streamoff>(source.find("padding\":\"") + 10));
        changed.put('x');
        changed.close();
        std::error_code error;
        std::filesystem::last_write_time(request.path, stamp, error);
        EXPECT_FALSE(error);
    }
    const asset::SceneLoadResult rejected = pending.get();
    ASSERT_TRUE(writing);
    EXPECT_EQ(rejected.status, asset::SceneImportStatus::SourceUnavailable);
    EXPECT_NE(rejected.error.find("Scene input changed"), std::string::npos);
    EXPECT_NE(rejected.error.find(std::filesystem::path(request.path).generic_string()), std::string::npos);
    EXPECT_FALSE(rejected.asset);
    EXPECT_FALSE(rejected.scene);
    EXPECT_EQ(FileBytes(cache), previous);
    EXPECT_TRUE(asset::WaitForSceneAssetCacheWrites().empty());
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(request.cacheDirectory))
    {
        EXPECT_EQ(entry.path().filename().string().find(".zenasset.tmp."), std::string::npos);
    }
}

TEST(SceneAssetCache, RequestCancellationStopsAnInFlightBackgroundPublication)
{
    const AssetFiles          files;
    asset::SceneImportRequest request{ReorderedSource(files)};
    request.cacheDirectory = files.Path("cache");
    request.control        = SharedPtr<asset::SceneImportControl, MultiThreadCounter>(new asset::SceneImportControl);
    {
        // The source document is retained in the package. Large metadata gives
        // this test a cancellable write without inflating runtime geometry.
        std::string source = FileBytes(request.path);
        source.insert(1, "\"extras\":{\"padding\":\"" + std::string(64ull << 20, ' ') + "\"},");
        PutBytes(request.path, source);
    }
    asset::SceneLoadResult (*load)(const asset::SceneImportRequest&, ThreadPool<void, uint32_t>*) = &asset::LoadSourceScene;
    std::future<asset::SceneLoadResult>         pending     = std::async(std::launch::async, load, std::cref(request), nullptr);
    const std::chrono::steady_clock::time_point deadline    = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    bool                                        interrupted = false;
    bool                                        published   = false;
    while (!interrupted && !published && std::chrono::steady_clock::now() < deadline)
    {
        std::error_code                           error;
        std::filesystem::directory_iterator       entry(std::filesystem::u8path(request.cacheDirectory), error);
        const std::filesystem::directory_iterator end;
        while (!error && entry != end)
        {
            const std::filesystem::path path = entry->path();
            if (path.filename().string().find(".zenasset.tmp.") != std::string::npos && WrittenFileBytes(path) >= 0)
            {
                interrupted = true;
                request.control->Cancel();
            }
            published |= path.extension() == ".zenasset";
            entry.increment(error);
        }
        std::this_thread::yield();
    }
    const asset::SceneLoadResult loaded = pending.get();
    ASSERT_TRUE(asset::WaitForSceneAssetCacheWrites().empty());
    ASSERT_TRUE(interrupted) << loaded.error;
    EXPECT_FALSE(published);
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(request.cacheDirectory))
    {
        EXPECT_NE(entry.path().extension(), ".zenasset");
        EXPECT_EQ(entry.path().filename().string().find(".zenasset.tmp."), std::string::npos);
    }
}

#if defined(_WIN32)
TEST(SceneAssetIO, AtomicReplacementPreservesAlreadyOpenReader)
{
    const AssetFiles         files;
    asset::SceneImportResult imported = asset::ImportScene({Fixture("complete_scene.glb")});
    ASSERT_TRUE(imported.asset) << imported.error;
    const std::string path = files.Path("replacement.zenasset");
    ASSERT_EQ(asset::WriteSceneAssetFile(path, *imported.asset).status, asset::SceneImportStatus::Success);
    const std::string original = FileBytes(path);
    const HANDLE reader = CreateFileW(std::filesystem::u8path(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                                      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(reader, INVALID_HANDLE_VALUE);
    asset::SceneAsset replacement          = *imported.asset;
    replacement.name                       = "Replacement scene";
    const asset::SceneImportReport written = asset::WriteSceneAssetFile(path, replacement);
    std::string                    retained(original.size(), '\0');
    DWORD                          transferred = 0;
    const bool read = ReadFile(reader, retained.data(), static_cast<DWORD>(retained.size()), &transferred, nullptr) != 0;
    CloseHandle(reader);
    EXPECT_EQ(written.status, asset::SceneImportStatus::Success) << written.error;
    EXPECT_TRUE(read);
    EXPECT_EQ(transferred, original.size());
    EXPECT_EQ(retained, original);
    const asset::SceneAssetFileResult published = asset::ReadSceneAssetFile(path);
    ASSERT_TRUE(published.asset) << published.error;
    EXPECT_EQ(published.asset->name, replacement.name);
}
#endif
