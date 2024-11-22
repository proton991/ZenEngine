#pragma once
#include <functional>

#define ZEN_RHI_SWAPCHAIN_IMAGE_COUNT 3

namespace zen::rhi
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

    bool operator==(const Handle& other) const
    {
        return this->value == other.value;
    }

    size_t value = 0;
};
#define RHI_DEFINE_HANDLE(m_name)                                            \
    struct m_name##Handle : public Handle                                    \
    {                                                                        \
        explicit operator bool() const                                       \
        {                                                                    \
            return value != 0;                                               \
        }                                                                    \
                                                                             \
        m_name##Handle& operator=(const m_name##Handle& other)               \
        {                                                                    \
            value = other.value;                                             \
            return *this;                                                    \
        }                                                                    \
        m_name##Handle& operator=(m_name##Handle&& other) noexcept           \
        {                                                                    \
            value = std::move(other.value);                                  \
            return *this;                                                    \
        }                                                                    \
        bool operator<(const m_name##Handle& other) const                    \
        {                                                                    \
            return value < other.value;                                      \
        }                                                                    \
        bool operator==(const m_name##Handle& other) const                   \
        {                                                                    \
            return value == other.value;                                     \
        }                                                                    \
        bool operator!=(const m_name##Handle& other) const                   \
        {                                                                    \
            return value != other.value;                                     \
        }                                                                    \
        m_name##Handle(const m_name##Handle& other) : Handle(other.value) {} \
        explicit m_name##Handle(uint64_t intVal) : Handle(intVal) {}         \
        explicit m_name##Handle(void* ptr) : Handle((size_t)ptr) {}          \
        m_name##Handle() = default;                                          \
    };
} // namespace zen::rhi

namespace std
{
template <> struct hash<zen::rhi::Handle>
{
    size_t operator()(const zen::rhi::Handle& handle) const noexcept
    {
        return std::hash<size_t>{}(handle.value);
    }
};
} // namespace std



#define HASH_DEFINE(m_name)                                                      \
    namespace std                                                                \
    {                                                                            \
    template <> struct hash<zen::rhi::m_name##Handle>                            \
    {                                                                            \
        size_t operator()(const zen::rhi::m_name##Handle& handle) const noexcept \
        {                                                                        \
            return std::hash<size_t>{}(handle.value);                            \
        }                                                                        \
    };                                                                           \
    }

namespace zen::rhi
{
// RHI_DEFINE_HANDLE(Surface);
// RHI_DEFINE_HANDLE(Swapchain);
//RHI_DEFINE_HANDLE(CommandPool);
//RHI_DEFINE_HANDLE(CommandBuffer);
RHI_DEFINE_HANDLE(Shader);
RHI_DEFINE_HANDLE(RenderPass);
RHI_DEFINE_HANDLE(Framebuffer);
RHI_DEFINE_HANDLE(Sampler);
RHI_DEFINE_HANDLE(Pipeline);
RHI_DEFINE_HANDLE(Texture);
RHI_DEFINE_HANDLE(Buffer);
RHI_DEFINE_HANDLE(DescriptorSet);
} // namespace zen::rhi

HASH_DEFINE(Shader)
HASH_DEFINE(RenderPass)