#pragma once
#include <cstdint>

namespace zen::runtime
{
enum class RuntimeState : uint32_t
{
    Idle,
    Starting,
    Running,
    Paused,
    Stopping,
    Failed
};

inline const char* GetRuntimeStateName(RuntimeState state)
{
    const char* name = "Invalid";
    switch (state)
    {
        case RuntimeState::Idle: name = "Idle"; break;
        case RuntimeState::Starting: name = "Starting"; break;
        case RuntimeState::Running: name = "Running"; break;
        case RuntimeState::Paused: name = "Paused"; break;
        case RuntimeState::Stopping: name = "Stopping"; break;
        case RuntimeState::Failed: name = "Failed"; break;
    }
    return name;
}
} // namespace zen::runtime
