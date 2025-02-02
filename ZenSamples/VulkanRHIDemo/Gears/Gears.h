#pragma once
#include "../Application.h"

#ifndef NUM_GEARS
#    define NUM_GEARS 3
#endif

#ifndef M_PI
#    define M_PI 3.14
#endif

// Used for passing the definition of a gear during construction
struct GearDefinition
{
    float innerRadius;
    float outerRadius;
    float width;
    int numTeeth;
    float toothDepth;
    glm::vec3 color;
    glm::vec3 pos;
    float rotSpeed;
    float rotOffset;
};
/*
 * Gear
 * This class contains the properties of a single gear and a function to generate vertices and indices
 */
class Gear
{
public:
    // Definition for the vertex data used to render the gears
    struct Vertex
    {
        glm::vec4 position;
        glm::vec3 normal;
        glm::vec3 color;
    };

    glm::vec3 color;
    glm::vec3 pos;
    float rotSpeed{0.0f};
    float rotOffset{0.0f};
    // These are used at draw time to offset into the single buffers
    uint32_t indexCount{0};
    uint32_t indexStart{0};

    // Generates the indices and vertices for this gear
    // They are added to the vertex and index buffers passed into the function
    // This way we can put all gears into single vertex and index buffers instead of having to allocate single buffers for each gear (which would be bad practice)
    void generate(GearDefinition& gearDefinition,
                  std::vector<Vertex>& vertexBuffer,
                  std::vector<uint32_t>& indexBuffer)
    {
        this->color     = gearDefinition.color;
        this->pos       = gearDefinition.pos;
        this->rotOffset = gearDefinition.rotOffset;
        this->rotSpeed  = gearDefinition.rotSpeed;

        int i;
        float r0, r1, r2;
        float ta, da;
        float u1, v1, u2, v2, len;
        float cos_ta, cos_ta_1da, cos_ta_2da, cos_ta_3da, cos_ta_4da;
        float sin_ta, sin_ta_1da, sin_ta_2da, sin_ta_3da, sin_ta_4da;
        int32_t ix0, ix1, ix2, ix3, ix4, ix5;

        // We need to know where this triangle's indices start within the single index buffer
        indexStart = static_cast<uint32_t>(indexBuffer.size());

        r0 = gearDefinition.innerRadius;
        r1 = gearDefinition.outerRadius - gearDefinition.toothDepth / 2.0f;
        r2 = gearDefinition.outerRadius + gearDefinition.toothDepth / 2.0f;
        da = static_cast<float>(2.0 * M_PI / gearDefinition.numTeeth / 4.0);

        glm::vec3 normal;

        // Use lambda functions to simplify vertex and face creation
        auto addFace = [&indexBuffer](int a, int b, int c) {
            indexBuffer.push_back(a);
            indexBuffer.push_back(b);
            indexBuffer.push_back(c);
        };

        auto addVertex = [this, &vertexBuffer](float x, float y, float z, glm::vec3 normal) {
            Vertex v{};
            v.position = {x, y, z, 1.0};
            v.normal   = normal;
            v.color    = this->color;
            vertexBuffer.push_back(v);
            return static_cast<int32_t>(vertexBuffer.size()) - 1;
        };

        for (i = 0; i < gearDefinition.numTeeth; i++)
        {
            ta = i * static_cast<float>(2.0 * M_PI / gearDefinition.numTeeth);

            cos_ta     = cos(ta);
            cos_ta_1da = cos(ta + da);
            cos_ta_2da = cos(ta + 2.0f * da);
            cos_ta_3da = cos(ta + 3.0f * da);
            cos_ta_4da = cos(ta + 4.0f * da);
            sin_ta     = sin(ta);
            sin_ta_1da = sin(ta + da);
            sin_ta_2da = sin(ta + 2.0f * da);
            sin_ta_3da = sin(ta + 3.0f * da);
            sin_ta_4da = sin(ta + 4.0f * da);

            u1  = r2 * cos_ta_1da - r1 * cos_ta;
            v1  = r2 * sin_ta_1da - r1 * sin_ta;
            len = sqrt(u1 * u1 + v1 * v1);
            u1 /= len;
            v1 /= len;
            u2 = r1 * cos_ta_3da - r2 * cos_ta_2da;
            v2 = r1 * sin_ta_3da - r2 * sin_ta_2da;

            // Front face
            normal = glm::vec3(0.0f, 0.0f, 1.0f);
            ix0    = addVertex(r0 * cos_ta, r0 * sin_ta, gearDefinition.width * 0.5f, normal);
            ix1    = addVertex(r1 * cos_ta, r1 * sin_ta, gearDefinition.width * 0.5f, normal);
            ix2    = addVertex(r0 * cos_ta, r0 * sin_ta, gearDefinition.width * 0.5f, normal);
            ix3 = addVertex(r1 * cos_ta_3da, r1 * sin_ta_3da, gearDefinition.width * 0.5f, normal);
            ix4 = addVertex(r0 * cos_ta_4da, r0 * sin_ta_4da, gearDefinition.width * 0.5f, normal);
            ix5 = addVertex(r1 * cos_ta_4da, r1 * sin_ta_4da, gearDefinition.width * 0.5f, normal);
            addFace(ix0, ix1, ix2);
            addFace(ix1, ix3, ix2);
            addFace(ix2, ix3, ix4);
            addFace(ix3, ix5, ix4);

            // Teeth front face
            normal = glm::vec3(0.0f, 0.0f, 1.0f);
            ix0    = addVertex(r1 * cos_ta, r1 * sin_ta, gearDefinition.width * 0.5f, normal);
            ix1 = addVertex(r2 * cos_ta_1da, r2 * sin_ta_1da, gearDefinition.width * 0.5f, normal);
            ix2 = addVertex(r1 * cos_ta_3da, r1 * sin_ta_3da, gearDefinition.width * 0.5f, normal);
            ix3 = addVertex(r2 * cos_ta_2da, r2 * sin_ta_2da, gearDefinition.width * 0.5f, normal);
            addFace(ix0, ix1, ix2);
            addFace(ix1, ix3, ix2);

            // Back face
            normal = glm::vec3(0.0f, 0.0f, -1.0f);
            ix0    = addVertex(r1 * cos_ta, r1 * sin_ta, -gearDefinition.width * 0.5f, normal);
            ix1    = addVertex(r0 * cos_ta, r0 * sin_ta, -gearDefinition.width * 0.5f, normal);
            ix2 = addVertex(r1 * cos_ta_3da, r1 * sin_ta_3da, -gearDefinition.width * 0.5f, normal);
            ix3 = addVertex(r0 * cos_ta, r0 * sin_ta, -gearDefinition.width * 0.5f, normal);
            ix4 = addVertex(r1 * cos_ta_4da, r1 * sin_ta_4da, -gearDefinition.width * 0.5f, normal);
            ix5 = addVertex(r0 * cos_ta_4da, r0 * sin_ta_4da, -gearDefinition.width * 0.5f, normal);
            addFace(ix0, ix1, ix2);
            addFace(ix1, ix3, ix2);
            addFace(ix2, ix3, ix4);
            addFace(ix3, ix5, ix4);

            // Teeth back face
            normal = glm::vec3(0.0f, 0.0f, -1.0f);
            ix0 = addVertex(r1 * cos_ta_3da, r1 * sin_ta_3da, -gearDefinition.width * 0.5f, normal);
            ix1 = addVertex(r2 * cos_ta_2da, r2 * sin_ta_2da, -gearDefinition.width * 0.5f, normal);
            ix2 = addVertex(r1 * cos_ta, r1 * sin_ta, -gearDefinition.width * 0.5f, normal);
            ix3 = addVertex(r2 * cos_ta_1da, r2 * sin_ta_1da, -gearDefinition.width * 0.5f, normal);
            addFace(ix0, ix1, ix2);
            addFace(ix1, ix3, ix2);

            // Outard teeth faces
            normal = glm::vec3(v1, -u1, 0.0f);
            ix0    = addVertex(r1 * cos_ta, r1 * sin_ta, gearDefinition.width * 0.5f, normal);
            ix1    = addVertex(r1 * cos_ta, r1 * sin_ta, -gearDefinition.width * 0.5f, normal);
            ix2 = addVertex(r2 * cos_ta_1da, r2 * sin_ta_1da, gearDefinition.width * 0.5f, normal);
            ix3 = addVertex(r2 * cos_ta_1da, r2 * sin_ta_1da, -gearDefinition.width * 0.5f, normal);
            addFace(ix0, ix1, ix2);
            addFace(ix1, ix3, ix2);

            normal = glm::vec3(cos_ta, sin_ta, 0.0f);
            ix0 = addVertex(r2 * cos_ta_1da, r2 * sin_ta_1da, gearDefinition.width * 0.5f, normal);
            ix1 = addVertex(r2 * cos_ta_1da, r2 * sin_ta_1da, -gearDefinition.width * 0.5f, normal);
            ix2 = addVertex(r2 * cos_ta_2da, r2 * sin_ta_2da, gearDefinition.width * 0.5f, normal);
            ix3 = addVertex(r2 * cos_ta_2da, r2 * sin_ta_2da, -gearDefinition.width * 0.5f, normal);
            addFace(ix0, ix1, ix2);
            addFace(ix1, ix3, ix2);

            normal = glm::vec3(v2, -u2, 0.0f);
            ix0 = addVertex(r2 * cos_ta_2da, r2 * sin_ta_2da, gearDefinition.width * 0.5f, normal);
            ix1 = addVertex(r2 * cos_ta_2da, r2 * sin_ta_2da, -gearDefinition.width * 0.5f, normal);
            ix2 = addVertex(r1 * cos_ta_3da, r1 * sin_ta_3da, gearDefinition.width * 0.5f, normal);
            ix3 = addVertex(r1 * cos_ta_3da, r1 * sin_ta_3da, -gearDefinition.width * 0.5f, normal);
            addFace(ix0, ix1, ix2);
            addFace(ix1, ix3, ix2);

            normal = glm::vec3(cos_ta, sin_ta, 0.0f);
            ix0 = addVertex(r1 * cos_ta_3da, r1 * sin_ta_3da, gearDefinition.width * 0.5f, normal);
            ix1 = addVertex(r1 * cos_ta_3da, r1 * sin_ta_3da, -gearDefinition.width * 0.5f, normal);
            ix2 = addVertex(r1 * cos_ta_4da, r1 * sin_ta_4da, gearDefinition.width * 0.5f, normal);
            ix3 = addVertex(r1 * cos_ta_4da, r1 * sin_ta_4da, -gearDefinition.width * 0.5f, normal);
            addFace(ix0, ix1, ix2);
            addFace(ix1, ix3, ix2);

            // Inside cylinder faces
            ix0 = addVertex(r0 * cos_ta, r0 * sin_ta, -gearDefinition.width * 0.5f,
                            glm::vec3(-cos_ta, -sin_ta, 0.0f));
            ix1 = addVertex(r0 * cos_ta, r0 * sin_ta, gearDefinition.width * 0.5f,
                            glm::vec3(-cos_ta, -sin_ta, 0.0f));
            ix2 = addVertex(r0 * cos_ta_4da, r0 * sin_ta_4da, -gearDefinition.width * 0.5f,
                            glm::vec3(-cos_ta_4da, -sin_ta_4da, 0.0f));
            ix3 = addVertex(r0 * cos_ta_4da, r0 * sin_ta_4da, gearDefinition.width * 0.5f,
                            glm::vec3(-cos_ta_4da, -sin_ta_4da, 0.0f));
            addFace(ix0, ix1, ix2);
            addFace(ix1, ix3, ix2);
        }

        // We need to know how many indices this triangle has at draw time
        indexCount = static_cast<uint32_t>(indexBuffer.size()) - indexStart;
    }
};

class GearsApp : public Application
{
public:
    explicit GearsApp(const platform::WindowConfig& config);

    void Prepare() final;

    void Run() final;

    void Destroy() final;

private:
    struct SceneData
    {
        Vec4 lightPos;
        Mat4 models[NUM_GEARS];
    };

    void GenerateGears();

    void BuildGraphicsPasses();

    void LoadResources() final;

    void BuildRenderGraph() final;

    void UpdateSceneData();

    std::vector<Gear> m_gears{};

    rc::GraphicsPass m_gfxPass;

    BufferHandle m_vertexBuffer;
    BufferHandle m_indexBuffer;

    SceneData m_sceneData;
    BufferHandle m_sceneUBO;
};