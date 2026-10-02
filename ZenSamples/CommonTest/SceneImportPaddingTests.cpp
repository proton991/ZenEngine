#include <gtest/gtest.h>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <draco/compression/encode.h>
#include <draco/mesh/triangle_soup_mesh_builder.h>
#include "AssetLib/FastGLTFLoader.h"
#include "SceneGraph/Scene.h"
#include "SceneGraph/Material.h"

using namespace zen;

namespace
{
class PaddingFixture
{
public:
    PaddingFixture()
    {
        const int64_t identifier = std::chrono::steady_clock::now().time_since_epoch().count();

        m_directory = std::filesystem::temp_directory_path() / ("ZenGLTFPaddingTests_" + std::to_string(identifier));

        if (!std::filesystem::create_directory(m_directory))
        {
            throw std::runtime_error("Could not create glTF padding fixture directory");
        }
    }

    ~PaddingFixture()
    {
        std::error_code error;

        std::filesystem::remove_all(m_directory, error);
    }

    std::string Write(const char* name, const std::string& contents)
    {
        const std::filesystem::path file = m_directory / name;

        std::ofstream output(file, std::ios::binary);

        output.write(contents.data(), static_cast<std::streamsize>(contents.size()));

        if (!output)
        {
            throw std::runtime_error("Could not write glTF padding fixture");
        }

        const std::string result = file.string();

        return result;
    }

private:
    std::filesystem::path m_directory;
};

std::string PaddedTriangleJson(size_t byteLength, bool embeddedBuffer)
{
    const std::string buffer = embeddedBuffer ? "{\"byteLength\":36}"
                                              : "{\"byteLength\":36,\"uri\":\"data:application/octet-stream;base64,"
                                                "AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAA\"}";

    std::string result       = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,"
                               "\"scenes\":[{\"nodes\":[0]}],\"nodes\":[{\"mesh\":0}],"
                               "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0}}]}],"
                               "\"buffers\":["
                       + buffer
                       + "],"
                         "\"bufferViews\":[{\"buffer\":0,\"byteLength\":36}],"
                         "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,"
                         "\"type\":\"VEC3\",\"min\":[0,0,0],\"max\":[1,1,0]}],"
                         "\"extras\":{\"padding\":\"";

    const std::string suffix = "\"}}";

    if (result.size() + suffix.size() > byteLength)
    {
        throw std::runtime_error("glTF padding fixture length is too small");
    }

    result.append(byteLength - result.size() - suffix.size(), 'x');

    result += suffix;

    return result;
}

std::string TriangleGlbFromJson(std::string json, bool embeddedBuffer)
{
    while (json.size() % 4 != 0)
    {
        json += ' ';
    }

    const uint32_t byteLength = static_cast<uint32_t>(20 + json.size() + (embeddedBuffer ? 44 : 0));

    const uint32_t header[]{0x46546C67, 2, byteLength, static_cast<uint32_t>(json.size()), 0x4E4F534A};

    std::string result(byteLength, '\0');

    std::memcpy(result.data(), header, sizeof(header));

    std::memcpy(result.data() + sizeof(header), json.data(), json.size());

    if (embeddedBuffer)
    {
        const uint32_t binaryHeader[]{36, 0x004E4942};

        const float positions[]{0, 0, 0, 1, 0, 0, 0, 1, 0};

        const size_t offset = sizeof(header) + json.size();

        std::memcpy(result.data() + offset, binaryHeader, sizeof(binaryHeader));

        std::memcpy(result.data() + offset + sizeof(binaryHeader), positions, sizeof(positions));
    }

    return result;
}

std::string TriangleGlb(size_t jsonLength, bool embeddedBuffer)
{
    const std::string result = TriangleGlbFromJson(PaddedTriangleJson(jsonLength, embeddedBuffer), embeddedBuffer);

    return result;
}

void ReplaceJsonToken(std::string& json, const std::string& before, const std::string& after)
{
    size_t offset = json.find(before);

    while (offset != std::string::npos)
    {
        json.replace(offset, before.size(), after);

        offset = json.find(before, offset + after.size());
    }
}

std::string DecimalTriangleJson(bool embeddedBuffer)
{
    std::string result = PaddedTriangleJson(600, embeddedBuffer);

    ReplaceJsonToken(result, "\"scene\":0", "\"scene\":0e0");

    ReplaceJsonToken(result, "\"nodes\":[0]", "\"nodes\":[-0.0]");

    ReplaceJsonToken(result, "\"mesh\":0", "\"mesh\":0.000");

    ReplaceJsonToken(result, "\"POSITION\":0", "\"POSITION\":0.0e20");

    ReplaceJsonToken(result, "\"buffer\":0", "\"buffer\":0E+0");

    ReplaceJsonToken(result, "\"bufferView\":0", "\"bufferView\":0.000e-2");

    ReplaceJsonToken(result, "\"byteLength\":36", "\"byteLength\":3.600e+1");

    ReplaceJsonToken(result, "\"componentType\":5126", "\"componentType\":5.126e3");

    ReplaceJsonToken(result, "\"count\":3", "\"count\":30.00e-1");

    ReplaceJsonToken(result, "\"padding\":",
                     "\"literal\":\"0.0 \\\"1e2\\\" -0.0\","
                     "\"fraction\":1.000000000000000000001,\"large\":1e30,\"padding\":");

    return result;
}

void ExpectTriangleImport(const std::string& file, size_t jsonLength)
{
    sg::Scene scene;

    asset::FastGLTFLoader loader;

    ASSERT_NO_THROW(loader.LoadFromFile(file, &scene));

    ASSERT_EQ(loader.GetVertices().size(), 3u);

    EXPECT_EQ(loader.GetIndices().size(), 3u);

    EXPECT_EQ(scene.GetRenderableCount(), 1u);

    EXPECT_EQ(scene.GetAssetData().sourceDocument.size(), jsonLength);

    EXPECT_EQ(Vec3(loader.GetVertices()[1].pos), Vec3(1, 0, 0));

    EXPECT_EQ(Vec3(loader.GetVertices()[2].pos), Vec3(0, 1, 0));
}
} // namespace

