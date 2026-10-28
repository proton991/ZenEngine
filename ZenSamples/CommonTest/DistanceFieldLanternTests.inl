// Optional actual-asset experiment, included after DistanceFieldSceneTests.inl.
// The binary is test-only source geometry; the production GPU builder creates every field sample.
#include "Graphics/Shared/DistanceFieldScene.h"

struct FieldLanternHeader
{
    uint32_t magic, version, triangles, groups;
    float    light[3], metresPerUnit;
};

struct FieldLanternGroup
{
    uint32_t triangles, originalMaterial, alphaMode, doubleSided;
    float    baseAlpha, cutoff;
};
static_assert(sizeof(FieldLanternHeader) == 32);
static_assert(sizeof(FieldLanternGroup) == 24);
static_assert(sizeof(Vec3) == 12);

struct FieldLanternAsset
{
    FieldLanternHeader            header{};
    HeapVector<FieldLanternGroup> groups;
    HeapVector<Vec3>              positions;

    bool Load(const char* path)
    {
        FILE* input = std::fopen(path, "rb");
        bool  valid = input != nullptr;
        if (valid)
        {
            valid = std::fread(&header, sizeof(header), 1, input) == 1 && header.magic == 0x5a4c414eu && header.version == 1
                 && header.triangles == 5962 && header.groups == 3 && header.metresPerUnit > 0;
            if (valid)
            {
                groups.resize(header.groups);
                positions.resize(header.triangles * 3u);
                valid = std::fread(groups.data(), sizeof(FieldLanternGroup), groups.size(), input) == groups.size()
                     && std::fread(positions.data(), sizeof(Vec3), positions.size(), input) == positions.size()
                     && std::fgetc(input) == EOF;
                if (valid)
                {
                    uint32_t triangles = 0;
                    for (const FieldLanternGroup& group : groups)
                    {
                        triangles += group.triangles;
                        valid      = valid && group.alphaMode == 0 && group.baseAlpha == 1.0f;
                    }
                    valid = valid && triangles == header.triangles;
                    for (const Vec3& position : positions)
                    {
                        valid = valid && std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z);
                    }
                }
            }
            std::fclose(input);
        }
        return valid;
    }
};

HeapVector<OracleTriangle> FieldLanternOracle(const FieldLanternAsset& asset, const Mat4& transform)
{
    HeapVector<OracleTriangle> triangles;
    uint32_t                   first = 0;
    // Preserve the original alpha classification before uniform shading materials are created.
    for (const FieldLanternGroup& group : asset.groups)
    {
        for (uint32_t index = 0; index < group.triangles; ++index)
        {
            const uint32_t vertex = 3u * (first + index);
            OracleTriangle triangle{};
            triangle.a        = glm::dvec3(transform * Vec4(asset.positions[vertex], 1));
            triangle.b        = glm::dvec3(transform * Vec4(asset.positions[vertex + 1u], 1));
            triangle.c        = glm::dvec3(transform * Vec4(asset.positions[vertex + 2u], 1));
            triangle.material = group.originalMaterial;
            triangle.alpha    = Vec3(group.baseAlpha);
            if (group.alphaMode == 0 || group.baseAlpha >= (group.alphaMode == 1 ? group.cutoff : 0.5f))
            {
                triangles.push_back(triangle);
            }
        }
        first += group.triangles;
    }
    return triangles;
}

bool InitFieldLantern(RenderDevice* device, const FieldLanternAsset& asset, BuiltFieldSceneFixture& fixture)
{
    UniquePtr<sg::Mesh> mesh = MakeUnique<sg::Mesh>("measured New Sponza lantern");
    Vec3                minimum(std::numeric_limits<float>::max());
    Vec3                maximum(-std::numeric_limits<float>::max());
    for (const Vec3& position : asset.positions)
    {
        minimum = glm::min(minimum, position);
        maximum = glm::max(maximum, position);
        asset::Vertex vertex{};
        vertex.pos    = Vec4(position, 1);
        vertex.normal = Vec4(0, 1, 0, 0);
        vertex.color  = Vec4(1);
        fixture.indices.push_back(static_cast<uint32_t>(fixture.vertices.size()));
        fixture.vertices.push_back(vertex);
    }
    mesh->SetAABB(minimum, maximum);
    uint32_t first = 0;
    for (uint32_t index = 0; index < asset.groups.size(); ++index)
    {
        const FieldLanternGroup& group      = asset.groups[index];
        UniquePtr<sg::Material>  material   = MakeUnique<sg::Material>("uniform original-coverage lantern material");
        material->index                     = index;
        material->data.baseColorFactor      = Vec4(0.5f, 0.5f, 0.5f, group.baseAlpha);
        material->data.surfaceProperties.x  = group.cutoff;
        material->data.surfaceProperties.y  = static_cast<float>(group.alphaMode);
        material->data.materialProperties.z = static_cast<float>(group.doubleSided);
        UniquePtr<sg::SubMesh> primitive =
            MakeUnique<sg::SubMesh>("original lantern primitive", first, group.triangles * 3u, group.triangles * 3u);
        primitive->SetMaterial(index, material.Get());
        mesh->AddSubMesh(primitive.Get());
        fixture.source.AddComponent(std::move(primitive));
        fixture.source.AddComponent(std::move(material));
        first += group.triangles * 3u;
    }
    UniquePtr<sg::Node> node = MakeUnique<sg::Node>(0, "lantern instance");
    node->SetData(0, Mat4(1));
    node->AddComponent(mesh.Get());
    fixture.source.AddRenderableNode(node.Get());
    HeapVector<UniquePtr<sg::Node>> nodes;
    nodes.push_back(std::move(node));
    fixture.source.SetNodes(std::move(nodes));
    fixture.source.AddComponent(std::move(mesh));
    asset::PrepareScenePlacement(fixture.source, asset::ScenePlacement::PreserveAuthored);
    SceneData data{};
    data.pScene      = &fixture.source;
    data.pCamera     = &fixture.camera;
    data.pVertices   = fixture.vertices.data();
    data.numVertices = static_cast<uint32_t>(fixture.vertices.size());
    data.pIndices    = fixture.indices.data();
    data.numIndices  = static_cast<uint32_t>(fixture.indices.size());
    fixture.scene    = MakeUnique<RenderScene>(device, data);
    fixture.scene->LoadSceneMaterials();
    fixture.scene->PrepareBuffers();
    // Construction recomputes the source bounds. Set the receiver cache afterwards to
    // keep the visibility coordinates fixed through the rotation/phase sweep.
    const bool valid = fixture.scene->SetVoxelBounds(sg::AABB(Vec3(-0.7f), Vec3(0.7f)));
    return valid && fixture.scene->Update();
}

