#pragma once
#include <atomic>

#define ZEN_NO_COPY_MOVE(ClassName)                  \
    ClassName(const ClassName&)            = delete; \
    ClassName(ClassName&&)                 = delete; \
    ClassName& operator=(const ClassName&) = delete; \
    ClassName& operator=(ClassName&&)      = delete;

#define ZEN_NO_COPY(ClassName)                       \
    ClassName(const ClassName&)            = delete; \
    ClassName& operator=(const ClassName&) = delete;

#define ZEN_NO_MOVE(ClassName)                  \
    ClassName(ClassName&&)            = delete; \
    ClassName& operator=(ClassName&&) = delete;

#define ZEN_MOVE_CONSTRUCTOR_ONLY(ClassName)         \
    ClassName(const ClassName&)            = delete; \
    ClassName& operator=(const ClassName&) = delete; \
    ClassName& operator=(ClassName&&)      = delete;

#define ZEN_SINGLETON(ClassName)          \
    ClassName(const ClassName&) = delete; \
    ClassName(ClassName&&)      = delete;

#if defined(__clang__)
// CLANG ENABLE/DISABLE WARNING DEFINITION
#    define ZEN_DISABLE_WARNINGS()                                                     \
        _Pragma("clang diagnostic push") _Pragma("clang diagnostic ignored \"-Wall\"") \
            _Pragma("clang diagnostic ignored \"-Wextra\"")                            \
                _Pragma("clang diagnostic ignored \"-Wtautological-compare\"")

#    define ZEN_ENABLE_WARNINGS() _Pragma("clang diagnostic pop")
#elif defined(__GNUC__) || defined(__GNUG__)
// GCC ENABLE/DISABLE WARNING DEFINITION
#    define ZEN_DISABLE_WARNINGS()                                                 \
        _Pragma("GCC diagnostic push") _Pragma("GCC diagnostic ignored \"-Wall\"") \
            _Pragma("clang diagnostic ignored \"-Wextra\"")                        \
                _Pragma("clang diagnostic ignored \"-Wtautological-compare\"")

#    define ZEN_ENABLE_WARNINGS() _Pragma("GCC diagnostic pop")
#elif defined(_MSC_VER)
// MSVC ENABLE/DISABLE WARNING DEFINITION
#    define ZEN_DISABLE_WARNINGS() __pragma(warning(push, 0))

#    define ZEN_ENABLE_WARNINGS() __pragma(warning(pop))
#endif

class ObjectBase
{
public:
    ObjectBase() : m_refCount(1) {}

    void IncreaseRefCount()
    {
        ++m_refCount;
    }

    void DecreaseRefCount()
    {
        if (--m_refCount == 0)
        {
            Destroy(); // delegate actual cleanup to derived class
        }
    }

protected:
    virtual ~ObjectBase() = default;

    virtual void Destroy() = 0;

private:
    std::atomic<uint32_t> m_refCount{0};
};