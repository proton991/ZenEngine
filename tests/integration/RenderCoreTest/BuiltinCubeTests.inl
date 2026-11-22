TEST_F(RenderCoreTest, BuiltinVoxelCubeHasUnitBoundsAndOutwardTriangles)
{
    RenderObject cube(device);
    ASSERT_TRUE(cube.IsValid());
    EXPECT_EQ(cube.GetAABB().GetMin(), Vec3(-0.5f));
    EXPECT_EQ(cube.GetAABB().GetMax(), Vec3(0.5f));
    TestBuffer* vertices = static_cast<TestBuffer*>(cube.GetVertexBuffer());
    TestBuffer* indices  = static_cast<TestBuffer*>(cube.GetIndexBuffer());
    ASSERT_EQ(vertices->bytes.size(), 24 * sizeof(asset::Vertex));
    ASSERT_EQ(indices->bytes.size(), 36 * sizeof(uint32_t));
    device->NextFrame();
    asset::Vertex geometry[24]{};
    uint32_t      corners[36]{};
    std::memcpy(geometry, vertices->bytes.data(), sizeof(geometry));
    std::memcpy(corners, indices->bytes.data(), sizeof(corners));
    for (const asset::Vertex& vertex : geometry)
    {
        EXPECT_FLOAT_EQ(std::abs(vertex.pos.x), 0.5f);
        EXPECT_FLOAT_EQ(std::abs(vertex.pos.y), 0.5f);
        EXPECT_FLOAT_EQ(std::abs(vertex.pos.z), 0.5f);
        EXPECT_FLOAT_EQ(vertex.pos.w, 1.0f);
        EXPECT_FLOAT_EQ(vertex.normal.w, 0.0f);
        EXPECT_FLOAT_EQ(glm::length(Vec3(vertex.normal)), 1.0f);
    }
    for (uint32_t triangle = 0; triangle < 12; ++triangle)
    {
        const uint32_t a = corners[triangle * 3];
        const uint32_t b = corners[triangle * 3 + 1];
        const uint32_t c = corners[triangle * 3 + 2];
        ASSERT_LT(a, 24u);
        ASSERT_LT(b, 24u);
        ASSERT_LT(c, 24u);
        const Vec3 face = glm::cross(Vec3(geometry[b].pos - geometry[a].pos), Vec3(geometry[c].pos - geometry[a].pos));
        EXPECT_FLOAT_EQ(glm::length(face), 1.0f);
        EXPECT_GT(glm::dot(face, Vec3(geometry[a].pos)), 0.0f);
        EXPECT_EQ(face, Vec3(geometry[a].normal));
        EXPECT_EQ(geometry[a].normal, geometry[b].normal);
        EXPECT_EQ(geometry[a].normal, geometry[c].normal);
    }
}

TEST_F(RenderCoreTest, BuiltinVoxelCubeRejectsPartialBufferAllocation)
{
    rhi->failBufferCreationAt = rhi->bufferCreations + 1;
    {
        RenderObject cube(device);
        EXPECT_FALSE(cube.IsValid());
        EXPECT_EQ(cube.GetVertexBuffer(), nullptr);
        EXPECT_EQ(cube.GetIndexBuffer(), nullptr);
    }
    // Warm the staging pool so the next two native allocations are cube buffers.
    const uint8_t bytes[16]{};
    RHIBuffer*    warm = device->CreateVertexBuffer(sizeof(bytes), bytes);
    ASSERT_NE(warm, nullptr);
    device->NextFrame();
    device->DestroyBuffer(warm);
    rhi->failBufferCreationAt = rhi->bufferCreations + 2;
    {
        RenderObject cube(device);
        EXPECT_FALSE(cube.IsValid());
        EXPECT_EQ(cube.GetVertexBuffer(), nullptr);
        EXPECT_EQ(cube.GetIndexBuffer(), nullptr);
    }
    rhi->failBufferCreationAt = 0;
    RenderObject retry(device);
    EXPECT_TRUE(retry.IsValid());
}
