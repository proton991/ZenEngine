#include "AssetLib/SceneLoader.h"
#include "AssetLib/SceneAssetIO.h"
#include "AssetLib/SceneRuntimeBuilder.h"
#include "SceneGraph/Transform.h"
#include "SceneGraph/SceneAnimation.h"
#include <ufbx.h>
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <sstream>
#include <cstring>
#include <algorithm>

using namespace zen;

namespace
{
std::string FbxFixture(const char* name)
{
    return (std::filesystem::path(__FILE__).parent_path() / "Assets" / "FBX" / name).string();
}

void ExpectClose(const Vec3& actual, const ufbx_vec3& expected)
{
    EXPECT_NEAR(actual.x, expected.x, 0.0001);
    EXPECT_NEAR(actual.y, expected.y, 0.0001);
    EXPECT_NEAR(actual.z, expected.z, 0.0001);
}

ufbx_load_opts ReferenceOptions()
{
    ufbx_load_opts options{};
    options.target_axes                 = ufbx_axes_right_handed_y_up;
    options.target_camera_axes          = ufbx_axes_right_handed_y_up;
    options.target_light_axes           = ufbx_axes_right_handed_y_up;
    options.target_unit_meters          = 1;
    options.space_conversion            = UFBX_SPACE_CONVERSION_MODIFY_GEOMETRY;
    options.geometry_transform_handling = UFBX_GEOMETRY_TRANSFORM_HANDLING_HELPER_NODES;
    options.inherit_mode_handling       = UFBX_INHERIT_MODE_HANDLING_HELPER_NODES;
    options.evaluate_skinning           = true;
    return options;
}

void CheckReference(const char* fixture)
{
    const std::string      path   = FbxFixture(fixture);
    asset::SceneLoadResult loaded = asset::LoadScene(path);
    ASSERT_EQ(loaded.status, asset::SceneImportStatus::Success) << fixture << ": " << loaded.error;
    ASSERT_TRUE(loaded.scene);
    const ufbx_load_opts options = ReferenceOptions();
    ufbx_error           error{};
    ufbx_scene*          reference = ufbx_load_file(path.c_str(), &options, &error);
    ASSERT_NE(reference, nullptr);
    const asset::SceneAsset& asset = *loaded.asset;
    EXPECT_EQ(asset.nodes.size(), reference->nodes.count);
    for (const UniquePtr<sg::Node>& node : loaded.scene->scene->GetNodes())
    {
        const ufbx_node& source = *reference->nodes.data[node->GetIndex()];
        const Mat4&      matrix = node->GetData().modelMatrix;
        for (uint32_t column = 0; column < 4; ++column)
        {
            ExpectClose(Vec3(matrix[column]), source.node_to_world.cols[column]);
        }
        if (source.mesh != nullptr)
        {
            const asset::MeshAsset& mesh    = asset.meshes[source.mesh->typed_id];
            const sg::Mesh&         runtime = *node->GetComponent<sg::Mesh>();
            size_t                  submesh = 0;
            for (const asset::MeshPrimitiveAsset& primitive : mesh.primitives)
            {
                if (primitive.vertexCount > 0)
                {
                    const sg::SubMesh& runtimePrimitive = *runtime.GetSubMeshes()[submesh++];
                    if (primitive.materialSlot < source.materials.count)
                    {
                        EXPECT_EQ(runtimePrimitive.GetMaterialIndex(), source.materials.data[primitive.materialSlot]->typed_id);
                    }
                    for (uint32_t vertex = 0; vertex < primitive.vertexCount; ++vertex)
                    {
                        const Vec3      actual   = Vec3(matrix * asset.vertices[primitive.firstVertex + vertex].pos);
                        const ufbx_vec3 position = source.mesh->vertices.data[primitive.sourceVertices[vertex]];
                        const ufbx_vec3 expected = ufbx_transform_position(&source.geometry_to_world, position);
                        ExpectClose(actual, expected);
                    }
                }
            }
        }
    }
    ufbx_free_scene(reference);
}

void CheckAnimatedPose(const asset::LoadedScene& loaded, const ufbx_scene& reference)
{
    for (const UniquePtr<sg::Node>& node : loaded.scene->GetNodes())
    {
        SCOPED_TRACE(node->GetName());
        const ufbx_node& source = *reference.nodes.data[node->GetIndex()];
        const Mat4       world  = node->GetComponent<sg::Transform>()->GetWorldMatrix();
        for (uint32_t column = 0; column < 4; ++column)
        {
            ExpectClose(Vec3(world[column]), source.node_to_world.cols[column]);
        }
    }
    const HeapVector<sg::Material*> materials = loaded.scene->GetComponents<sg::Material>();
    for (const ufbx_material* source : reference.materials)
    {
        const ufbx_material_pbr_maps& pbr      = source->pbr;
        const sg::Material&           material = *materials[source->typed_id];
        if (pbr.base_color.has_value)
        {
            const float factor = pbr.base_factor.has_value ? static_cast<float>(pbr.base_factor.value_real) : 1;
            EXPECT_NEAR(material.baseColorFactor.x, pbr.base_color.value_vec3.x * factor, 0.0002);
            EXPECT_NEAR(material.baseColorFactor.y, pbr.base_color.value_vec3.y * factor, 0.0002);
            EXPECT_NEAR(material.baseColorFactor.z, pbr.base_color.value_vec3.z * factor, 0.0002);
        }
    }
    for (const sg::Light* light : loaded.scene->GetComponents<sg::Light>())
    {
        const ufbx_light&          source = *reference.lights.data[light->sourceIndex];
        const sg::LightProperties& actual = light->GetProperties();
        ExpectClose(Vec3(actual.color), source.color);
        EXPECT_NEAR(actual.intensity, source.cast_light ? source.intensity : 0, 0.0002);
        EXPECT_NEAR(actual.innerConeAngle, glm::radians(source.inner_angle * 0.5), 0.0002);
        EXPECT_NEAR(actual.outerConeAngle, glm::radians(source.outer_angle * 0.5), 0.0002);
    }
    const sg::SceneAssetData& data = loaded.scene->GetAssetData();
    for (const asset::DeformationPrimitiveAsset& deformation : data.deformations)
    {
        const ufbx_node& source = *reference.nodes.data[deformation.node];
        const sg::Node*  node   = nullptr;
        for (const UniquePtr<sg::Node>& candidate : loaded.scene->GetNodes())
        {
            if (candidate->GetIndex() == deformation.node)
            {
                node = candidate.Get();
            }
        }
        ASSERT_NE(node, nullptr);
        for (uint32_t index = 0; index < deformation.vertexCount; ++index)
        {
            const uint32_t point    = deformation.sourceVertices[index];
            const uint32_t corner   = source.mesh->vertex_first_index.data[point];
            ufbx_vec3      expected = ufbx_get_vertex_vec3(&source.mesh->skinned_position, corner);
            if (source.mesh->skinned_is_local)
            {
                expected = ufbx_transform_position(&source.geometry_to_world, expected);
            }
            const asset::Vertex& vertex = loaded.vertices[deformation.firstVertex + index];
            ExpectClose(Vec3(node->GetData().modelMatrix * vertex.pos), expected);
        }
    }
}

void CheckAnimatedReference(const char* fixture)
{
    const std::string      path   = FbxFixture(fixture);
    asset::SceneLoadResult loaded = asset::LoadScene(path);
    ASSERT_TRUE(loaded.scene) << loaded.error;
    asset::SceneLoadResult independent = asset::BuildSceneRuntime(loaded.asset);
    ASSERT_TRUE(independent.scene) << independent.error;
    const HeapVector<asset::Vertex> initial = independent.scene->vertices;
    ufbx_load_opts                  options = ReferenceOptions();
    options.use_blender_pbr_material        = true;
    ufbx_error  error{};
    ufbx_scene* reference = ufbx_load_file(path.c_str(), &options, &error);
    ASSERT_NE(reference, nullptr);
    ASSERT_EQ(loaded.asset->animations.size(), reference->anim_stacks.count);
    ASSERT_FALSE(loaded.asset->animations.empty());
    // Descending clips also exercise restoration of channels absent from a take.
    for (size_t reverse = reference->anim_stacks.count; reverse > 0; --reverse)
    {
        const uint32_t               clip      = static_cast<uint32_t>(reverse - 1);
        const ufbx_anim_stack&       stack     = *reference->anim_stacks.data[clip];
        const asset::AnimationAsset& animation = loaded.asset->animations[clip];
        EXPECT_TRUE(animation.baked);
        EXPECT_EQ(animation.name, std::string(stack.name.data, stack.name.length));
        const float duration = sg::GetAnimationDuration(animation);
        EXPECT_NEAR(duration, stack.time_end - stack.time_begin, 1e-5);
        HeapVector<float> times{0, duration};
        for (uint32_t index = 0; index < 15; ++index)
        {
            times.push_back(duration * (float(index) + 0.371f) / 15);
        }
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
                            if (time >= 0 && time <= duration)
                            {
                                times.push_back(time);
                                times.push_back(std::max(0.0f, std::nextafter(time, -INFINITY)));
                                times.push_back(std::min(duration, std::nextafter(time, INFINITY)));
                            }
                        }
                    }
                }
            }
        }
        std::sort(times.begin(), times.end());
        times.resize(static_cast<size_t>(std::unique(times.begin(), times.end()) - times.begin()));
        for (float time : times)
        {
            SCOPED_TRACE(animation.name + " time=" + std::to_string(time));
            ASSERT_TRUE(sg::EvaluateSceneAnimation(*loaded.scene->scene, clip, time, false));
            const HeapVector<asset::Vertex>& bind = loaded.scene->scene->GetAssetData().bindVertices;
            ASSERT_TRUE(sg::ApplySceneDeformations(
                *loaded.scene->scene, VectorView<const asset::Vertex>(bind.data(), bind.size()), loaded.scene->vertices));
            ufbx_evaluate_opts evaluate{};
            evaluate.evaluate_skinning = true;
            ufbx_scene* expected       = ufbx_evaluate_scene(reference, stack.anim, stack.time_begin + time, &evaluate, &error);
            ASSERT_NE(expected, nullptr);
            CheckAnimatedPose(*loaded.scene, *expected);
            ufbx_free_scene(expected);
            if (::testing::Test::HasFailure())
            {
                break;
            }
        }
    }
    ASSERT_EQ(initial.size(), independent.scene->vertices.size());
    for (size_t index = 0; index < initial.size(); ++index)
    {
        EXPECT_EQ(initial[index].pos, independent.scene->vertices[index].pos);
    }
    ufbx_free_scene(reference);
}
} // namespace

