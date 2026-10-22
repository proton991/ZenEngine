#include "AssetLib/ImportErrors.h"

namespace zen::asset
{
void ImportErrors::Fail(std::string message, SceneImportStatus status)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_failed.load(std::memory_order_relaxed))
    {
        m_message = std::move(message);
        m_status  = status;

        m_failed.store(true, std::memory_order_release);
    }
}

SceneImportStatus ImportErrors::Status() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_status;
}

bool ImportErrors::Failed() const
{
    return m_failed.load(std::memory_order_acquire) || (m_control != nullptr && m_control->IsCancelled());
}

std::string ImportErrors::Message() const
{
    std::lock_guard<std::mutex> lock(m_mutex);

    return m_message.empty() && m_control != nullptr && m_control->IsCancelled() ? "Scene import cancelled" : m_message;
}

void ImportErrors::Reset(SceneImportControl* control)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    m_message.clear();
    m_status  = SceneImportStatus::InvalidData;

    m_control = control;

    m_failed.store(false, std::memory_order_release);
}

void ImportErrors::DetachControl()
{
    // Called only after every processing future has completed.
    m_control = nullptr;
}

void ImportErrors::SetStage(SceneImportStage stage)
{
    if (m_control != nullptr)
    {
        m_control->SetStage(stage);
    }
}

} // namespace zen::asset