TEST(SceneImportPadding, JsonNearMappedPageBoundariesHasSafeParserPadding)
{
    PaddingFixture fixture;

    for (size_t length : {4072u, 4092u, 4096u, 8168u, 8188u, 8192u})
    {
        SCOPED_TRACE(length);

        const std::string json = PaddedTriangleJson(length, false);

        ASSERT_EQ(json.size(), length);

        const std::string file = fixture.Write("triangle.gltf", json);

        ExpectTriangleImport(file, length);
    }
}

TEST(SceneImportPadding, GlbJsonAtFilePageBoundaryHasSafeParserPadding)
{
    PaddingFixture fixture;

    for (size_t fileLength : {4072u, 4092u, 4096u, 8168u, 8192u})
    {
        SCOPED_TRACE(fileLength);

        const size_t jsonLength    = fileLength - 20;

        const std::string contents = TriangleGlb(jsonLength, false);

        ASSERT_EQ(contents.size(), fileLength);

        const std::string file = fixture.Write("triangle.glb", contents);

        ExpectTriangleImport(file, jsonLength);
    }
}

TEST(SceneImportPadding, GlbJsonAndEmbeddedBinaryCrossMappedPageBoundaries)
{
    PaddingFixture fixture;

    for (size_t jsonLength : {4032u, 4052u, 4072u, 4096u, 8128u, 8168u})
    {
        SCOPED_TRACE(jsonLength);

        const std::string contents = TriangleGlb(jsonLength, true);

        const std::string file     = fixture.Write("triangle.glb", contents);

        ExpectTriangleImport(file, jsonLength);
    }
}

TEST(SceneImportNumbers, DecimalAndExponentIntegersPreserveTheAuthoredDocument)
{
    PaddingFixture fixture;

    const std::string json = DecimalTriangleJson(false);

    sg::Scene scene;

    asset::FastGLTFLoader loader;

    ASSERT_NO_THROW(loader.LoadFromFile(fixture.Write("numbers.gltf", json), &scene));

    ASSERT_EQ(loader.GetVertices().size(), 3u);

    EXPECT_EQ(Vec3(loader.GetVertices()[1].pos), Vec3(1, 0, 0));

    EXPECT_EQ(scene.GetAssetData().sourceDocument, json);
}

TEST(SceneImportNumbers, GlbIntegerNormalizationPreservesTheEmbeddedBinaryOffset)
{
    PaddingFixture fixture;

    std::string json = DecimalTriangleJson(true);

    while (json.size() % 4 != 0)
    {
        json += ' ';
    }

    sg::Scene scene;

    asset::FastGLTFLoader loader;

    ASSERT_NO_THROW(loader.LoadFromFile(fixture.Write("numbers.glb", TriangleGlbFromJson(json, true)), &scene));

    ASSERT_EQ(loader.GetVertices().size(), 3u);

    EXPECT_EQ(Vec3(loader.GetVertices()[1].pos), Vec3(1, 0, 0));

    EXPECT_EQ(Vec3(loader.GetVertices()[2].pos), Vec3(0, 1, 0));

    EXPECT_EQ(scene.GetAssetData().sourceDocument, json);
}

