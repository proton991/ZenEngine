class ForwardBlendDrawCounters
{
public:
    ForwardBlendDrawCounters() : m_previous(RHIOptions::GetInstance().ExecutionCountersEnabled())
    {
        RHIOptions::GetInstance().SetExecutionCountersEnabled(true);
    }

    ~ForwardBlendDrawCounters()
    {
        RHIOptions::GetInstance().SetExecutionCountersEnabled(m_previous);
    }

private:
    bool m_previous;
};

struct ForwardBlendSceneFixture
{
    RenderDevice&                   device;
    sg::Scene                       source;
    HeapVector<UniquePtr<sg::Node>> nodes;
    HeapVector<asset::Vertex>       vertices;
    HeapVector<uint32_t>            indices;
    sg::Camera                      camera{Vec3(0), Vec3(0, 0, -1), 1.0f, 60.0f, .1f, 10.0f};
    UniquePtr<RenderScene>          scene;

    explicit ForwardBlendSceneFixture(RenderDevice& renderDevice) : device(renderDevice) {}

    ~ForwardBlendSceneFixture()
    {
        device.WaitForPreviousFrames();
        device.GetRendererServer()->SetRenderScene(nullptr);
        if (scene)
        {
            scene->Destroy();
        }
    }

    bool Init(uint32_t scenario, bool withVariant = false, uint32_t uvSetCount = 2, bool opaqueBackdrop = false)
    {
        // Depth order alternates between nodes A/B/A/B. Vertex colors make any
        // change to that order visible even though every triangle shares one material.
        const Vec4 colors[]  = {Vec4(1, 0, 0, .5f), Vec4(0, 1, 0, .5f), Vec4(0, 0, 1, .5f), Vec4(1, 1, 0, .5f)};
        const Vec2 corners[] = {Vec2(-.75f, -.75f), Vec2(.75f, -.75f), Vec2(0, .75f)};
        for (uint32_t triangle = 0; triangle < 4; ++triangle)
        {
            for (const Vec2& corner : corners)
            {
                asset::Vertex vertex{};
                vertex.pos = Vec4(corner, -4.0f + .5f * triangle, 1);
                if (scenario == 0 && (triangle & 1u) != 0)
                {
                    // Countertranslate B's local positions: this reference has
                    // different node data, but exactly the same world positions.
                    vertex.pos.x -= 2.0f;
                }
                vertex.normal  = Vec4(0, 0, 1, 0);
                vertex.tangent = Vec4(1, 0, 0, 1);
                vertex.color   = colors[triangle];
                vertex.uv0     = Vec2(.125f, .5f);
                vertex.uv1     = corner * .5f + Vec2(.5f);
                vertices.push_back(vertex);
            }
        }
        UniquePtr<sg::Material> material    = MakeUnique<sg::Material>("forward blend parity");
        material->index                     = 0;
        material->data.surfaceProperties.y  = static_cast<float>(sg::AlphaMode::Blend);
        material->data.materialProperties.y = 1.0f;
        material->data.materialProperties.z = 1.0f;
        sg::Material* materialPointer       = material.Get();
        source.AddComponent(std::move(material));
        if (withVariant)
        {
            UniquePtr<sg::Material> variant = MakeUnique<sg::Material>("opaque variant");
            variant->index                  = 1;
            source.AddComponent(std::move(variant));
            source.GetAssetData().materialVariants.push_back("opaque");
        }
        for (uint32_t instance = 0; instance < 2; ++instance)
        {
            const uint32_t firstIndex = static_cast<uint32_t>(indices.size());
            sg::AABB       bounds;
            for (uint32_t triangle = instance; triangle < 4; triangle += 2)
            {
                for (uint32_t corner = 0; corner < 3; ++corner)
                {
                    const uint32_t vertex = triangle * 3 + corner;
                    indices.push_back(vertex);
                    bounds.SetMin(Vec3(vertices[vertex].pos));
                    bounds.SetMax(Vec3(vertices[vertex].pos));
                }
            }
            UniquePtr<sg::SubMesh> primitive = MakeUnique<sg::SubMesh>("interleaved blend triangles", firstIndex, 6, 6);
            primitive->SetMaterial(0, materialPointer);
            primitive->SetBounds(bounds);
            if (withVariant)
            {
                primitive->assetMesh      = instance;
                primitive->assetPrimitive = 0;
                source.GetAssetData().variantPrimitives.push_back({instance, 0, {1}});
            }
            UniquePtr<sg::Mesh> mesh = MakeUnique<sg::Mesh>("forward blend mesh");
            mesh->AddSubMesh(primitive.Get());
            mesh->SetAABB(bounds.GetMin(), bounds.GetMax());
            UniquePtr<sg::Node> node = MakeUnique<sg::Node>(instance, "forward blend node");
            node->AddComponent(mesh.Get());
            const float translation = instance == 0 || scenario == 1 ? 0.0f : scenario == 0 ? 2.0f : .5f;
            node->SetData(instance, glm::translate(Mat4(1), Vec3(translation, 0, 0)));
            source.AddRenderableNode(node.Get());
            nodes.push_back(std::move(node));
            source.AddComponent(std::move(mesh));
            source.AddComponent(std::move(primitive));
        }
        if (opaqueBackdrop)
        {
            const uint32_t firstIndex = static_cast<uint32_t>(indices.size());
            for (const Vec2& corner : corners)
            {
                asset::Vertex vertex{};
                vertex.pos     = Vec4(corner * 2.0f, -4.5f, 1);
                vertex.normal  = Vec4(0, 0, 1, 0);
                vertex.tangent = Vec4(1, 0, 0, 1);
                vertex.color   = Vec4(1);
                indices.push_back(static_cast<uint32_t>(vertices.size()));
                vertices.push_back(vertex);
            }
            const uint32_t          materialIndex = withVariant ? 2u : 1u;
            UniquePtr<sg::Material> backdrop      = MakeUnique<sg::Material>("opaque receiver behind blend layers");
            backdrop->index                       = materialIndex;
            backdrop->data.baseColorFactor        = Vec4(.25f, .25f, .25f, 1);
            backdrop->data.metallicFactor         = 0;
            backdrop->data.materialProperties.z   = 1;
            const sg::AABB         bounds(Vec3(-1.5f, -1.5f, -4.5f), Vec3(1.5f, 1.5f, -4.5f));
            UniquePtr<sg::SubMesh> primitive = MakeUnique<sg::SubMesh>("opaque backdrop", firstIndex, 3, 3);
            primitive->SetMaterial(materialIndex, backdrop.Get());
            primitive->SetBounds(bounds);
            UniquePtr<sg::Mesh> mesh = MakeUnique<sg::Mesh>("opaque backdrop");
            mesh->AddSubMesh(primitive.Get());
            mesh->SetAABB(bounds.GetMin(), bounds.GetMax());
            UniquePtr<sg::Node> node = MakeUnique<sg::Node>(2, "opaque backdrop");
            node->SetData(2, Mat4(1));
            node->AddComponent(mesh.Get());
            source.AddRenderableNode(node.Get());
            nodes.push_back(std::move(node));
            source.AddComponent(std::move(mesh));
            source.AddComponent(std::move(primitive));
            source.AddComponent(std::move(backdrop));
        }
        if (uvSetCount > 2)
        {
            for (const asset::Vertex& vertex : vertices)
            {
                source.GetAssetData().vertexTexCoords.push_back({vertex.uv0, vertex.uv1, vertex.uv1});
            }
        }
        SceneData data{};
        data.pScene      = &source;
        data.pCamera     = &camera;
        data.pVertices   = vertices.data();
        data.numVertices = static_cast<uint32_t>(vertices.size());
        data.pIndices    = indices.data();
        data.numIndices  = static_cast<uint32_t>(indices.size());
        scene            = MakeUnique<RenderScene>(&device, data);
        const bool valid = scene->Init() && scene->SetEnvironmentLighting(0, 0, false, false);
        if (valid)
        {
            device.GetRendererServer()->SetRenderScene(scene.Get());
        }
        return valid;
    }
};

