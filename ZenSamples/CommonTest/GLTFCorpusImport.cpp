#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#if defined(_WIN32)
#    include <Windows.h>
#    if defined(_MSC_VER)
#        include <crtdbg.h>
#    endif
#endif
#include "AssetLib/FastGLTFLoader.h"
#include "SceneGraph/Scene.h"
#include "SceneGraph/SceneAnimation.h"
#include "Systems/SceneEditor.h"

using namespace zen;

namespace
{
void ConfigureDiagnostics()
{
#if defined(_WIN32)
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#    if defined(_MSC_VER)
    _set_error_mode(_OUT_TO_STDERR);
#        if defined(_DEBUG)
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);

    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);

    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);

    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);

    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);

    _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
#        endif
#    endif
#endif
}

void Require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

template <typename Value> void CheckVector(const Value& value, uint32_t components)
{
    for (uint32_t component = 0; component < components; ++component)
    {
        Require(std::isfinite(value[component]), "Imported vector contains a non-finite value");
    }
}

void CheckMatrix(const Mat4& matrix)
{
    for (uint32_t column = 0; column < 4; ++column)
    {
        CheckVector(matrix[column], 4);
    }
}

void CheckBounds(const sg::AABB& bounds)
{
    CheckVector(bounds.GetMin(), 3);

    CheckVector(bounds.GetMax(), 3);

    Require(glm::all(glm::lessThanEqual(bounds.GetMin(), bounds.GetMax())),
            "Imported bounds have reversed minimum and maximum");
}

void CheckAssetData(const sg::SceneAssetData& data)
{
    for (const sg::SkinAsset& skin : data.skins)
    {
        Require(skin.inverseBindMatrices.size() == skin.joints.size(),
                "Skin inverse-bind matrix count differs from its joint count");

        for (const Mat4& matrix : skin.inverseBindMatrices)
        {
            CheckMatrix(matrix);
        }
    }

    for (const sg::AnimationAsset& animation : data.animations)
    {
        for (const sg::AnimationChannel& channel : animation.channels)
        {
            Require(channel.sampler < animation.samplers.size(), "Animation channel references an absent sampler");
        }

        for (const sg::AnimationSampler& sampler : animation.samplers)
        {
            for (size_t index = 0; index < sampler.times.size(); ++index)
            {
                Require(std::isfinite(sampler.times[index]), "Animation timestamp contains a non-finite value");

                if (index != 0)
                {
                    Require(sampler.times[index] > sampler.times[index - 1],
                            "Animation timestamps are not strictly increasing");
                }
            }

            for (float value : sampler.values)
            {
                Require(std::isfinite(value), "Animation contains a non-finite value");
            }
        }
    }

    for (const sg::MorphPrimitiveAsset& primitive : data.morphPrimitives)
    {
        for (float weight : primitive.weights)
        {
            Require(std::isfinite(weight), "Morph weight contains a non-finite value");
        }

        for (const sg::MorphTargetAsset& target : primitive.targets)
        {
            for (const Vec3& position : target.positions)
            {
                CheckVector(position, 3);
            }

            for (const Vec3& normal : target.normals)
            {
                CheckVector(normal, 3);
            }

            for (const Vec3& tangent : target.tangents)
            {
                CheckVector(tangent, 3);
            }
        }
    }
}

void CheckVertices(VectorView<const asset::Vertex> vertices)
{
    for (const asset::Vertex& vertex : vertices)
    {
        CheckVector(vertex.pos, 4);

        CheckVector(vertex.normal, 4);

        CheckVector(vertex.tangent, 4);

        CheckVector(vertex.uv0, 2);

        CheckVector(vertex.uv1, 2);

        CheckVector(vertex.joint0, 4);

        CheckVector(vertex.weight0, 4);

        CheckVector(vertex.color, 4);
    }
}

