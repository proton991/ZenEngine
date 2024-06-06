#pragma once

#define ZEN_RHI_SWAPCHAIN_IMAGE_COUNT 3
#include <functional>
namespace zen
{
enum class Status : uint32_t
{
    eSuccess = 0,
    eFailed  = 1,
};
class Handle
{
public:
    explicit Handle(size_t v) : value(v) {}

    Handle() = default;

    size_t value = 0;
};

} // namespace zen

namespace std
{
template <> struct hash<zen::Handle>
{
    size_t operator()(const zen::Handle& handle) const noexcept
    {
        return std::hash<size_t>{}(handle.value);
    }
};
} // namespace std

#define RHI_DEFINE_HANDLE(m_name)                                                           \
    struct m_name##Handle : public zen::Handle                                              \
    {                                                                                       \
        explicit operator bool() const { return value != 0; }                               \
                                                                                            \
        m_name##Handle& operator=(const m_name##Handle& other)                              \
        {                                                                                   \
            value = other.value;                                                            \
            return *this;                                                                   \
        }                                                                                   \
        m_name##Handle& operator=(m_name##Handle&& other) noexcept                          \
        {                                                                                   \
            value = std::move(other.value);                                                 \
            return *this;                                                                   \
        }                                                                                   \
        bool operator<(const m_name##Handle& other) const { return value < other.value; }   \
        bool operator==(const m_name##Handle& other) const { return value == other.value; } \
        bool operator!=(const m_name##Handle& other) const { return value != other.value; } \
        m_name##Handle(const m_name##Handle& other) : Handle(other.value) {}                \
        explicit m_name##Handle(uint64_t intVal) : Handle(intVal) {}                        \
        explicit m_name##Handle(void* ptr) : Handle((size_t)ptr) {}                         \
        m_name##Handle() = default;                                                         \
    };

#define HASH_DEFINE(m_name)                                                 \
    namespace std                                                           \
    {                                                                       \
    template <> struct hash<zen::m_name##Handle>                            \
    {                                                                       \
        size_t operator()(const zen::m_name##Handle& handle) const noexcept \
        {                                                                   \
            return std::hash<size_t>{}(handle.value);                       \
        }                                                                   \
    };                                                                      \
    }

namespace zen
{
RHI_DEFINE_HANDLE(Surface);
RHI_DEFINE_HANDLE(Swapchain);
RHI_DEFINE_HANDLE(CommandPool);
RHI_DEFINE_HANDLE(CommandBuffer);
} // namespace zen

HASH_DEFINE(CommandPool);
HASH_DEFINE(CommandBuffer);