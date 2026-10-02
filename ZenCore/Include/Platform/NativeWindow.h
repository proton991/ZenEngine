#pragma once
#include "ObjectBase.h"
#include <cstdint>
#include <string>

namespace zen::platform
{

struct WindowConfig
{
    std::string title{"ZenEngine"};
    bool        resizable{false};
    uint32_t    width{1280};
    uint32_t    height{720};
    float       aspect{0.0f};
};

struct WindowExtent
{
    uint32_t width{0};
    uint32_t height{0};
};

class NativeWindow
{
public:
    NativeWindow() = default;

    ZEN_NO_COPY_MOVE(NativeWindow)

    virtual ~NativeWindow()                  = default;

    virtual WindowExtent GetExtent2D() const = 0;

    virtual float GetAspect()                = 0;
};
} // namespace zen::platform