double FieldTriangleIntersection(const BuiltFieldRay&              ray,
                                 const HeapVector<OracleTriangle>& triangles,
                                 double                            alphaCutoff = -1.0)
{
    double closest = std::numeric_limits<double>::infinity();
    for (const OracleTriangle& triangle : triangles)
    {
        OracleCandidate candidate{};
        if (OracleIntersect(triangle, glm::dvec3(ray.originMinimum), glm::dvec3(ray.directionMaximum), candidate)
            && candidate.u >= -1e-10 && candidate.v >= -1e-10 && candidate.u + candidate.v <= 1.0 + 1e-10
            && candidate.t >= ray.originMinimum.w && candidate.t < ray.directionMaximum.w
            && (alphaCutoff < 0.0
                || triangle.alpha.x * (1.0 - candidate.u - candidate.v) + triangle.alpha.y * candidate.u
                           + triangle.alpha.z * candidate.v
                       >= alphaCutoff))
        {
            closest = std::min(closest, candidate.t);
        }
    }
    return closest;
}

HeapVector<BuiltFieldRay> FieldLanternRays(const FieldLanternAsset&          asset,
                                           const HeapVector<OracleTriangle>& triangles,
                                           const Mat4&                       transform)
{
    HeapVector<BuiltFieldRay> rays;
    const Vec3     light      = Vec3(transform * Vec4(asset.header.light[0], asset.header.light[1], asset.header.light[2], 1));
    const uint32_t directions = 1024;
    for (uint32_t index = 0; index < directions; ++index)
    {
        const float vertical = 1.0f - 2.0f * (static_cast<float>(index) + 0.5f) / directions;
        const float radius   = std::sqrt(std::max(0.0f, 1.0f - vertical * vertical));
        const float angle    = static_cast<float>(index) * 2.39996322972865332f;
        const Vec3  direction =
            glm::normalize(Vec3(transform * Vec4(radius * std::cos(angle), vertical, radius * std::sin(angle), 0)));
        rays.push_back({Vec4(light, 0), Vec4(direction, 1.5f)});
    }
    // Both approaches to actual curved/thin metal and all eight authored opaque glazing triangles.
    for (uint32_t index = 0; index < triangles.size(); ++index)
    {
        const OracleTriangle& triangle = triangles[index];
        if (index % 19u == 0 || triangle.material == 11)
        {
            const glm::dvec3 cross = glm::cross(triangle.b - triangle.a, triangle.c - triangle.a);
            if (glm::dot(cross, cross) > 1e-24)
            {
                const Vec3 normal = Vec3(glm::normalize(cross));
                const Vec3 center = Vec3((triangle.a + triangle.b + triangle.c) / 3.0);
                rays.push_back({Vec4(center - normal * 0.015f, 0), Vec4(normal, 0.03f)});
                rays.push_back({Vec4(center + normal * 0.015f, 0), Vec4(-normal, 0.03f)});
            }
        }
    }
    return rays;
}

bool WriteFieldLanternTrace(const char*                         path,
                            float                               metresPerUnit,
                            const HeapVector<BuiltFieldRay>&    rays,
                            const HeapVector<double>&           exactDistances,
                            const HeapVector<BuiltFieldResult>& results)
{
    FILE* output = std::fopen(path, "wb");
    bool  valid  = output != nullptr && rays.size() == exactDistances.size() && rays.size() == results.size();
    if (valid)
    {
        valid =
            std::fprintf(output,
                         "ray,origin_x,origin_y,origin_z,direction_x,direction_y,direction_z,maximum,oracle_distance,"
                         "status,distance,hit_x,hit_y,hit_z,normal_x,normal_y,normal_z,reason,work,samples,metres_per_unit\n")
            > 0;
        for (uint32_t index = 0; index < rays.size() && valid; ++index)
        {
            const BuiltFieldRay&    ray    = rays[index];
            const BuiltFieldResult& result = results[index];
            valid =
                std::fprintf(
                    output, "%u,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.17g,%u,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%u,%u,%u,%.9g\n",
                    index, ray.originMinimum.x, ray.originMinimum.y, ray.originMinimum.z, ray.directionMaximum.x,
                    ray.directionMaximum.y, ray.directionMaximum.z, ray.directionMaximum.w, exactDistances[index],
                    result.statistics.x, result.positionDistance.w, result.positionDistance.x, result.positionDistance.y,
                    result.positionDistance.z, result.normal.x, result.normal.y, result.normal.z,
                    static_cast<uint32_t>(result.normal.w), result.statistics.z, result.statistics.w, metresPerUnit)
                > 0;
        }
    }
    if (output != nullptr)
    {
        valid = std::fclose(output) == 0 && valid;
    }
    return valid;
}

