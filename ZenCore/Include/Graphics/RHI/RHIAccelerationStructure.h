#pragma once
#include "RHIResource.h"
#include "Templates/VectorView.h"
#include <cmath>

namespace zen
{
enum class RHIAccelerationStructureType : uint32_t
{
    eTopLevel    = 0,
    eBottomLevel = 1
};

enum class RHIAccelerationStructureInstanceFlagBits : uint32_t
{
    eDisableTriangleCulling = 1u << 0,
    eReverseTriangleFacing  = 1u << 1,
    eForceOpaque            = 1u << 2,
    eForceNonOpaque         = 1u << 3
};

class RHIAccelerationStructure;

// CPU description, never native instance bytes. Recorded builds copy this data
// and retain pStructure. The backend encodes its own address or instance index.
struct RHIAccelerationStructureInstance
{
    float                                              transform[3][4]{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
    uint32_t                                           customIndex{0}; // Shader-visible 24-bit custom index.
    uint8_t                                            mask{0xff};
    BitField<RHIAccelerationStructureInstanceFlagBits> flags;
    RHIAccelerationStructure*                          pStructure{nullptr};

    bool IsValid() const;
};

// Triangle ranges use the same indexing as DrawIndexed: indices are relative to
// vertexOffset, and indexOffset points at the first index in this geometry.
struct RHIAccelerationStructureGeometry
{
    RHIBuffer* pVertexBuffer{nullptr};
    RHIBuffer* pIndexBuffer{nullptr};
    uint64_t   vertexOffset{0};
    uint64_t   indexOffset{0};
    uint32_t   vertexStride{0};
    uint32_t   vertexCount{0};
    uint32_t   indexCount{0};
    DataFormat vertexFormat{DataFormat::eR32G32B32SFloat};
    DataFormat indexFormat{DataFormat::eR32UInt};
    bool       opaque{true};
};

struct RHIAccelerationStructureBuildDesc
{
    RHIAccelerationStructureType                       type{RHIAccelerationStructureType::eBottomLevel};
    VectorView<const RHIAccelerationStructureGeometry> geometries;
    // Size queries use the count; builds additionally require valid BLAS references.
    VectorView<const RHIAccelerationStructureInstance> instances;
    bool                                               allowUpdate{false};
};

struct RHIAccelerationStructureBuildSizes
{
    uint64_t storageSize{0};
    uint64_t buildScratchSize{0};
    uint64_t updateScratchSize{0};
    // Native packed instance upload owned by a recorded build until GPU completion.
    uint64_t instanceDataSize{0};

    bool IsValid() const
    {
        return storageSize != 0;
    }
};

struct RHIAccelerationStructureCreateInfo
{
    RHIAccelerationStructureType type{RHIAccelerationStructureType::eBottomLevel};
    uint64_t                     size{0};
    NameID                       tag;
    // Immutable dependencies: a replacement TLAS with different BLASes is a new
    // resource. Holding the TLAS therefore retains every indirectly queried BLAS.
    VectorView<RHIAccelerationStructure* const> referencedStructures;
};

class RHIAccelerationStructure : public RHIResource
{
public:
    RHIAccelerationStructureType GetType() const
    {
        return m_type;
    }

    uint64_t GetSize() const
    {
        return m_size;
    }

    const HeapVector<RHIAccelerationStructure*>& GetReferencedStructures() const
    {
        return m_referencedStructures;
    }

protected:
    explicit RHIAccelerationStructure(const RHIAccelerationStructureCreateInfo& info) :
        RHIResource(RHIResourceType::eAccelerationStructure, info.tag), m_type(info.type), m_size(info.size)
    {
        for (RHIAccelerationStructure* structure : info.referencedStructures)
        {
            structure->AddReference();
            m_referencedStructures.push_back(structure);
        }
    }

    ~RHIAccelerationStructure() override
    {
        for (RHIAccelerationStructure* structure : m_referencedStructures)
        {
            structure->ReleaseReference();
        }
    }

private:
    RHIAccelerationStructureType          m_type;
    uint64_t                              m_size{0};
    HeapVector<RHIAccelerationStructure*> m_referencedStructures;
};

struct RHIAccelerationStructureBuildInfo
{
    RHIAccelerationStructureBuildDesc description;
    RHIAccelerationStructure*         pDestination{nullptr};
    // Non-null requests an update from a compatible, previously built source.
    RHIAccelerationStructure* pSource{nullptr};
    RHIBuffer*                pScratchBuffer{nullptr};
    uint64_t                  scratchOffset{0};
};

inline bool RHIAccelerationStructureInstance::IsValid() const
{
    bool valid = pStructure != nullptr && pStructure->GetType() == RHIAccelerationStructureType::eBottomLevel
              && customIndex <= 0xffffffu && (uint32_t(flags) & ~0xfu) == 0
              && !(flags.HasFlag(RHIAccelerationStructureInstanceFlagBits::eForceOpaque)
                   && flags.HasFlag(RHIAccelerationStructureInstanceFlagBits::eForceNonOpaque));
    for (const float (&row)[4] : transform)
    {
        for (float value : row)
        {
            valid &= std::isfinite(value);
        }
    }
    const double determinant =
        double(transform[0][0]) * (double(transform[1][1]) * transform[2][2] - double(transform[1][2]) * transform[2][1])
        - double(transform[0][1]) * (double(transform[1][0]) * transform[2][2] - double(transform[1][2]) * transform[2][0])
        + double(transform[0][2]) * (double(transform[1][0]) * transform[2][1] - double(transform[1][1]) * transform[2][0]);
    return valid && std::isfinite(determinant) && determinant != 0;
}
} // namespace zen