void CheckScene(const asset::FastGLTFLoader& loader, const sg::Scene& scene)
{
    const std::vector<asset::Vertex>& vertices = loader.GetVertices();

    const std::vector<uint32_t>& indices       = loader.GetIndices();

    CheckVertices(vertices);

    for (uint32_t index : indices)
    {
        Require(index < vertices.size(), "Imported index exceeds the vertex buffer");
    }

    for (const UniquePtr<sg::Node>& node : scene.GetNodes())
    {
        Require(bool(node), "Imported scene contains an absent node");

        CheckMatrix(node->GetData().modelMatrix);

        CheckMatrix(node->GetData().normalMatrix);

        if (node->HasComponent<sg::Transform>())
        {
            CheckMatrix(node->GetComponent<sg::Transform>()->GetWorldMatrix());
        }
    }

    const zen::HeapVector<sg::SubMesh*> subMeshes = scene.GetComponents<sg::SubMesh>();

    for (const sg::SubMesh* subMesh : subMeshes)
    {
        Require(subMesh != nullptr, "Imported scene contains an absent submesh");

        Require(uint64_t(subMesh->GetFirstIndex()) + subMesh->GetIndexCount() <= indices.size(),
                "Submesh index range exceeds the index buffer");

        Require(subMesh->GetVertexCount() <= vertices.size(), "Submesh vertex count exceeds the vertex buffer");

        Require(subMesh->GetMaterial() != nullptr, "Submesh has no material");

        if (subMesh->GetVertexCount() != 0)
        {
            CheckBounds(subMesh->GetAABB());
        }
    }

    const zen::HeapVector<sg::Texture*> textures = scene.GetComponents<sg::Texture>();

    for (const sg::Texture* texture : textures)
    {
        Require(texture != nullptr && texture->width != 0 && texture->height != 0 && !texture->bytesData.empty(),
                "Imported texture has no decoded image data");

        Require(texture->format == asset::Format::R8G8B8A8_UNORM || texture->format == asset::Format::R8G8B8A8_SRGB,
                "Imported image has an unexpected decoded pixel format");

        Require(texture->bytesData.size() == uint64_t(texture->width) * texture->height * 4,
                "Imported image byte count differs from its dimensions");

        for (uint32_t level = 0; level < texture->mipBytes.size(); ++level)
        {
            Require(level < 32, "Imported image has too many mip levels");

            const uint32_t width  = std::max(1u, texture->width >> level);

            const uint32_t height = std::max(1u, texture->height >> level);

            Require(texture->mipBytes[level].size() == uint64_t(width) * height * 4,
                    "Authored image mip byte count differs from its dimensions");
        }

        if (!texture->mipBytes.empty())
        {
            Require(std::equal(texture->bytesData.begin(), texture->bytesData.end(), texture->mipBytes[0].begin(),
                               texture->mipBytes[0].end()),
                    "Authored image level zero differs from the decoded image");
        }
    }

    if (scene.GetRenderableCount() != 0 && !vertices.empty())
    {
        CheckBounds(scene.GetAABB());
    }

    CheckAssetData(scene.GetAssetData());
}

void CheckPosedBounds(const asset::FastGLTFLoader& loader, sg::Scene& scene, VectorView<const asset::Vertex> vertices)
{
    CheckBounds(scene.GetAABB());

    const Vec3 minimum = scene.GetAABB().GetMin();

    const Vec3 maximum = scene.GetAABB().GetMax();

    float magnitude    = 1.0f;

    for (uint32_t axis = 0; axis < 3; ++axis)
    {
        magnitude = std::max(magnitude, std::max(std::abs(minimum[axis]), std::abs(maximum[axis])));
    }

    const Vec3 tolerance(magnitude * 1e-4f);

    for (sg::Node* node : scene.GetRenderableNodes())
    {
        if (node->IsVisible() && node->HasComponent<sg::Mesh>())
        {
            const Mat4 world = node->GetData().modelMatrix;

            for (const sg::SubMesh* primitive : node->GetComponent<sg::Mesh>()->GetSubMeshes())
            {
                for (uint32_t index = 0; index < primitive->GetIndexCount(); ++index)
                {
                    const uint32_t vertex = loader.GetIndices()[primitive->GetFirstIndex() + index];

                    Require(vertex < vertices.size(), "Posed draw references an absent vertex");

                    const Vec3 position(world * vertices[vertex].pos);

                    CheckVector(position, 3);

                    Require(glm::all(glm::greaterThanEqual(position, minimum - tolerance))
                                && glm::all(glm::lessThanEqual(position, maximum + tolerance)),
                            "Scene bounds do not contain its posed rendered geometry");
                }
            }
        }
    }
}