TEST_P(DistanceFieldSceneIntegrationTest, DistanceFieldActualLanternVisibilityExperiment)
{
    const char* path = std::getenv("ZEN_FIELD_LANTERN_PATH");
    if (path == nullptr)
    {
        GTEST_SKIP() << "Set ZEN_FIELD_LANTERN_PATH to the hashed lantern-native.bin inspection fixture";
    }
    FieldLanternAsset asset;
    ASSERT_TRUE(asset.Load(path));
    RegisterDistanceFieldPrograms(device);
    DistanceFieldSceneSettings settings;
    settings.cellsPerAxis       = 64;
    settings.cellBatchSize      = 256;
    settings.dispatchesPerFrame = 16;
    const char* toleranceText   = std::getenv("ZEN_FIELD_LANTERN_EPSILON");
    if (toleranceText != nullptr)
    {
        char* end             = nullptr;
        settings.hitTolerance = std::strtof(toleranceText, &end);
        ASSERT_NE(end, toleranceText);
        ASSERT_EQ(*end, '\0');
        ASSERT_TRUE(std::isfinite(settings.hitTolerance));
        ASSERT_GT(settings.hitTolerance, 0.0f);
        ASSERT_LE(settings.hitTolerance, 0.5f / 7.0f);
    }
    const float spacing = 1.4f / (static_cast<float>(settings.cellsPerAxis - 2u) * 7.0f);
    RecordProperty("lantern_fixture", std::string(path));
    RecordProperty("source_triangles", std::to_string(asset.header.triangles));
    RecordProperty("hit_tolerance_brick_units", std::to_string(settings.hitTolerance));
    for (uint32_t pose = 0; pose < 4; ++pose)
    {
        SCOPED_TRACE(pose);
        Mat4 transform(1);
        if ((pose & 1u) != 0)
        {
            transform = glm::rotate(transform, glm::radians(23.0f), Vec3(0, 1, 0));
            transform = glm::rotate(transform, glm::radians(17.0f), Vec3(0, 0, 1));
        }
        if ((pose & 2u) != 0)
        {
            transform = glm::translate(Mat4(1), Vec3(0.5f, 0.25f, 0.75f) * spacing) * transform;
        }
        const HeapVector<OracleTriangle> triangles = FieldLanternOracle(asset, transform);
        const HeapVector<BuiltFieldRay>  rays      = FieldLanternRays(asset, triangles, transform);
        HeapVector<double>               exactDistances;
        for (const BuiltFieldRay& ray : rays)
        {
            exactDistances.push_back(FieldTriangleIntersection(ray, triangles));
        }
        BuiltFieldSceneFixture fixture;
        ASSERT_TRUE(InitFieldLantern(device, asset, fixture));
        ASSERT_EQ(fixture.scene->GetVoxelSceneBounds().GetMin(), Vec3(-0.7f));
        ASSERT_EQ(fixture.scene->GetVoxelSceneBounds().GetMax(), Vec3(0.7f));
        ASSERT_TRUE(fixture.scene->SetInstanceTransform(0, transform));
        ASSERT_TRUE(fixture.scene->Update());
        DistanceFieldScene                          field(device, settings);
        const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        ASSERT_TRUE(CompleteBuiltField(*device, *fixture.scene, field));
        ASSERT_EQ(field.GetState(), DistanceFieldScene::State::Ready) << field.GetFailureReason();
        EXPECT_NEAR(field.GetUniform().gridMinCellSize.w / 7.0f, spacing, 1e-6f);
        const double milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        const char*  fieldPath    = std::getenv("ZEN_FIELD_LANTERN_FIELD_PATH");
        if (fieldPath != nullptr)
        {
            FieldSliceDescription description;
            description.minimum = fixture.scene->GetShadowCasters()[0].bounds.GetMin();
            description.maximum = fixture.scene->GetShadowCasters()[0].bounds.GetMax();
            description.center = Vec3(transform * Vec4(asset.header.light[0], asset.header.light[1], asset.header.light[2], 1));
            description.transform     = transform;
            description.metresPerUnit = asset.header.metresPerUnit;
            description.pose          = pose;
            description.mode          = GetParam();
            const std::string outputPath =
                std::string(fieldPath) + "-mode" + std::to_string(GetParam()) + "-pose" + std::to_string(pose);
            ASSERT_TRUE(WriteBuiltFieldSlices(*device, field, outputPath, description));
            RecordProperty("lantern_pose_" + std::to_string(pose) + "_field_slices", outputPath + ".json");
        }
        const HeapVector<BuiltFieldResult> results = TraceBuiltField(*device, field, rays);
        ASSERT_EQ(results.size(), rays.size());
        const char* tracePath = std::getenv("ZEN_FIELD_LANTERN_TRACE_PATH");
        if (tracePath != nullptr)
        {
            const std::string outputPath =
                std::string(tracePath) + "-mode" + std::to_string(GetParam()) + "-pose" + std::to_string(pose) + ".csv";
            ASSERT_TRUE(WriteFieldLanternTrace(outputPath.c_str(), asset.header.metresPerUnit, rays, exactDistances, results));
            RecordProperty("lantern_pose_" + std::to_string(pose) + "_trace_csv", outputPath);
        }
        uint32_t    blocked = 0, open = 0, leaks = 0, overblocks = 0, unavailable = 0, invalidNormals = 0;
        uint64_t    samples = 0, work = 0;
        double      error    = 0;
        uint32_t    errorRay = 0, diagnosticCount = 0;
        std::string diagnostics;
        for (uint32_t index = 0; index < rays.size(); ++index)
        {
            const bool     exactHit  = std::isfinite(exactDistances[index]);
            const uint32_t status    = results[index].statistics.x;
            blocked                 += exactHit ? 1u : 0u;
            open                    += exactHit ? 0u : 1u;
            leaks                   += exactHit && status == 0u ? 1u : 0u;
            overblocks              += !exactHit && status == 1u ? 1u : 0u;
            unavailable             += status == 2u ? 1u : 0u;
            invalidNormals +=
                status == 1u && glm::dot(Vec3(results[index].normal), Vec3(results[index].normal)) < 0.5f ? 1u : 0u;
            samples += results[index].statistics.w;
            work    += results[index].statistics.z;
            if (exactHit && status == 1u)
            {
                const double distanceError = std::abs(results[index].positionDistance.w - exactDistances[index]);
                if (distanceError > error)
                {
                    error    = distanceError;
                    errorRay = index;
                }
            }
            if (diagnosticCount < 32u
                && ((exactHit && status != 1u) || (!exactHit && status != 0u)
                    || (status == 1u && glm::dot(Vec3(results[index].normal), Vec3(results[index].normal)) < 0.5f)))
            {
                diagnostics += "ray=" + std::to_string(index) + " exact=" + std::to_string(exactDistances[index])
                             + " status=" + std::to_string(status) + " t=" + std::to_string(results[index].positionDistance.w)
                             + " reason=" + std::to_string(static_cast<uint32_t>(results[index].normal.w))
                             + " work=" + std::to_string(results[index].statistics.z)
                             + " samples=" + std::to_string(results[index].statistics.w) + "; ";
                ++diagnosticCount;
            }
            EXPECT_LE(status, 2u);
            EXPECT_LE(results[index].statistics.z, field.GetUniform().dimensions.w);
        }
        const std::string key = "lantern_pose_" + std::to_string(pose);
        RecordProperty(
            key, "blocked=" + std::to_string(blocked) + " open=" + std::to_string(open) + " leaks=" + std::to_string(leaks)
                     + " overblocks=" + std::to_string(overblocks) + " unavailable=" + std::to_string(unavailable)
                     + " invalid_normals=" + std::to_string(invalidNormals)
                     + " max_distance_error_m=" + std::to_string(error * asset.header.metresPerUnit)
                     + " samples=" + std::to_string(samples) + " work=" + std::to_string(work)
                     + " spacing_m=" + std::to_string(field.GetUniform().gridMinCellSize.w * asset.header.metresPerUnit / 7.0f)
                     + " persistent_bytes=" + std::to_string(field.GetPersistentBytes()) + " snapshot_bytes="
                     + std::to_string(field.GetSnapshotBytes()) + " graphs=" + std::to_string(field.GetSubmittedGraphCount())
                     + " construction_ms=" + std::to_string(milliseconds));
        RecordProperty(key + "_diagnostics", diagnostics);
        RecordProperty(key + "_worst_distance_ray", "ray=" + std::to_string(errorRay)
                                                        + " exact=" + std::to_string(exactDistances[errorRay])
                                                        + " t=" + std::to_string(results[errorRay].positionDistance.w)
                                                        + " work=" + std::to_string(results[errorRay].statistics.z)
                                                        + " samples=" + std::to_string(results[errorRay].statistics.w));
        // Quality counts are measurements, not a claim that this preset is qualified.
        EXPECT_GT(blocked, 0u);
        EXPECT_GT(open, 0u);
    }
}

