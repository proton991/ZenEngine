#pragma once
#include "RHIResource.h"
#include "Templates/VectorView.h"

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
    RHIBuffer*                                         pInstanceBuffer{nullptr};
    uint64_t                                           instanceOffset{0};
    uint32_t                                           instanceCount{0};
    bool                                               allowUpdate{false};
};

struct RHIAccelerationStructureBuildSizes
{
    uint64_t storageSize{0};
    uint64_t buildScratchSize{0};
    uint64_t updateScratchSize{0};

    bool IsValid() const
    {
        return storageSize != 0;
    }
};

class RHIAccelerationStructure;

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

    virtual uint64_t GetDeviceAddress() const = 0;

    // The graph tracks AS accesses on this allocation, including indirect BLAS reads.
    virtual RHIBuffer* GetStorageBuffer() const = 0;

    const HeapVector<RHIAccelerationStructure*>& GetReferencedStructures() const
    {
        return m_referencedStructures;
    }

protected:
    explicit RHIAccelerationStructure(const RHIAccelerationStructureCreateInfo& info) :
        RHIResource(RHIResourceType::eAccelerationStructure, info.tag), m_type(info.type)
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

// Portable instance payload: row-major affine transform followed by packed
// 24-bit custom index / 8-bit mask, zero SBT offset / 8-bit flags, and BLAS address.
struct RHIAccelerationStructureInstance
{
    float    transform[3][4]{};
    uint32_t customIndexAndMask{0};
    uint32_t offsetAndFlags{0};
    uint64_t accelerationStructureAddress{0};
};
static_assert(sizeof(RHIAccelerationStructureInstance) == 64);
} // namespace zen
