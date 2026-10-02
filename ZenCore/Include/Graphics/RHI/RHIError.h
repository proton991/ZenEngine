#pragma once
#include "Utils/Errors.h"
#include <cstdint>
#include <optional>
#include <utility>

namespace zen
{
enum class RHIErrorCode : uint8_t
{
    eNone,
    eInvalidArgument,
    eUnsupported,
    eOutOfHostMemory,
    eOutOfDeviceMemory,
    eDeviceLost,
    eBackendFailure,
    eCancelled
};

// Capture does not allocate. All operation/source strings have static lifetime.
struct RHIError
{
    RHIErrorCode code{RHIErrorCode::eNone};
    int64_t nativeCode{0};
    const char* operation{nullptr};
    const char* source{nullptr};
    uint32_t line{0};
    uint64_t resourceId{0};

    bool IsFailure() const
    {
        return code != RHIErrorCode::eNone;
    }
};

struct [[nodiscard]] RHIStatus
{
    RHIError error{};

    explicit operator bool() const
    {
        return !error.IsFailure();
    }
};

template <typename T> class [[nodiscard]] RHIResult
{
public:
    explicit RHIResult(T value) : m_value(std::move(value)) {}

    explicit RHIResult(RHIError error) : m_error(error)
    {
        VERIFY_EXPR(error.IsFailure());
    }

    explicit operator bool() const
    {
        return m_value.has_value();
    }

    const RHIError& GetError() const
    {
        return m_error;
    }

    T& GetValue()
    {
        VERIFY_EXPR(m_value.has_value());

        return *m_value;
    }

private:
    std::optional<T> m_value;
    RHIError m_error{};
};
} // namespace zen