TEST_P(DistanceFieldSceneIntegrationTest, DistanceFieldShiftedObliqueNormals)
{
    RegisterDistanceFieldPrograms(device);
    const DistanceFieldSceneSettings settings = SmallFieldSettings();
    const float                      spacing  = 4.0f / (static_cast<float>(settings.cellsPerAxis - 2u) * 7.0f);
    for (uint32_t orientation = 0; orientation < 2; ++orientation)
    {
        for (uint32_t phase = 0; phase < 4; ++phase)
        {
            SCOPED_TRACE("orientation=" + std::to_string(orientation) + " phase=" + std::to_string(phase));
            BuiltFieldSceneFixture fixture;
            ASSERT_TRUE(fixture.Init(device));
            ASSERT_TRUE(fixture.scene->SetVoxelBounds(sg::AABB(Vec3(-2), Vec3(2))));
            Mat4 transform = glm::translate(Mat4(1), Vec3(1.0f, 0.5f, 0.25f) * (static_cast<float>(phase) * spacing / 4.0f));
            if (orientation != 0)
            {
                transform = glm::rotate(transform, glm::radians(31.0f), Vec3(0, 1, 0));
                transform = glm::rotate(transform, glm::radians(-19.0f), Vec3(0, 0, 1));
            }
            ASSERT_TRUE(fixture.scene->SetInstanceTransform(0, transform));
            ASSERT_TRUE(fixture.scene->Update());
            DistanceFieldScene field(device, settings);
            ASSERT_TRUE(CompleteBuiltField(*device, *fixture.scene, field));
            ASSERT_EQ(field.GetState(), DistanceFieldScene::State::Ready) << field.GetFailureReason();
            EXPECT_NEAR(field.GetUniform().gridMinCellSize.w / 7.0f, spacing, 1e-6f);
            const Vec3 normal = glm::normalize(Vec3(transform * Vec4(glm::normalize(Vec3(1, -0.35f, -0.2f)), 0)));
            HeapVector<BuiltFieldRay> rays;
            for (float y : {-0.25f, 0.0f, 0.25f})
            {
                for (float z : {-0.25f, 0.0f, 0.25f})
                {
                    const Vec3 surface = Vec3(transform * Vec4(0.75f + 0.35f * y + 0.2f * z, y, z, 1));
                    rays.push_back({Vec4(surface - normal, 0), Vec4(normal, 2)});
                    rays.push_back({Vec4(surface + normal, 0), Vec4(-normal, 2)});
                }
            }
            const HeapVector<BuiltFieldResult> results = TraceBuiltField(*device, field, rays);
            ASSERT_EQ(results.size(), rays.size());
            for (uint32_t index = 0; index < rays.size(); ++index)
            {
                SCOPED_TRACE(index);
                EXPECT_EQ(results[index].statistics.x, 1u);
                EXPECT_NEAR(results[index].positionDistance.w, 1.0f, spacing);
                EXPECT_LT(glm::dot(Vec3(results[index].normal), Vec3(rays[index].directionMaximum)), -0.8f);
                EXPECT_LE(results[index].statistics.z, field.GetUniform().dimensions.w);
                EXPECT_GE(results[index].statistics.w, 5u);
            }
        }
    }
}

