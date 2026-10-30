#pragma once
#include "Runtime/NavigationCamera.h"

namespace zen::editor
{
// Editor input/actions use the same navigation model as the standalone runtime.
using EditorCamera                                   = runtime::NavigationCamera;
using CameraInput                                    = runtime::CameraInput;
inline constexpr float kDefaultEditorCameraMoveSpeed = runtime::kDefaultNavigationCameraMoveSpeed;
inline constexpr float kMinEditorCameraMoveSpeed     = runtime::kMinNavigationCameraMoveSpeed;
inline constexpr float kMaxEditorCameraMoveSpeed     = runtime::kMaxNavigationCameraMoveSpeed;
inline float           ClampEditorCameraMoveSpeed(float speed)
{
    return runtime::ClampNavigationCameraMoveSpeed(speed);
}
} // namespace zen::editor