TEST(SceneImportNumbers, InvalidAndFractionalIntegerTokensAreNotRoundedIntoValidIndices)
{
    PaddingFixture fixture;

    for (const char* index : {"00.0", "-01.0", "0..0", "0e+", "0.000000000000000000001"})
    {
        SCOPED_TRACE(index);

        std::string json = PaddedTriangleJson(600, false);

        ReplaceJsonToken(json, "\"scene\":0", std::string("\"scene\":") + index);

        sg::Scene scene;

        asset::FastGLTFLoader loader;

        EXPECT_THROW(loader.LoadFromFile(fixture.Write("invalid.gltf", json), &scene), std::exception);
    }
}

TEST(SceneImportNumbers, ZeroWithAnOversizedExponentIsExactlyZero)
{
    PaddingFixture fixture;

    std::string json = PaddedTriangleJson(600, false);

    ReplaceJsonToken(json, "\"scene\":0", "\"scene\":0.0e9999999999999999999999999");

    sg::Scene scene;

    asset::FastGLTFLoader loader;

    ASSERT_NO_THROW(loader.LoadFromFile(fixture.Write("zero.gltf", json), &scene));

    EXPECT_EQ(loader.GetVertices().size(), 3u);

    EXPECT_EQ(scene.GetAssetData().sourceDocument, json);
}

TEST(SceneImportNumbers, ManuallyImportedExtensionIntegersUseTheSameNormalization)
{
    PaddingFixture fixture;

    const float values[]{0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 1};

    fixture.Write("numbers.bin", std::string(reinterpret_cast<const char*>(values), sizeof(values)));

    const std::string json = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0.0,"
                             "\"extensionsUsed\":[\"KHR_materials_retroreflection\",\"KHR_texture_transform\"],"
                             "\"extensionsRequired\" : [\"KHR_materials_retroreflection\"],"
                             "\"scenes\":[{\"nodes\":[0.0]}],\"nodes\":[{\"mesh\":0.0}],"
                             "\"meshes\":[{\"primitives\":[{\"material\":0.0,\"attributes\":{\"POSITION\":0.0,"
                             "\"TEXCOORD_0\":1.0,\"TEXCOORD_1\":1e0}}]}],"
                             "\"materials\":[{\"extensions\":{\"KHR_materials_retroreflection\":{"
                             "\"retroreflectionFactor\":0.5,\"retroreflectionTexture\":{\"index\":0.0,"
                             "\"extensions\":{\"KHR_texture_transform\":{\"texCoord\":1.0}}}}}}],"
                             "\"textures\":[{\"source\":0.0}],\"images\":[{\"uri\":\"data:image/png;base64,"
                             "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAusB9Wl6LwsAAAAASUVORK5CYII=\"}],"
                             "\"buffers\":[{\"uri\":\"numbers.bin\",\"byteLength\":6e1}],"
                             "\"bufferViews\":[{\"buffer\":0.0,\"byteLength\":3.6e1},"
                             "{\"buffer\":0.0,\"byteOffset\":3.6e1,\"byteLength\":2.4e1}],"
                             "\"accessors\":[{\"bufferView\":0.0,\"componentType\":5.126e3,\"count\":3.0,"
                             "\"type\":\"VEC3\",\"min\":[0,0,0],\"max\":[1,1,0]},"
                             "{\"bufferView\":1.0,\"componentType\":5.126e3,\"count\":3.0,\"type\":\"VEC2\"}]}";

    sg::Scene scene;

    asset::FastGLTFLoader loader;

    ASSERT_NO_THROW(loader.LoadFromFile(fixture.Write("extension.gltf", json), &scene));

    const zen::HeapVector<sg::Material*> materials = scene.GetComponents<sg::Material>();

    ASSERT_EQ(materials.size(), 2u);

    EXPECT_NE(materials[0]->features.retroreflectionTexture.texture, nullptr);

    EXPECT_EQ(materials[0]->features.retroreflectionTexture.texCoord, 1u);

    EXPECT_EQ(scene.GetAssetData().sourceDocument, json);
}

TEST(SceneImportExtensions, RequiredExtensionRewriteOnlyReplacesTheRootProperty)
{
    PaddingFixture fixture;

    for (bool binary : {false, true})
    {
        std::string json = DecimalTriangleJson(binary);

        json.erase(json.find(",\"extras\":"));

        json += '}';

        json.insert(1, "\"extras\":{\"extensionsRequired\":[\"nested ] \\\" marker\"],"
                       "\"escaped\\\"key\":true},"
                       "\"extensionsUsed\":[\"KHR_node_visibility\",\"KHR_mesh_quantization\"],"
                       "\"extensionsRequired\" : [\"KHR_node_visibility\",\"KHR_mesh_quantization\"],");

        ReplaceJsonToken(json, "\"mesh\":0.000", "\"mesh\":0.000,\"extensions\":{\"KHR_node_visibility\":{\"visible\":false}}");

        sg::Scene scene;

        asset::FastGLTFLoader loader;

        const std::string file =
            binary ? fixture.Write("nested.glb", TriangleGlbFromJson(json, true)) : fixture.Write("nested.gltf", json);

        ASSERT_NO_THROW(loader.LoadFromFile(file, &scene));

        EXPECT_EQ(loader.GetVertices().size(), 3u);

        ASSERT_EQ(scene.GetNodes().size(), 1u);

        EXPECT_FALSE(scene.GetNodes()[0]->IsVisible());

        EXPECT_EQ(scene.GetAssetData().sourceDocument.substr(0, json.size()), json);
    }
}

