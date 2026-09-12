#pragma once
#include "Graphics/RHI/RHICommon.h"
#include "Templates/BitField.h"
#include <string>
#include <optional>

constexpr uint32_t kMaxRDGNameLen = 64;

// RDG_ID class
class RDG_ID
{
public:
    static constexpr int32_t UndefinedValue = -1;

    // Constructors
    RDG_ID() : m_value(UndefinedValue) {} // Default constructor with "undefined" value

    RDG_ID(int32_t id) : m_value(id) {} // Constructor with int32_t value

    // Copy constructor
    RDG_ID(const RDG_ID& other) : m_value(other.m_value) {}

    // Move constructor
    RDG_ID(RDG_ID&& other) noexcept : m_value(other.m_value)
    {
        other.m_value = UndefinedValue;
    }

    // Copy assignment operator
    RDG_ID& operator=(const RDG_ID& other)
    {
        if (this != &other)
        {
            m_value = other.m_value;
        }

        return *this;
    }

    // Move assignment operator
    RDG_ID& operator=(RDG_ID&& other) noexcept
    {
        if (this != &other)
        {
            m_value       = other.m_value;
            other.m_value = UndefinedValue;
        }

        return *this;
    }

    // Assignment from int32_t
    RDG_ID& operator=(int32_t id)
    {
        m_value = id;
        return *this;
    }

    // Conversion operator to int32_t
    operator int32_t() const
    {
        return m_value;
    }

    // Equality operators
    bool operator==(const RDG_ID& other) const
    {
        return m_value == other.m_value;
    }

    bool operator!=(const RDG_ID& other) const
    {
        return m_value != other.m_value;
    }

    bool IsValid() const
    {
        return m_value != UndefinedValue;
    }

private:
    int32_t m_value;
};

namespace std
{
template <> struct hash<RDG_ID>
{
    std::size_t operator()(const RDG_ID& id) const noexcept
    {
        return std::hash<int32_t>()(id);
    }
};
} // namespace std

namespace zen::rc
{
enum class RDGErrorCode : uint8_t
{
    eNone,
    eLifecycle,
    eShader,
    eBinding,
    eRange,
    eAttachment,
    eDuplicateTag,
    eConflictingLayout,
    eUnsupportedCommand,
    eAllocation,
    eCallback,
    eUninitialized,
    eUnknownContents,
    eExport,
    eVersion,
    eMissingProducer,
    eDuplicateProducer,
    eDependencyCycle,
    eSubmission,
};

struct RDGResult
{
    RDGErrorCode code{RDGErrorCode::eNone};
    std::string message;

    bool Fail(RDGErrorCode error, const std::string& detail)
    {
        if (code == RDGErrorCode::eNone)
        {
            code    = error;
            message = detail;
        }

        return false;
    }

    bool Check(bool condition, RDGErrorCode error, const std::string& detail)
    {
        return code == RDGErrorCode::eNone && (condition || Fail(error, detail));
    }

    explicit operator bool() const
    {
        return code == RDGErrorCode::eNone;
    }
};

class RDGResourceManager;

// Build-scoped logical resource identity. Copies remain valid within this build, including replay,
// but expire on Reset/Begin. A nonempty value still requires validation by its live manager.
class RDGResource
{
public:
    RDGResource() = default;

    explicit operator bool() const
    {
        return m_owner != 0;
    }

    bool operator==(const RDGResource&) const = default;

    bool IsVersioned() const
    {
        return m_version >= 0;
    }

private:
    friend class RDGResourceManager;
    friend class RenderGraph;

    RDGResource(uint64_t owner, uint64_t generation, uint32_t index) :
        m_owner(owner), m_generation(generation), m_index(index)
    {}

    uint64_t m_owner{0};
    uint64_t m_generation{0};
    uint32_t m_index{0};
    int32_t m_version{
        -1}; // -1 requests automatic version selection during declaration finalization.
};

// Resource values select logical contents; allocation metadata stays in the manager.
class RDGTexture
{
public:
    RDGTexture() = default;

    explicit operator bool() const
    {
        return bool(m_resource);
    }

    operator RDGResource() const
    {
        return m_resource;
    }

    bool operator==(const RDGTexture&) const = default;

    bool IsVersioned() const
    {
        return m_resource.IsVersioned();
    }

private:
    friend class RDGResourceManager;

    explicit RDGTexture(RDGResource resource) : m_resource(resource) {}