uint32_t CheckMaterialVariants(sg::Scene& scene)
{
    const sg::SceneAssetData& data                 = scene.GetAssetData();

    const zen::HeapVector<sg::Material*> materials = scene.GetComponents<sg::Material>();

    const zen::HeapVector<sg::SubMesh*> primitives = scene.GetComponents<sg::SubMesh>();

    for (uint32_t variant = 0; variant < data.materialVariants.size(); ++variant)
    {
        std::cerr << "ZEN_GLTF_IMPORT_STAGE material_variant=" << variant << '\n';

        Require(scene.SetMaterialVariant(static_cast<int32_t>(variant)), "Imported material variant cannot be selected");

        for (const sg::SubMesh* primitive : primitives)
        {
            const sg::Material* expected = primitive->GetDefaultMaterial();

            for (const sg::MaterialVariantPrimitiveAsset& mapping : data.variantPrimitives)
            {
                if (mapping.mesh == primitive->assetMesh && mapping.primitive == primitive->assetPrimitive
                    && variant < mapping.materials.size() && mapping.materials[variant] >= 0)
                {
                    const uint32_t material = static_cast<uint32_t>(mapping.materials[variant]);

                    Require(material < materials.size(), "Variant references an absent material");

                    expected = materials[material];
                }
            }

            Require(expected != nullptr && primitive->GetMaterial() == expected
                        && primitive->GetMaterialIndex() == expected->index,
                    "Material variant selected a different material from its source mapping");
        }
    }

    HeapVector<sg::Material*> previous;

    for (const sg::SubMesh* primitive : primitives)
    {
        previous.push_back(primitive->GetMaterial());
    }

    Require(!scene.SetMaterialVariant(static_cast<int32_t>(data.materialVariants.size())),
            "An out-of-range material variant was accepted");

    Require(!scene.SetMaterialVariant(-2), "An invalid negative material variant was accepted");

    for (size_t index = 0; index < primitives.size(); ++index)
    {
        Require(primitives[index]->GetMaterial() == previous[index], "Rejected material variant changed the selected material");
    }

    Require(scene.SetMaterialVariant(-1), "Default material bindings cannot be restored");

    for (const sg::SubMesh* primitive : primitives)
    {
        Require(primitive->GetMaterial() == primitive->GetDefaultMaterial(),
                "Restoring the default material variant changed an authored material binding");
    }

    const uint32_t selections = static_cast<uint32_t>(data.materialVariants.size()) + 1;

    return selections;
}

uint32_t CheckAnimations(const asset::FastGLTFLoader& loader, sg::Scene& scene)
{
    const sg::SceneAssetData& data                     = scene.GetAssetData();

    const VectorView<const asset::Vertex> bindVertices = data.bindVertices.empty()
                                                           ? VectorView<const asset::Vertex>(loader.GetVertices())
                                                           : VectorView<const asset::Vertex>(data.bindVertices);

    HeapVector<asset::Vertex> deformed;

    std::cerr << "ZEN_GLTF_IMPORT_STAGE initial_deformation\n";

    Require(sg::ApplySceneDeformations(scene, bindVertices, deformed),
            "Imported initial skin or morph pose cannot be evaluated");

    CheckVertices(deformed);

    CheckPosedBounds(loader, scene, deformed);

    std::cerr << "ZEN_GLTF_IMPORT_STAGE normalize\n";

    const float extent = scene.GetAABB().GetMaxExtent();

    sys::SceneEditor::CenterAndNormalizeScene(&scene);

    if (std::isfinite(extent) && extent > 1e-6f)
    {
        Require(std::abs(scene.GetAABB().GetMaxExtent() - 1.0f) < 1e-4f, "Scene normalization did not produce unit bounds");
    }

    Require(sg::ApplySceneDeformations(scene, bindVertices, deformed), "Normalized skin or morph pose cannot be evaluated");

    CheckVertices(deformed);

    CheckPosedBounds(loader, scene, deformed);

    uint32_t samples = 0;

    for (uint32_t index = 0; index < data.animations.size(); ++index)
    {
        const sg::AnimationAsset& animation = data.animations[index];

        const float duration                = sg::GetAnimationDuration(animation);

        float firstTime                     = duration;

        for (const sg::AnimationSampler& sampler : animation.samplers)
        {
            if (!sampler.times.empty())
            {
                firstTime = std::min(firstTime, sampler.times.front());
            }
        }

        const float times[] = {firstTime, (firstTime + duration) * 0.5f, duration};

        for (float time : times)
        {
            std::cerr << "ZEN_GLTF_IMPORT_STAGE animation=" << index << " time=" << time << '\n';

            Require(sg::EvaluateSceneAnimation(scene, index, time, false), "Imported animation cannot be sampled");

            Require(sg::ApplySceneDeformations(scene, bindVertices, deformed),
                    "Imported animated skin or morph pose cannot be evaluated");

            CheckVertices(deformed);

            CheckScene(loader, scene);

            CheckPosedBounds(loader, scene, deformed);

            ++samples;
        }
    }

    return samples;
}