TEST(FbxSceneImport, BakedTransformsSkinsAndMorphsMatchSourceAtKeysAndIntermediateTimes)
{
    const char* fixtures[] = {"maya_anim_pivot_rotate_7700_ascii.fbx",  "maya_anim_no_inherit_scale_7700_ascii.fbx",
                              "maya_anim_interpolation_7700_ascii.fbx", "maya_blend_inbetween_7500_binary.fbx",
                              "maya_instanced_skin_7700_binary.fbx",    "maya_transformed_skin_7700_binary.fbx",
                              "blender_279_sausage_7400_binary.fbx",    "maya_anim_diffuse_curve_7700_ascii.fbx",
                              "maya_anim_light_7500_binary.fbx",        "combined_morph_skin_ascii.fbx"};
    for (const char* fixture : fixtures)
    {
        SCOPED_TRACE(fixture);
        CheckAnimatedReference(fixture);
    }
}

TEST(FbxSceneImport, CombinedMorphSkinRetainsEveryInfluence)
{
    asset::SceneImportResult imported = asset::ImportScene({FbxFixture("combined_morph_skin_ascii.fbx")});
    ASSERT_TRUE(imported.asset) << imported.error;
    ASSERT_EQ(imported.asset->skins.size(), 1u);
    ASSERT_EQ(imported.asset->morphPrimitives.size(), 1u);
    for (const asset::MeshPrimitiveAsset& primitive : imported.asset->meshes[0].primitives)
    {
        ASSERT_FALSE(primitive.influences.empty());
        for (const HeapVector<asset::VertexJointInfluence>& influences : primitive.influences)
        {
            EXPECT_EQ(influences.size(), 6u);
            float total = 0;
            for (const asset::VertexJointInfluence& influence : influences)
            {
                total += influence.weight;
            }
            EXPECT_NEAR(total, 1, 1e-6);
        }
    }
}