    RDGResource m_resource;
};

class RDGBuffer
{
public:
    RDGBuffer() = default;

    explicit operator bool() const
    {
        return bool(m_resource);
    }

    operator RDGResource() const
    {
        return m_resource;
    }

    bool operator==(const RDGBuffer&) const = default;

    bool IsVersioned() const
    {
        return m_resource.IsVersioned();
    }

private:
    friend class RDGResourceManager;

    explicit RDGBuffer(RDGResource resource) : m_resource(resource) {}

    RDGResource m_resource;
};

// An absent range selects the full texture. An explicitly supplied empty range is invalid.
// View selection carries no resource/version identity and can be reused for different resources.
struct RDGTextureViewDesc
{
    std::optional<RHITextureSubResourceRange> range;
};

// Optional content guarantees never override reflected shader reads or writes.
enum class RDGContentGuarantee : uint8_t
{
    eNone,
    eDiscard,
    eFullWrite,
    eProducedElements,
    eConsumeProducedElements
};

enum class RDGContentStatus : uint8_t
{
    eUnknown,
    eUndefined,
    eDefined
};
// Preserve uses device-tracked contents, or Unknown for external work with no contract.
// Defined/Undefined/Unknown are caller assertions at the start of each execution.
enum class RDGImportContents : uint8_t
{
    ePreserve,
    eDefined,
    eUndefined,
    eUnknown
};

// Internal content effects derived from reflection, guarantees, and fixed-function operations.
// Shader binding APIs do not accept these as access overrides.
enum class RDGContentEffect : uint8_t
{
    eAutomatic, // Fixed-function declaration defaults; shader effects are inferred explicitly.
    eRead,
    eWrite, // Partial write; preserves untouched contents, proves no initialization.
    eReadWrite,
    eDiscardWrite, // Discard old contents, then partial write.
    // Buffer stream contracts: the producer defines every emitted record; the
    // consumer uses its matching count/index set and never reads unused capacity.
    eWriteProducedElements,
    eReadProducedElements,
    eFullWrite, // Discard and define every texel/byte in the declared range.
};

struct RDGContentAccess
{
    RDG_ID resourceId;
    RDGContentEffect intent{RDGContentEffect::eRead};
    RHITextureSubResourceRange range;
    bool fullCoverage{true};
    bool discardAfter{false};
    bool requiresPriorContents{false}; // Reflected reads survive full-write/discard guarantees.
    RDG_ID sourceResourceId{-1};
    RHITextureSubResourceRange sourceRange;
    uint64_t bufferOffset{0};
    uint64_t bufferSize{0}; // Zero means the full buffer.
    uint64_t sourceBufferOffset{0};
    uint64_t sourceBufferSize{0};
};

enum class RDGNodeType : uint32_t
{
    eNone         = 0,
    eGraphicsPass = 1,
    eComputePass  = 2,
    eTransferPass = 3,
    eMax          = 4
};

// A shader write defines this version and reads its predecessor when its content intent
// requires existing data. A separate read binding consumes the named version itself.
struct RDGVersionAccess
{
    RDG_ID resourceId{-1};
    int32_t version{-1};
    bool writes{false};
};

enum class RDGDependencyReason : uint8_t
{
    eWriteAfterRead,
    eWriteAfterWrite,
    eVersionProducer
};

struct RDGDependency
{
    RDG_ID source{-1};
    RDG_ID destination{-1};
    RDG_ID resourceId{-1};
    int32_t version{-1}; // Per-resource version number; every compiled dependency has a version.
    RDGDependencyReason reason{};

    // Whole allocation scope until range-aware dependencies land in Phase 4.
    RHITextureSubResourceRange textureRange;
    uint64_t bufferSize{0};
};

struct RDGAccess
{
    RHIAccessMode accessMode{};

    // Declaration metadata only; End resolves automatic accesses into versionAccesses.
    bool explicitVersion{false};
    RDG_ID nodeId{-1};
    RDG_ID resourceId{-1};
    BitField<RHIBufferUsageFlagBits> bufferUsage{};
    RHITextureUsage textureUsage{RHITextureUsage::eMax};
    BitField<RHIAccessFlagBits> accessFlags;

    // Resource-specific execution stages used by synchronization and diagnostics.
    BitField<RHIPipelineStageFlagBits> pipelineStages;
    RHITextureSubResourceRange textureSubResourceRange;
};
} // namespace zen::rc
