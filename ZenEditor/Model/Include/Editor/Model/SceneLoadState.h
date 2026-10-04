#pragma once
#include <string>

namespace zen::editor
{
enum class SceneLoadStage
{
    Idle,
    Queued,
    Reading,
    Preparing,
    Publishing,
    Complete,
    Failed
};

// A snapshot owned by the controller and read on the main thread. Progress is by
// completed stages, not elapsed time or bytes; parsing has indeterminate duration.
struct SceneLoadState
{
    SceneLoadStage stage{SceneLoadStage::Idle};
    std::string    path;

    bool IsActive() const
    {
        return stage != SceneLoadStage::Idle && stage != SceneLoadStage::Failed;
    }
};
} // namespace zen::editor