void PrintResult(const asset::FastGLTFLoader& loader,
                 const sg::Scene&             scene,
                 uint32_t                     samples,
                 const char*                  marker,
                 uint32_t                     variantSelections = 0)
{
    const sg::SceneAssetData& data = scene.GetAssetData();

    size_t channels                = 0;

    for (const sg::AnimationAsset& animation : data.animations)
    {
        channels += animation.channels.size();
    }

    std::cout << marker << "{\"vertices\":" << loader.GetVertices().size() << ",\"indices\":" << loader.GetIndices().size()
              << ",\"nodes\":" << scene.GetNodes().size() << ",\"renderable_nodes\":" << scene.GetRenderableCount()
              << ",\"meshes\":" << scene.GetComponents<sg::Mesh>().size()
              << ",\"submeshes\":" << scene.GetComponents<sg::SubMesh>().size()
              << ",\"materials\":" << scene.GetComponents<sg::Material>().size()
              << ",\"textures\":" << scene.GetComponents<sg::Texture>().size() << ",\"skins\":" << data.skins.size()
              << ",\"animations\":" << data.animations.size() << ",\"animation_channels\":" << channels
              << ",\"animation_samples\":" << samples << ",\"material_variants\":" << data.materialVariants.size()
              << ",\"variant_primitives\":" << data.variantPrimitives.size() << ",\"variant_selections\":" << variantSelections
              << ",\"morph_primitives\":" << data.morphPrimitives.size() << "}\n";

    std::cout.flush();
}
} // namespace

// The Python corpus runner launches one process per asset to isolate native crashes.
#if defined(_WIN32)
int wmain(int argc, wchar_t** argv)
#else
int main(int argc, char** argv)
#endif
{
    ConfigureDiagnostics();

    int result = 0;

    if (argc != 2)
    {
        std::cerr << "Usage: GLTFCorpusImport <model.gltf|model.glb>\n";

        result = 2;
    }
    else
    {
        try
        {
            sg::Scene scene;

            asset::FastGLTFLoader loader;

            std::cerr << "ZEN_GLTF_IMPORT_STAGE load\n";

#if defined(_WIN32)
            const std::u8string utf8 = std::filesystem::path(argv[1]).u8string();

            const std::string path(reinterpret_cast<const char*>(utf8.data()), utf8.size());
#else
            const std::string path(argv[1]);
#endif

            loader.LoadFromFile(path, &scene);

            PrintResult(loader, scene, 0, "ZEN_GLTF_IMPORT_STATE ");

            std::cerr << "ZEN_GLTF_IMPORT_STAGE structure\n";

            CheckScene(loader, scene);

            const uint32_t variants = CheckMaterialVariants(scene);

            const uint32_t samples  = CheckAnimations(loader, scene);

            PrintResult(loader, scene, samples, "ZEN_GLTF_IMPORT_RESULT ", variants);
        }
        catch (const std::exception& error)
        {
            std::cerr << "ZEN_GLTF_IMPORT_ERROR " << error.what() << '\n';

            result = 1;
        }
    }

    return result;
}