TEST(FbxSceneImport, RejectsUnsupportedSkinningAndInvalidOrExceededBakeBudgets)
{
    asset::SceneImportResult unsupported = asset::ImportScene({FbxFixture("max_transformed_skin_7500_binary.fbx")});
    EXPECT_FALSE(unsupported.asset);
    EXPECT_EQ(unsupported.status, asset::SceneImportStatus::UnsupportedFeature);
    EXPECT_NE(unsupported.error.find("dual-quaternion"), std::string::npos);
    asset::SceneImportRequest request{FbxFixture("maya_anim_pivot_rotate_7700_ascii.fbx")};
    request.settings.animationBake.sampleRate = 0;
    asset::SceneImportResult invalid          = asset::ImportScene(request);
    EXPECT_FALSE(invalid.asset);
    EXPECT_EQ(invalid.status, asset::SceneImportStatus::InvalidRequest);
    request.settings.animationBake.sampleRate = 60;
    request.settings.animationBake.maxValues  = 20;
    asset::SceneImportResult limited          = asset::ImportScene(request);
    EXPECT_FALSE(limited.asset);
    EXPECT_EQ(limited.status, asset::SceneImportStatus::ResourceLimit);
}

TEST(FbxSceneImport, RejectsAnExplicitBlendAfterSkinConnectionOrder)
{
    std::ifstream      input(FbxFixture("combined_morph_skin_ascii.fbx"));
    std::ostringstream contents;
    contents << input.rdbuf();
    std::string       document = contents.str();
    const std::string before   = " C: \"OO\",31,1\n C: \"OO\",30,1";
    const size_t      position = document.find(before);
    ASSERT_NE(position, std::string::npos);
    document.replace(position, before.size(), " C: \"OO\",30,1\n C: \"OO\",31,1");
    const std::filesystem::path path =
        std::filesystem::temp_directory_path()
        / ("zen_post_skin_blend_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".fbx");
    {
        std::ofstream output(path);
        output << document;
    }
    asset::SceneImportResult imported = asset::ImportScene({path.string()});
    EXPECT_FALSE(imported.asset);
    EXPECT_EQ(imported.status, asset::SceneImportStatus::UnsupportedFeature);
    EXPECT_NE(imported.error.find("after skin"), std::string::npos);
    std::error_code error;
    std::filesystem::remove(path, error);
}

TEST(FbxSceneImport, AsciiAndBinaryHaveEquivalentGeometryInMeters)
{
    asset::SceneImportResult ascii  = asset::ImportScene({FbxFixture("maya_cube_7500_ascii.fbx")});
    asset::SceneImportResult binary = asset::ImportScene({FbxFixture("maya_cube_7500_binary.fbx")});
    ASSERT_TRUE(ascii.asset) << ascii.error;
    ASSERT_TRUE(binary.asset) << binary.error;
    ASSERT_EQ(ascii.asset->vertices.size(), binary.asset->vertices.size());
    ASSERT_EQ(ascii.asset->indices.size(), 36u);
    EXPECT_EQ(ascii.asset->vertices.size(), 24u);
    for (size_t index = 0; index < ascii.asset->vertices.size(); ++index)
    {
        const asset::Vertex& vertex = ascii.asset->vertices[index];
        EXPECT_EQ(vertex.pos, binary.asset->vertices[index].pos);
        EXPECT_EQ(vertex.normal, binary.asset->vertices[index].normal);
        EXPECT_EQ(vertex.uv0, binary.asset->vertices[index].uv0);
        EXPECT_NEAR(glm::length(Vec3(vertex.pos)), std::sqrt(3.0f) * 0.005f, 1e-7f);
        EXPECT_FLOAT_EQ(glm::length(Vec3(vertex.normal)), 1);
    }
}

TEST(FbxSceneImport, StaticMorphsAndUnweightedSkinVerticesMatchSourceEvaluator)
{
    const char* fixtures[] = {"blender_331_static_blend_shape_7400_binary.fbx", "blender_293_half_skinned_7400_binary.fbx"};
    for (const char* fixture : fixtures)
    {
        SCOPED_TRACE(fixture);
        const std::string      path   = FbxFixture(fixture);
        asset::SceneLoadResult loaded = asset::LoadScene(path);
        ASSERT_TRUE(loaded.scene) << loaded.error;
        const ufbx_load_opts options = ReferenceOptions();
        ufbx_error           error{};
        ufbx_scene*          reference = ufbx_load_file(path.c_str(), &options, &error);
        ASSERT_NE(reference, nullptr);
        const sg::SceneAssetData& data = loaded.scene->scene->GetAssetData();
        EXPECT_FALSE(data.deformations.empty());
        for (const asset::DeformationPrimitiveAsset& deformation : data.deformations)
        {
            const ufbx_node& source = *reference->nodes.data[deformation.node];
            const sg::Node*  node   = nullptr;
            for (const UniquePtr<sg::Node>& candidate : loaded.scene->scene->GetNodes())
            {
                if (candidate->GetIndex() == deformation.node)
                {
                    node = candidate.Get();
                }
            }
            ASSERT_NE(node, nullptr);
            for (uint32_t index = 0; index < deformation.vertexCount; ++index)
            {
                const uint32_t point    = deformation.sourceVertices[index];
                const uint32_t corner   = source.mesh->vertex_first_index.data[point];
                ufbx_vec3      expected = ufbx_get_vertex_vec3(&source.mesh->skinned_position, corner);
                if (source.mesh->skinned_is_local)
                {
                    expected = ufbx_transform_position(&source.geometry_to_world, expected);
                }
                const asset::Vertex& vertex = loaded.scene->vertices[deformation.firstVertex + index];
                ExpectClose(Vec3(node->GetData().modelMatrix * vertex.pos), expected);
            }
        }
        ufbx_free_scene(reference);
    }
}

TEST(FbxSceneImport, ConvertedWorldGeometryAndInstanceMaterialsMatchSource)
{
    const char* fixtures[] = {"maya_cube_7500_ascii.fbx",
                              "max_instanced_material_7700_ascii.fbx",
                              "synthetic_geometric_transform_7500_ascii.fbx",
                              "maya_instanced_pivots_7700_ascii.fbx",
                              "maya_axes_7700_ascii.fbx",
                              "maya_camera_light_axes_y_up_7700_ascii.fbx"};
    for (const char* fixture : fixtures)
    {
        CheckReference(fixture);
    }
}

TEST(FbxSceneImport, EmbeddedPixelsOutliveParserAndAreSharedAcrossRuntimeInstances)
{
    asset::SceneImportResult imported = asset::ImportScene({FbxFixture("blender_293_embedded_textures_7400_binary.fbx")});
    ASSERT_TRUE(imported.asset) << imported.error;
    ASSERT_FALSE(imported.asset->images.empty());
    asset::SceneLoadResult first  = asset::BuildSceneRuntime(imported.asset);
    asset::SceneLoadResult second = asset::BuildSceneRuntime(imported.asset);
    ASSERT_TRUE(first.scene) << first.error;
    ASSERT_TRUE(second.scene) << second.error;
    const HeapVector<sg::Texture*> a = first.scene->scene->GetComponents<sg::Texture>();
    const HeapVector<sg::Texture*> b = second.scene->scene->GetComponents<sg::Texture>();
    for (size_t index = 0; index < imported.asset->textures.size(); ++index)
    {
        EXPECT_EQ(a[index]->pixels.Get(), b[index]->pixels.Get());
        EXPECT_FALSE(a[index]->GetBytes().empty());
    }
}

TEST(FbxSceneImport, EquivalentGltfAgreesOnCanonicalCornersAndPlacement)
{
    asset::SceneLoadResult fbx  = asset::LoadScene(FbxFixture("maya_cube_7500_ascii.fbx"));
    asset::SceneLoadResult gltf = asset::LoadScene(FbxFixture("equivalent_cube.gltf"));
    ASSERT_TRUE(fbx.scene) << fbx.error;
    ASSERT_TRUE(gltf.scene) << gltf.error;
    EXPECT_EQ(fbx.scene->vertices.size(), gltf.scene->vertices.size());
    EXPECT_EQ(fbx.scene->indices.size(), gltf.scene->indices.size());
    for (const asset::Vertex& vertex : fbx.scene->vertices)
    {
        bool found = false;
        for (const asset::Vertex& expected : gltf.scene->vertices)
        {
            found |= glm::length(vertex.pos - expected.pos) < 1e-6f && glm::length(vertex.normal - expected.normal) < 1e-6f
                  && glm::length(vertex.uv0 - expected.uv0) < 1e-6f;
        }
        EXPECT_TRUE(found);
    }
    const asset::MaterialAsset& a = fbx.asset->materials[0];
    const asset::MaterialAsset& b = gltf.asset->materials[0];
    EXPECT_LT(glm::length(a.baseColorFactor - b.baseColorFactor), 1e-6f);
    EXPECT_FLOAT_EQ(a.metallicFactor, b.metallicFactor);
    EXPECT_FLOAT_EQ(a.roughnessFactor, b.roughnessFactor);
    EXPECT_EQ(fbx.scene->scene->GetAABB(), gltf.scene->scene->GetAABB());
    asset::PrepareScenePlacement(*fbx.scene->scene, asset::ScenePlacement::CenterAndNormalize);
    asset::PrepareScenePlacement(*gltf.scene->scene, asset::ScenePlacement::CenterAndNormalize);
    EXPECT_EQ(fbx.scene->scene->GetAABB(), gltf.scene->scene->GetAABB());
}

TEST(FbxSceneImport, PreservesNamedUvSetsAndSecondaryCornerColors)
{
    asset::SceneImportResult imported = asset::ImportScene({FbxFixture("maya_uv_and_color_sets_7500_ascii.fbx")});
    ASSERT_TRUE(imported.asset) << imported.error;
    const asset::SceneAsset& scene = *imported.asset;
    ASSERT_EQ(scene.vertexTexCoordSets.size(), 3u); // Default, UVA, UVB.
    ASSERT_EQ(scene.extraVertexAttributes.size(), 1u);
    const asset::VertexAttributeAsset& colors = scene.extraVertexAttributes[0];
    EXPECT_EQ(colors.semantic, "COLOR_1");
    EXPECT_EQ(colors.domain, asset::VertexAttributeDomain::PrimitiveVertex);
    ASSERT_EQ(colors.values.size(), scene.vertices.size() * 4);
    bool differentUVs    = false;
    bool differentColors = false;
    for (size_t vertex = 0; vertex < scene.vertices.size(); ++vertex)
    {
        ASSERT_EQ(scene.vertexTexCoords[vertex].size(), 3u);
        EXPECT_EQ(scene.vertexTexCoords[vertex][0], scene.vertexTexCoords[vertex][1]);
        differentUVs |= scene.vertexTexCoords[vertex][1] != scene.vertexTexCoords[vertex][2];
        for (uint32_t channel = 0; channel < 4; ++channel)
        {
            differentColors |= std::abs(scene.vertices[vertex].color[channel] - colors.values[vertex * 4 + channel]) > 1e-6;
        }
    }
    EXPECT_TRUE(differentUVs);
    EXPECT_TRUE(differentColors);
}

TEST(FbxSceneImport, ExternalTexturesResolveRelativeToSourceAndReportMissingDependencies)
{
    const std::filesystem::path root =
        std::filesystem::temp_directory_path()
        / ("zen_fbx_texture_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(root);
    std::ifstream      input(FbxFixture("maya_cube_7500_ascii.fbx"));
    std::ostringstream contents;
    contents << input.rdbuf();
    std::string  document = contents.str();
    const size_t objects  = document.find("\n\tAnimationStack:");
    ASSERT_NE(objects, std::string::npos);
    document.insert(objects, R"(
    Geometry: 990, "Geometry::NoUV", "Mesh" {
        Vertices: *9 { a: 0,0,0,1,0,0,0,1,0 }
        PolygonVertexIndex: *3 { a: 0,1,-3 }
        LayerElementMaterial: 0 {
            MappingInformationType: "AllSame"
            ReferenceInformationType: "IndexToDirect"
            Materials: *1 { a: 0 }
        }
        Layer: 0 {
            LayerElement: { Type: "LayerElementMaterial"
                TypedIndex: 0
            }
        }
    }
    Model: 991, "Model::NoUV", "Mesh" { Version: 232 }
    Material: 889, "Material::Untextured", "" { Version: 102 }
    Texture: 888, "Texture::external", "" {
        Type: "TextureVideoClip"
        Version: 202
        TextureName: "Texture::external"
        Media: "Video::external"
        FileName: "pixel.tga"
        RelativeFilename: "pixel.tga"
        ModelUVTranslation: 0,0
        ModelUVScaling: 1,1
    }
)");
    const size_t connections = document.find("Connections:  {");
    ASSERT_NE(connections, std::string::npos);
    document.insert(connections + std::string("Connections:  {").size(),
                    "\n C: \"OP\",888,1907674217712,\"DiffuseColor\"\n C: \"OP\",888,1907674217712,\"NormalMap\"\n"
                    " C: \"OO\",991,0\n C: \"OO\",990,991\n C: \"OO\",889,991\n C: \"OO\",1907674217712,991\n");
    {
        std::ofstream model(root / "external.fbx");
        model << document;
        // Two rows in top-left order: red, green; validates shared decoder orientation.
        const uint8_t tga[] = {0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 2, 0, 24, 32, 0, 0, 255, 0, 255, 0};
        std::ofstream texture(root / "pixel.tga", std::ios::binary);
        texture.write(reinterpret_cast<const char*>(tga), sizeof(tga));
    }
    asset::SceneImportResult loaded = asset::ImportScene({(root / "external.fbx").string()});
    ASSERT_TRUE(loaded.asset) << loaded.error;
    ASSERT_EQ(loaded.asset->images.size(), 2u);
    ASSERT_EQ(loaded.asset->dependencies.size(), 1u);
    EXPECT_TRUE(std::filesystem::equivalent(std::filesystem::u8path(loaded.asset->dependencies[0].path), root / "pixel.tga"));
    const asset::ImagePixels& pixels = *loaded.asset->images[0].pixels;
    ASSERT_EQ(pixels.bytes.size(), 8u);
    EXPECT_EQ(pixels.bytes[0], 255);
    EXPECT_EQ(pixels.bytes[1], 0);
    EXPECT_EQ(pixels.bytes[4], 0);
    EXPECT_EQ(pixels.bytes[5], 255);
    const asset::TextureId normal = loaded.asset->materials[0].m_pNormalTexture;
    ASSERT_TRUE(normal.IsValid());
    const asset::ImagePixels& normalPixels = *loaded.asset->images[loaded.asset->textures[normal.value].image.value].pixels;
    EXPECT_EQ(normalPixels.bytes[1], 255);
    EXPECT_EQ(normalPixels.bytes[5], 0);
    std::string  missingName = document;
    const size_t textureName = missingName.find("TextureName: \"Texture::external\"");
    ASSERT_NE(textureName, std::string::npos);
    missingName.insert(textureName, "Properties70: { P: \"UVSet\", \"KString\", \"\", \"\", \"MissingUV\" }\n        ");
    {
        std::ofstream model(root / "external.fbx");
        model << missingName;
    }
    asset::SceneImportRequest namedRequest{(root / "external.fbx").string()};
    namedRequest.useCache             = true;
    namedRequest.cacheDirectory       = (root / "cache").string();
    asset::SceneImportResult fallback = asset::ImportScene(namedRequest);
    ASSERT_TRUE(fallback.asset) << fallback.error;
    const uint32_t namedSlot = fallback.asset->materials[0].texCoordSets.baseColor;
    EXPECT_GT(namedSlot, 0u);
    bool warned = false;
    for (const asset::SceneImportDiagnostic& diagnostic : fallback.diagnostics)
    {
        warned |= diagnostic.message.find("requests missing UV set 'MissingUV'") != std::string::npos;
    }
    EXPECT_TRUE(warned);
    for (const asset::VertexTextureCoordinates& coordinates : fallback.asset->vertexTexCoords)
    {
        ASSERT_GT(coordinates.size(), namedSlot);
        EXPECT_EQ(coordinates[namedSlot], coordinates[0]);
    }
    asset::SceneLoader     parallel(1, 2);
    asset::SceneLoadResult parallelFallback = parallel.LoadAsync(namedRequest).get();
    ASSERT_TRUE(parallelFallback.scene) << parallelFallback.error;
    ASSERT_EQ(parallelFallback.diagnostics.size(), fallback.diagnostics.size());
    for (size_t index = 0; index < fallback.diagnostics.size(); ++index)
    {
        EXPECT_EQ(parallelFallback.diagnostics[index].message, fallback.diagnostics[index].message);
    }
    EXPECT_FALSE(parallelFallback.cacheHit);
    ASSERT_TRUE(asset::WaitForSceneAssetCacheWrites().empty());
    EXPECT_TRUE(asset::LoadScene(namedRequest).cacheHit);
    const std::filesystem::file_time_type timestamp = std::filesystem::last_write_time(root / "pixel.tga");
    {
        std::fstream texture(root / "pixel.tga", std::ios::in | std::ios::out | std::ios::binary);
        texture.seekp(20);
        texture.put(char(128));
    }
    std::filesystem::last_write_time(root / "pixel.tga", timestamp);
    asset::SceneLoadResult recolored = asset::LoadScene(namedRequest);
    ASSERT_TRUE(recolored.scene) << recolored.error;
    EXPECT_FALSE(recolored.cacheHit);
    EXPECT_EQ(recolored.asset->images[0].pixels->bytes[0], 128u);
    ASSERT_TRUE(asset::WaitForSceneAssetCacheWrites().empty());
    EXPECT_TRUE(asset::LoadScene(namedRequest).cacheHit);
    namedRequest.settings.fallbackMissingNamedUVSets = false;
    asset::SceneImportResult strict                  = asset::ImportScene(namedRequest);
    EXPECT_FALSE(strict.asset);
    EXPECT_NE(strict.error.find("missing UV set"), std::string::npos);
    // An unused textured slot must not demand UVs. Selecting it on the same
    // triangle must still fail, rather than silently supplying zero coordinates.
    ASSERT_EQ(loaded.asset->meshes.size(), 2u);
    std::string  missingUV         = document;
    const size_t materialSelection = missingUV.find("Materials: *1 { a: 0 }");
    ASSERT_NE(materialSelection, std::string::npos);
    missingUV.replace(materialSelection, std::string("Materials: *1 { a: 0 }").size(), "Materials: *1 { a: 1 }");
    {
        std::ofstream model(root / "external.fbx");
        model << missingUV;
    }
    asset::SceneImportResult invalidUV = asset::ImportScene({(root / "external.fbx").string()});
    EXPECT_FALSE(invalidUV.asset);
    EXPECT_NE(invalidUV.error.find("missing UV set"), std::string::npos);
    {
        std::ofstream model(root / "external.fbx");
        model << document;
    }
    std::error_code error;
    std::filesystem::remove(root / "pixel.tga", error);
    loaded = asset::ImportScene({(root / "external.fbx").string()});
    EXPECT_FALSE(loaded.asset);
    EXPECT_EQ(loaded.status, asset::SceneImportStatus::SourceUnavailable);
    EXPECT_NE(loaded.error.find("pixel.tga"), std::string::npos);
    // Bytes no decoder accepts (a TIFF or DDS in practice) fail the import with the
    // decoder's reason, never an empty image that later stages would index.
    {
        std::ofstream texture(root / "pixel.tga", std::ios::binary);
        texture << "not an image";
    }
    loaded = asset::ImportScene({(root / "external.fbx").string()});
    EXPECT_FALSE(loaded.asset);
    EXPECT_NE(loaded.error.find("Unsupported or corrupt image"), std::string::npos) << loaded.error;
    // A relocated asset whose recorded paths are stale finds the file name beside the
    // FBX, and records both stale locations as missing preferred files.
    {
        const uint8_t tga[] = {0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 2, 0, 24, 32, 0, 0, 255, 0, 255, 0};
        std::ofstream texture(root / "pixel.tga", std::ios::binary);
        texture.write(reinterpret_cast<const char*>(tga), sizeof(tga));
    }
    std::string relocated = document;
    for (const char* key : {"FileName: \"pixel.tga\"", "RelativeFilename: \"pixel.tga\""})
    {
        size_t position = relocated.find(key);
        while (position != std::string::npos)
        {
            const std::string replacement = std::string(key).starts_with("FileName")
                                              ? "FileName: \"Z:/stale/authoring/pixel.tga\""
                                              : "RelativeFilename: \"stale/authoring/pixel.tga\"";
            relocated.replace(position, std::strlen(key), replacement);
            position = relocated.find(key, position + replacement.size());
        }
    }
    {
        std::ofstream model(root / "external.fbx");
        model << relocated;
    }
    loaded = asset::ImportScene({(root / "external.fbx").string()});
    ASSERT_TRUE(loaded.asset) << loaded.error;
    uint32_t found   = 0;
    uint32_t missing = 0;
    for (const asset::SceneDependency& dependency : loaded.asset->dependencies)
    {
        found += dependency.exists && std::filesystem::equivalent(std::filesystem::u8path(dependency.path), root / "pixel.tga");
        missing += dependency.exists ? 0 : 1;
    }
    EXPECT_EQ(found, 1u);
    EXPECT_EQ(missing, 2u);
    std::filesystem::remove(root / "pixel.tga", error);
    std::filesystem::remove(root / "external.fbx", error);
    std::filesystem::remove_all(root / "cache", error);
    std::filesystem::remove(root, error);
}

TEST(FbxSceneImport, ConcurrentFormatsMatchSerialResultsAndCancellationIsIsolated)
{
    asset::SceneLoader loader(2, 2);
    const char*        fixtures[] = {"maya_cube_7500_ascii.fbx", "blender_293_embedded_textures_7400_binary.fbx",
                                     "max_instanced_material_7700_ascii.fbx"};
    HeapVector<std::future<asset::SceneLoadResult>> pending;
    for (const char* fixture : fixtures)
    {
        pending.push_back(loader.LoadAsync(FbxFixture(fixture)));
    }
    for (size_t index = 0; index < pending.size(); ++index)
    {
        asset::SceneLoadResult parallel = pending[index].get();
        asset::SceneLoadResult serial   = asset::LoadScene(FbxFixture(fixtures[index]));
        ASSERT_TRUE(parallel.scene) << parallel.error;
        ASSERT_TRUE(serial.scene) << serial.error;
        ASSERT_EQ(parallel.scene->vertices.size(), serial.scene->vertices.size());
        EXPECT_TRUE(std::equal(parallel.scene->indices.begin(), parallel.scene->indices.end(), serial.scene->indices.begin(),
                               serial.scene->indices.end()));
        for (size_t vertex = 0; vertex < parallel.scene->vertices.size(); ++vertex)
        {
            EXPECT_EQ(parallel.scene->vertices[vertex].pos, serial.scene->vertices[vertex].pos);
            EXPECT_EQ(parallel.scene->vertices[vertex].uv0, serial.scene->vertices[vertex].uv0);
        }
    }
    asset::SceneImportRequest cancelled{FbxFixture(fixtures[0])};
    cancelled.control.Reset(new asset::SceneImportControl);
    cancelled.control->Cancel();
    EXPECT_EQ(loader.LoadAsync(cancelled).get().status, asset::SceneImportStatus::Cancelled);
    EXPECT_EQ(loader.LoadAsync(FbxFixture(fixtures[0])).get().status, asset::SceneImportStatus::Success);
}

TEST(FbxSceneImport, InvalidSceneSelectionAndCorruptInputPublishNothing)
{
    asset::SceneImportRequest request{FbxFixture("maya_cube_7500_ascii.fbx")};
    request.settings.sceneIndex     = 1;
    asset::SceneImportResult result = asset::ImportScene(request);
    EXPECT_EQ(result.status, asset::SceneImportStatus::InvalidRequest);
    EXPECT_FALSE(result.asset);
    const std::filesystem::path path =
        std::filesystem::temp_directory_path()
        / ("zen_corrupt_scene_import_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".fbx");
    {
        std::ofstream stream(path, std::ios::binary);
        stream << "Kaydara FBX Binary";
    }
    result = asset::ImportScene({path.string()});
    EXPECT_EQ(result.status, asset::SceneImportStatus::InvalidData);
    EXPECT_FALSE(result.asset);
    std::error_code error;
    std::filesystem::remove(path, error);
}