void ReadForwardBlendPixels(RenderDevice& device, RHITexture* color, RHIBuffer* packed, RHIBuffer* readback)
{
    RenderGraph graph("ForwardBlendParityReadback");
    ASSERT_TRUE(graph.Begin());
    RDGComputePassDesc pass;
    pass.SetShaderProgramName("CaptureFrameSP");
    pass.BindSampledTexture("sourceColor", device.CreateSampler(RHISamplerCreateInfo::CreateLinearRepeat()),
                            color->GetDefaultView());
    pass.BindStorageBuffer("packedColor", packed, RDGContentGuarantee::eFullWrite);
    const glm::uvec2 groups((color->GetWidth() + 7) / 8, (color->GetHeight() + 7) / 8);
    graph.AddComputePass(std::move(pass)).RecordPassCommands([groups](RDGPassCmdEncoder& encoder) {
        encoder.Dispatch(groups.x, groups.y, 1);
    });
    graph.AddTransferPass("ReadForwardBlendPixels").CopyBuffer(packed, readback, {0, 0, packed->GetRequiredSize()}).NeverCull();
    ASSERT_TRUE(graph.End()) << graph.GetResult().message;
    ASSERT_TRUE(device.ExecuteRenderGraph(graph)) << graph.GetResult().message;
    device.WaitForPreviousFrames();
}