#if defined(_WIN32)
TEST(SceneImportExtensions, FileUrisComposeWithRequiredExtensionsAndNumberNormalization)
{
    PaddingFixture fixture;

    const float positions[]{0, 0, 0, 1, 0, 0, 0, 1, 0};

    const std::string buffer =
        fixture.Write("triangle.bin", std::string(reinterpret_cast<const char*>(positions), sizeof(positions)));

    const std::string uri = "file:///" + std::filesystem::path(buffer).generic_string();

    for (bool binary : {false, true})
    {
        std::string json = DecimalTriangleJson(false);

        ReplaceJsonToken(json,
                         "data:application/octet-stream;base64,"
                         "AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAA",
                         uri);

        json.insert(1, "\"extensionsUsed\":[\"KHR_node_visibility\"],"
                       "\"extensionsRequired\":[\"KHR_node_visibility\"],");

        sg::Scene scene;

        asset::FastGLTFLoader loader;

        const std::string file =
            binary ? fixture.Write("uri.glb", TriangleGlbFromJson(json, false)) : fixture.Write("uri.gltf", json);

        ASSERT_NO_THROW(loader.LoadFromFile(file, &scene));

        EXPECT_EQ(loader.GetVertices().size(), 3u);

        EXPECT_EQ(scene.GetAssetData().sourceDocument.substr(0, json.size()), json);
    }
}
#endif

TEST(SceneImportExtensions, DracoReferencesAreCheckedBeforeDecodingAndPreserveTheScene)
{
    PaddingFixture fixture;

    draco::TriangleSoupMeshBuilder builder;

    builder.Start(1);

    const int attribute = builder.AddAttribute(draco::GeometryAttribute::POSITION, 3, draco::DT_FLOAT32);

    const float positions[3][3]{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};

    builder.SetAttributeValuesForFace(attribute, draco::FaceIndex(0), positions[0], positions[1], positions[2]);

    builder.SetAttributeUniqueId(attribute, 0);

    const std::unique_ptr<draco::Mesh> mesh = builder.Finalize();

    ASSERT_NE(mesh, nullptr);

    draco::Encoder encoder;

    draco::EncoderBuffer encoded;

    ASSERT_TRUE(encoder.EncodeMeshToBuffer(*mesh, &encoded).ok());

    fixture.Write("triangle.drc", std::string(encoded.data(), encoded.size()));

    const std::string json = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
                             "\"nodes\":[{\"mesh\":0}],\"extensionsUsed\":[\"KHR_draco_mesh_compression\"],"
                             "\"extensionsRequired\":[\"KHR_draco_mesh_compression\"],"
                             "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"indices\":1,"
                             "\"extensions\":{\"KHR_draco_mesh_compression\":{\"bufferView\":0,"
                             "\"attributes\":{\"POSITION\":0}}}}]}],"
                             "\"accessors\":[{\"componentType\":5126,\"count\":3,\"type\":\"VEC3\","
                             "\"min\":[0,0,0],\"max\":[1,1,0]},"
                             "{\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}],"
                             "\"buffers\":[{\"uri\":\"triangle.drc\",\"byteLength\":"
                           + std::to_string(encoded.size())
                           + "}],\"bufferViews\":[{\"buffer\":0,\"byteLength\":" + std::to_string(encoded.size()) + "}]}";

    sg::Scene scene;

    asset::FastGLTFLoader loader;

    ASSERT_NO_THROW(loader.LoadFromFile(fixture.Write("valid-draco.gltf", json), &scene));

    ASSERT_EQ(loader.GetVertices().size(), 3u);

    const sg::Node* previous = scene.GetNodes()[0].Get();

    const char* invalidTokens[][2]{{"\"indices\":1", "\"indices\":2"},
                                   {"\"POSITION\":0},\"indices\"", "\"POSITION\":2},\"indices\""},
                                   {"\"bufferView\":0", "\"bufferView\":1"}};

    for (const char* const* tokens : invalidTokens)
    {
        std::string invalid = json;

        ReplaceJsonToken(invalid, tokens[0], tokens[1]);

        EXPECT_THROW(loader.LoadFromFile(fixture.Write("invalid-draco.gltf", invalid), &scene), std::exception);

        EXPECT_EQ(loader.GetVertices().size(), 3u);

        EXPECT_EQ(scene.GetNodes()[0].Get(), previous);
    }
}