// Enable explicitly after the partial-coverage builder stages are registered. A failed or
// unavailable generation is a failed gate, never a successful transparent result.
TEST_P(DistanceFieldSceneIntegrationTest, DISABLED_DistanceFieldPartialVertexAlphaExperiment)
{
    RegisterDistanceFieldPrograms(device);
    for (uint32_t pose = 0; pose < 4; ++pose)
    {
        SCOPED_TRACE(pose);
        BuiltFieldSceneFixture fixture;
        ASSERT_TRUE(fixture.Init(device, false, Vec3(1, 0, 0), true));
        ASSERT_TRUE(fixture.scene->SetVoxelBounds(sg::AABB(Vec3(-2), Vec3(2))));
        Mat4 transform(1);
        if ((pose & 1u) != 0)
        {
            transform = glm::rotate(transform, glm::radians(29.0f), Vec3(0, 1, 0));
            transform = glm::rotate(transform, glm::radians(13.0f), Vec3(0, 0, 1));
        }
        if ((pose & 2u) != 0)
        {
            transform = glm::translate(Mat4(1), Vec3(0.013f, 0.007f, -0.019f)) * transform;
        }
        HeapVector<OracleTriangle> triangles;
        for (uint32_t index = 0; index < fixture.indices.size(); index += 3u)
        {
            const asset::Vertex& a = fixture.vertices[fixture.indices[index]];
            const asset::Vertex& b = fixture.vertices[fixture.indices[index + 1u]];
            const asset::Vertex& c = fixture.vertices[fixture.indices[index + 2u]];
            OracleTriangle       triangle{};
            triangle.a     = glm::dvec3(transform * a.pos);
            triangle.b     = glm::dvec3(transform * b.pos);
            triangle.c     = glm::dvec3(transform * c.pos);
            triangle.alpha = Vec3(a.color.a, b.color.a, c.color.a);
            triangles.push_back(triangle);
        }
        ASSERT_TRUE(fixture.scene->SetInstanceTransform(0, transform));
        ASSERT_TRUE(fixture.scene->Update());
        DistanceFieldSceneSettings settings = SmallFieldSettings();
        settings.triangleBatchSize          = 256;
        settings.cellBatchSize              = 256;
        settings.dispatchesPerFrame         = 16;
        settings.referencesPerCell          = 1024;
        settings.referenceCapacity          = 1u << 20;
        DistanceFieldScene field(device, settings);
        ASSERT_TRUE(CompleteBuiltField(*device, *fixture.scene, field, 4096));
        ASSERT_EQ(field.GetState(), DistanceFieldScene::State::Ready) << field.GetFailureReason();
        EXPECT_NEAR(field.GetUniform().gridMinCellSize.w / 7.0f, 4.0f / (static_cast<float>(settings.cellsPerAxis - 2u) * 7.0f),
                    1e-6f);
        HeapVector<BuiltFieldRay> rays;
        HeapVector<float>         coverage;
        const Vec3                normal = glm::normalize(Vec3(transform * Vec4(1, 0, 0, 0)));
        for (uint32_t y = 0; y < 17; ++y)
        {
            for (uint32_t z = 0; z < 17; ++z)
            {
                const float localY = -0.5f + static_cast<float>(y) / 16.0f;
                const float localZ = -0.5f + static_cast<float>(z) / 16.0f;
                const Vec3  point  = Vec3(transform * Vec4(0.75f, localY, localZ, 1));
                const float alpha  = 1.0f - std::max((localY + 0.6f) / 1.2f, (localZ + 0.6f) / 1.2f);
                rays.push_back({Vec4(point - normal * 0.3f, 0), Vec4(normal, 0.6f)});
                rays.push_back({Vec4(point + normal * 0.3f, 0), Vec4(-normal, 0.6f)});
                coverage.push_back(alpha);
                coverage.push_back(alpha);
            }
        }
        const HeapVector<BuiltFieldResult> results = TraceBuiltField(*device, field, rays);
        ASSERT_EQ(results.size(), rays.size());
        uint32_t blocked = 0, open = 0, leaks = 0, overblocks = 0, unavailable = 0;
        for (uint32_t index = 0; index < rays.size(); ++index)
        {
            const bool     expectedHit  = std::isfinite(FieldTriangleIntersection(rays[index], triangles, 0.5));
            const uint32_t status       = results[index].statistics.x;
            blocked                    += expectedHit ? 1u : 0u;
            open                       += expectedHit ? 0u : 1u;
            leaks                      += expectedHit && status == 0u ? 1u : 0u;
            overblocks                 += !expectedHit && status == 1u ? 1u : 0u;
            unavailable                += status == 2u ? 1u : 0u;
            // Keep dense edge measurements, while asserting the independent clear/opaque controls.
            if (coverage[index] >= 0.65f || coverage[index] <= 0.35f)
            {
                EXPECT_EQ(status, expectedHit ? 1u : 0u) << "ray=" << index << " coverage=" << coverage[index];
            }
        }
        RecordProperty("vertex_alpha_pose_" + std::to_string(pose),
                       "blocked=" + std::to_string(blocked) + " open=" + std::to_string(open)
                           + " leaks=" + std::to_string(leaks) + " overblocks=" + std::to_string(overblocks) + " unavailable="
                           + std::to_string(unavailable) + " graphs=" + std::to_string(field.GetSubmittedGraphCount())
                           + " spacing=" + std::to_string(field.GetUniform().gridMinCellSize.w / 7.0f));
        EXPECT_EQ(unavailable, 0u);
    }
}

