#pragma once
#include "AssetLib/SceneImporter.h"
#include <mutex>

namespace zen::asset
{
// The first content error of one import. Texture-decoding workers report through it too,
// and each import stage skips its work once an error is recorded.
class ImportErrors
{
public:
    // Later failures keep the first message.
    void Fail(std::string message, SceneImportStatus status = SceneImportStatus::InvalidData);

    bool Failed() const;

    SceneImportStatus Status() const;

    std::string Message() const;

    void Reset(SceneImportControl* control = nullptr);

    void DetachControl();

    void SetStage(SceneImportStage stage);

private:
    SceneImportStatus   m_status{SceneImportStatus::InvalidData};
    mutable std::mutex  m_mutex;
    std::string         m_message;
    std::atomic<bool>   m_failed{false};
    SceneImportControl* m_control{nullptr};
};

} // namespace zen::asset
