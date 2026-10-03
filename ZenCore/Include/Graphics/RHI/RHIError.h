#pragma once
#include "Utils/Errors.h"
#include "RHIOptions.h"
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
    int64_t      nativeCode{0};
    const char*  operation{nullptr};
    const char*  source{nullptr};
    uint32_t     line{0};
    uint64_t     resourceId{0};

    bool IsFailure() const
    {
        return code != RHIErrorCode::eNone;
    }
};

inline RHIError MakeRHIError(RHIErrorCode code, const char* operation, const char* source, uint32_t line)
{
    return {code, 0, operation, source, line};
}

// Expected incomplete operations are distinct from failures.
enum class RHIWaitOutcome : uint8_t
{
    eCompleted,
    eIncomplete,
    eFailed
};

struct [[nodiscard]] RHIWaitResult
{
    RHIWaitOutcome outcome{RHIWaitOutcome::eIncomplete};
    RHIError       error{};
};

enum class RHIJobAdmission : uint8_t
{
    eAccepted,
    eQueueFull,
    eClosed,
    eFailed
};

struct [[nodiscard]] RHIJobAdmissionResult
{
    RHIJobAdmission admission{RHIJobAdmission::eClosed};
    RHIError        error{};

    explicit operator bool() const
    {
        return admission == RHIJobAdmission::eAccepted;
    }
};

enum class RHISurfaceOutcome : uint8_t
{
    eReady,
    eIncomplete,
    eRecreate,
    eSurfaceLost,
    eFailed
};

enum class RHIPresentAcceptance : uint8_t
{
    eNotAttempted,
    eRejected,
    eEnqueued,
    eUncertain
};

struct [[nodiscard]] RHIAcquireResult
{
    RHISurfaceOutcome outcome{RHISurfaceOutcome::eIncomplete};
    int32_t           imageIndex{-1};
    RHIError          error{};
};

struct [[nodiscard]] RHIPresentResult
{
    RHISurfaceOutcome    outcome{RHISurfaceOutcome::eIncomplete};
    RHIPresentAcceptance acceptance{RHIPresentAcceptance::eNotAttempted};
    RHIError             error{};
    bool                 presented{false};
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
    RHIError         m_error{};
};

template <> class [[nodiscard]] RHIResult<void>
{
public:
    RHIResult() = default;

    explicit RHIResult(RHIError error) : m_error(error)
    {
        VERIFY_EXPR(error.IsFailure());
    }

    explicit operator bool() const
    {
        return !m_error.IsFailure();
    }

    const RHIError& GetError() const
    {
        return m_error;
    }

    void GetValue() const
    {
        VERIFY_EXPR(!m_error.IsFailure());
    }

private:
    RHIError m_error{};
};

// Teardown ownership is fatal under RHIOptions::StrictTeardownChecks and logged otherwise.
inline void VerifyTeardownOwnership(bool released, const char* message, const char* pFile, int line)
{
    if (!released)
    {
        if (RHIOptions::GetInstance().StrictTeardownChecks())
        {
            VerificationFailure("teardown ownership", pFile, line, message);
        }
        else
        {
            LOGE("{}", message);
        }
    }
}
} // namespace zen
