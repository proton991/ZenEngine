#pragma once
#include "AssetLib/SceneImporter.h"
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
    Failed,
    Cancelled
};

// A snapshot owned by the controller and read on the main thread. File progress
// counts distinct input files, independently of cooking and graphics preparation.
struct SceneLoadState
{
    SceneLoadStage                 stage{SceneLoadStage::Idle};
    std::string                    path;
    asset::SceneImportStage        importStage{asset::SceneImportStage::Queued};
    bool                           cancelRequested{false};
    bool                           recook{false};
    asset::SceneImportFileProgress fileProgress;

    bool IsActive() const
    {
        return stage != SceneLoadStage::Idle && stage != SceneLoadStage::Failed && stage != SceneLoadStage::Cancelled;
    }
};
} // namespace zen::editor
