#pragma once
#include "Templates/HeapVector.h"
#include "Utils/UniquePtr.h"
#include <string>

namespace zen::editor
{
// Owns an external compiler, never imported model data. Poll from the UI thread.
class AssetPreprocessor
{
public:
    AssetPreprocessor();
    ~AssetPreprocessor();
    AssetPreprocessor(const AssetPreprocessor&)            = delete;
    AssetPreprocessor& operator=(const AssetPreprocessor&) = delete;

    bool               AddSource(const std::string& path);
    bool               Start();
    void               Poll();
    void               Cancel();
    bool               IsRunning() const;
    bool               Succeeded() const;
    const std::string& GetStatusText() const;
    const std::string& GetLog() const;
    const std::string& GetOutput() const;
    // Completed build milestones, not an estimate of elapsed/remaining time.
    float              GetProgress() const;
    const std::string& GetProgressText() const;

    HeapVector<std::string> sources;
    std::string             outputDirectory;
    std::string             name{"scene"};
    bool                    compressTextures{false};
    uint32_t                workers{2};
    bool                    browseRequested{false};
    bool                    browseOutputRequested{false};

private:
    struct State;
    UniquePtr<State> m_state;
};
} // namespace zen::editor