int FieldMaskCoordinate(int coordinate, uint32_t address)
{
    int result = std::clamp(coordinate, 0, 7);
    if (address == 0u)
    {
        result = (coordinate % 8 + 8) % 8;
    }
    else if (address == 1u)
    {
        const int mirrored = (coordinate % 16 + 16) % 16;
        result             = mirrored < 8 ? mirrored : 15 - mirrored;
    }
    return result;
}

double FieldMaskTexel(int x, int y, uint32_t address)
{
    const int column = FieldMaskCoordinate(x, address);
    const int row    = FieldMaskCoordinate(y, address);
    return column >= 2 && column <= 5 && row >= 2 && row <= 5 ? 0.0 : 1.0;
}

double FieldMaskAlpha(const glm::dvec2& uv, bool linear, uint32_t address)
{
    double alpha = FieldMaskTexel(static_cast<int>(std::floor(uv.x * 8)), static_cast<int>(std::floor(uv.y * 8)), address);
    if (linear)
    {
        const glm::dvec2 texel = uv * 8.0 - 0.5;
        const int        x     = static_cast<int>(std::floor(texel.x));
        const int        y     = static_cast<int>(std::floor(texel.y));
        const double     u     = texel.x - x;
        const double     v     = texel.y - y;
        alpha = (1.0 - v) * ((1.0 - u) * FieldMaskTexel(x, y, address) + u * FieldMaskTexel(x + 1, y, address))
              + v * ((1.0 - u) * FieldMaskTexel(x, y + 1, address) + u * FieldMaskTexel(x + 1, y + 1, address));
    }
    return alpha;
}

// These numerical cases may be too uncertain to construct within the coverage budget.
// An unresolved region or explicit coverage failure is safe; publishing an empty field is not.
TEST_P(DistanceFieldSceneIntegrationTest, DistanceFieldCoverageArithmeticRegression)
{
    ShaderProgramManager::GetInstance().BuildShaderPrograms(device);
    device->InitializeRendererServer();
    const Vec2 barycentrics[] = {Vec2(0.3626762926578522f, 0.3864086866378784f),
                                 Vec2(0.23923003673553467f, 0.028369784355163574f)};
    for (uint32_t testCase = 0; testCase < 2; ++testCase)
    {
        SCOPED_TRACE(testCase == 0 ? "constant alpha immediately below cutoff" : "raw UV / affine cancellation");
        const float            alpha = testCase == 0 ? std::nextafter(0.5f, 0.0f) : 1.0f;
        BuiltFieldSceneFixture fixture;
        ASSERT_TRUE(fixture.Init(device, false, Vec3(alpha), true));
        ASSERT_TRUE(fixture.scene->SetVoxelBounds(sg::AABB(Vec3(-2), Vec3(2))));
        if (testCase != 0)
        {
            UniquePtr<sg::Texture> texture = MakeUnique<sg::Texture>("UV cancellation opaque witness texel");
            texture->width                 = 8;
            texture->height                = 1;
            texture->samplerIndex          = 0;
            texture->format                = asset::Format::R8G8B8A8_UNORM;
            texture->bytesData.resize(8u * 4u);
            for (uint32_t x = 0; x < 8; ++x)
            {
                texture->bytesData[4u * x] = texture->bytesData[4u * x + 1u] = texture->bytesData[4u * x + 2u] = 255;
                texture->bytesData[4u * x + 3u] = x == 1u ? 255 : 0;
            }
            UniquePtr<sg::Sampler> sampler = MakeUnique<sg::Sampler>("nearest cancellation witness sampler");
            sampler->magFilter             = asset::TextureFilter::Nearest;
            sampler->minFilter             = asset::TextureFilter::Linear;
            sampler->useMipmaps            = false;
            sampler->wrapS = sampler->wrapT = asset::SamplerAddressMode::Repeat;
            fixture.source.AddComponent(std::move(texture));
            fixture.source.AddComponent(std::move(sampler));
            fixture.scene->LoadSceneTextures();
            for (asset::Vertex& vertex : fixture.vertices)
            {
                vertex.uv0 = Vec2(2000000.0f, 0.5f);
            }
            ASSERT_TRUE(fixture.scene->UpdateVertices(0, MakeVecView(fixture.vertices)));
            sg::MaterialData material          = fixture.scene->GetMaterialsData()[0];
            material.bcTexIndex                = 0;
            material.bcTexSet                  = 0;
            material.textureTransforms[0].row0 = Vec4(1, 0, -2000000.0f, 1);
            material.textureTransforms[0].row1 = Vec4(0, 1, 0, 0);
            ASSERT_TRUE(fixture.scene->UpdateMaterial(0, material));
        }
        ASSERT_TRUE(fixture.scene->Update());

        // Replay one permitted float32 accumulation at the known covered barycentric.
        // The standalone CPU regression additionally checks separated operations.
        const Vec2     barycentric = barycentrics[testCase];
        volatile float firstWeight = 1.0f - barycentric.x;
        const Vec3     weights(firstWeight - barycentric.y, barycentric.x, barycentric.y);
        const float    value    = testCase == 0 ? alpha : 2000000.0f;
        float          weighted = 0.0f;
        for (uint32_t corner = 0; corner < 3; ++corner)
        {
            weighted = std::fma(weights[corner], value, weighted);
        }
        if (testCase == 0)
        {
            ASSERT_LT(alpha, 0.5f);
            ASSERT_GE(weighted, 0.5f);
        }
        else
        {
            ASSERT_EQ(weighted - 2000000.0f, 0.125f); // Nearest texel 1 is opaque; corner texel 0 is empty.
        }
        const Vec3                 point    = Vec3(fixture.vertices[fixture.indices[0]].pos) * weights.x
                                            + Vec3(fixture.vertices[fixture.indices[1]].pos) * weights.y
                                            + Vec3(fixture.vertices[fixture.indices[2]].pos) * weights.z;
        DistanceFieldSceneSettings settings = SmallFieldSettings();
        settings.triangleBatchSize          = 256;
        settings.cellBatchSize              = 256;
        settings.dispatchesPerFrame         = 16;
        settings.referencesPerCell          = 1024;
        settings.referenceCapacity          = 1u << 20;
        DistanceFieldScene field(device, settings);
        ASSERT_TRUE(CompleteBuiltField(*device, *fixture.scene, field, 4096));
        const uint32_t flags = field.GetFailureDetails().flags;
        RecordProperty("coverage_arithmetic_" + std::to_string(testCase),
                       "state=" + std::to_string(static_cast<uint32_t>(field.GetState()))
                           + " failure_flags=" + std::to_string(flags) + " reason=" + field.GetFailureReason()
                           + " witness=" + std::to_string(testCase == 0 ? weighted : weighted - 2000000.0f)
                           + " graphs=" + std::to_string(field.GetSubmittedGraphCount()));
        if (field.GetState() == DistanceFieldScene::State::Ready)
        {
            HeapVector<BuiltFieldRay> rays;
            rays.push_back({Vec4(point - Vec3(0.3f, 0, 0), 0), Vec4(1, 0, 0, 0.6f)});
            rays.push_back({Vec4(point + Vec3(0.3f, 0, 0), 0), Vec4(-1, 0, 0, 0.6f)});
            const HeapVector<BuiltFieldResult> results = TraceBuiltField(*device, field, rays);
            ASSERT_EQ(results.size(), rays.size());
            for (const BuiltFieldResult& result : results)
            {
                EXPECT_NE(result.statistics.x, 0u);
                if (result.statistics.x == 2u)
                {
                    EXPECT_EQ(result.normal.w, 3.0f); // Missing coverage, not an iteration limit.
                    EXPECT_GT(field.GetUnavailableCellCount(), 0u);
                }
            }
        }
        else
        {
            EXPECT_EQ(field.GetState(), DistanceFieldScene::State::Failed);
            const uint32_t coverageFlags = ZEN_FIELD_FAILURE_COVERAGE | ZEN_FIELD_FAILURE_COVERAGE_BUDGET;
            EXPECT_NE(flags & coverageFlags, 0u) << field.GetFailureReason();
            EXPECT_EQ(flags & ~coverageFlags, 0u) << field.GetFailureReason();
        }
    }
}

TEST_P(DistanceFieldSceneIntegrationTest, DISABLED_DistanceFieldTextureMaskExperiment)
{
    ShaderProgramManager::GetInstance().BuildShaderPrograms(device);
    device->InitializeRendererServer();
    for (uint32_t filter = 0; filter < 2; ++filter)
    {
        for (uint32_t address = 0; address < 3; ++address)
        {
            SCOPED_TRACE("filter=" + std::to_string(filter) + " address=" + std::to_string(address));
            // Each fixture replaces the scene's explicit bindless slots, including
            // its sampler. Retire their users and reset registrations at that boundary.
            ASSERT_TRUE(device->PrepareForSceneReplacement());
            BuiltFieldSceneFixture fixture;
            ASSERT_TRUE(fixture.Init(device, false, Vec3(1), true));
            UniquePtr<sg::Texture> texture = MakeUnique<sg::Texture>("eight by eight opaque card with cutout");
            texture->width = texture->height = 8;
            texture->samplerIndex            = 0;
            texture->format                  = asset::Format::R8G8B8A8_UNORM;
            texture->bytesData.resize(8u * 8u * 4u);
            for (uint32_t y = 0; y < 8; ++y)
            {
                for (uint32_t x = 0; x < 8; ++x)
                {
                    const uint32_t offset      = (x + 8u * y) * 4u;
                    texture->bytesData[offset] = texture->bytesData[offset + 1u] = texture->bytesData[offset + 2u] = 255;
                    texture->bytesData[offset + 3u] = FieldMaskTexel(x, y, 2) == 0 ? 0 : 255;
                }
            }
            UniquePtr<sg::Sampler> sampler = MakeUnique<sg::Sampler>("authored cutout sampler");
            sampler->magFilter             = filter == 0 ? asset::TextureFilter::Nearest : asset::TextureFilter::Linear;
            // Deliberately different min/mag: textureLod(...,0) must use the verified mag-filter contract.
            sampler->minFilter  = filter == 0 ? asset::TextureFilter::Linear : asset::TextureFilter::Nearest;
            sampler->useMipmaps = false;
            sampler->wrapS = sampler->wrapT = static_cast<asset::SamplerAddressMode>(address);
            fixture.source.AddComponent(std::move(texture));
            fixture.source.AddComponent(std::move(sampler));
            fixture.scene->LoadSceneTextures();
            for (asset::Vertex& vertex : fixture.vertices)
            {
                vertex.uv0 = Vec2((vertex.pos.y + 0.6f) / 1.2f, (vertex.pos.z + 0.6f) / 1.2f);
            }
            ASSERT_TRUE(fixture.scene->UpdateVertices(0, MakeVecView(fixture.vertices)));
            sg::MaterialData material          = fixture.scene->GetMaterialsData()[0];
            material.bcTexIndex                = 0;
            material.bcTexSet                  = 0;
            const float angle                  = glm::radians(20.0f);
            material.textureTransforms[0].row0 = Vec4(2.25f * std::cos(angle), -1.75f * std::sin(angle), -0.35f, 1);
            material.textureTransforms[0].row1 = Vec4(2.25f * std::sin(angle), 1.75f * std::cos(angle), -0.4f, 0);
            ASSERT_TRUE(fixture.scene->UpdateMaterial(0, material));
            Mat4 transform = glm::translate(Mat4(1), Vec3(0.013f, 0.007f, -0.019f));
            transform      = glm::rotate(transform, glm::radians(29.0f), Vec3(0, 1, 0));
            transform      = glm::rotate(transform, glm::radians(13.0f), Vec3(0, 0, 1));
            ASSERT_TRUE(fixture.scene->SetInstanceTransform(0, transform));
            ASSERT_TRUE(fixture.scene->SetVoxelBounds(sg::AABB(Vec3(-2), Vec3(2))));
            ASSERT_TRUE(fixture.scene->Update());
            HeapVector<OracleTriangle> triangles;
            for (uint32_t index = 0; index < fixture.indices.size(); index += 3u)
            {
                OracleTriangle triangle{};
                triangle.a = glm::dvec3(transform * fixture.vertices[fixture.indices[index]].pos);
                triangle.b = glm::dvec3(transform * fixture.vertices[fixture.indices[index + 1u]].pos);
                triangle.c = glm::dvec3(transform * fixture.vertices[fixture.indices[index + 2u]].pos);
                triangles.push_back(triangle);
            }
            HeapVector<BuiltFieldRay> rays;
            HeapVector<double>        coverage;
            HeapVector<bool>          controls;
            const Vec3                normal = glm::normalize(Vec3(transform * Vec4(1, 0, 0, 0)));
            for (uint32_t y = 0; y < 33; ++y)
            {
                for (uint32_t z = 0; z < 33; ++z)
                {
                    const float      localY = -0.51f + static_cast<float>(y) * 1.02f / 32.0f;
                    const float      localZ = -0.51f + static_cast<float>(z) * 1.02f / 32.0f;
                    const glm::dvec3 uv((localY + 0.6f) / 1.2f, (localZ + 0.6f) / 1.2f, 1);
                    const glm::dvec2 transformed(glm::dot(glm::dvec3(material.textureTransforms[0].row0), uv),
                                                 glm::dot(glm::dvec3(material.textureTransforms[0].row1), uv));
                    const double     alpha    = FieldMaskAlpha(transformed, filter != 0, address);
                    const glm::dvec2 fraction = glm::fract(transformed * 8.0);
                    const bool       core     = (alpha <= 0.05 || alpha >= 0.95) && fraction.x > 0.3 && fraction.x < 0.7
                                             && fraction.y > 0.3 && fraction.y < 0.7;
                    const Vec3       point    = Vec3(transform * Vec4(0.75f, localY, localZ, 1));
                    rays.push_back({Vec4(point - normal * 0.3f, 0), Vec4(normal, 0.6f)});
                    rays.push_back({Vec4(point + normal * 0.3f, 0), Vec4(-normal, 0.6f)});
                    coverage.push_back(alpha);
                    coverage.push_back(alpha);
                    controls.push_back(core);
                    controls.push_back(core);
                }
            }
            DistanceFieldSceneSettings settings;
            settings.cellsPerAxis       = 32;
            settings.cellBatchSize      = 256;
            settings.dispatchesPerFrame = 16;
            settings.referencesPerCell  = 1024;
            DistanceFieldScene field(device, settings);
            ASSERT_TRUE(CompleteBuiltField(*device, *fixture.scene, field, 4096));
            ASSERT_EQ(field.GetState(), DistanceFieldScene::State::Ready) << field.GetFailureReason();
            EXPECT_NEAR(field.GetUniform().gridMinCellSize.w / 7.0f,
                        4.0f / (static_cast<float>(settings.cellsPerAxis - 2u) * 7.0f), 1e-6f);
            const HeapVector<BuiltFieldResult> results = TraceBuiltField(*device, field, rays);
            ASSERT_EQ(results.size(), rays.size());
            uint32_t blocked = 0, open = 0, leaks = 0, overblocks = 0, unavailable = 0, coreCount = 0;
            for (uint32_t index = 0; index < rays.size(); ++index)
            {
                const bool expectedHit =
                    std::isfinite(FieldTriangleIntersection(rays[index], triangles)) && coverage[index] >= 0.5;
                const uint32_t status  = results[index].statistics.x;
                blocked               += expectedHit ? 1u : 0u;
                open                  += expectedHit ? 0u : 1u;
                leaks                 += expectedHit && status == 0u ? 1u : 0u;
                overblocks            += !expectedHit && status == 1u ? 1u : 0u;
                unavailable           += status == 2u ? 1u : 0u;
                if (controls[index])
                {
                    ++coreCount;
                    EXPECT_EQ(status, expectedHit ? 1u : 0u) << "ray=" << index << " coverage=" << coverage[index];
                }
            }
            RecordProperty("texture_mask_" + std::to_string(filter) + "_" + std::to_string(address),
                           "blocked=" + std::to_string(blocked) + " open=" + std::to_string(open)
                               + " leaks=" + std::to_string(leaks) + " overblocks=" + std::to_string(overblocks)
                               + " unavailable=" + std::to_string(unavailable) + " controls=" + std::to_string(coreCount)
                               + " graphs=" + std::to_string(field.GetSubmittedGraphCount())
                               + " spacing=" + std::to_string(field.GetUniform().gridMinCellSize.w / 7.0f));
            EXPECT_GT(coreCount, 0u);
            EXPECT_EQ(unavailable, 0u);
        }
    }
}
