#pragma once
#include "Runtime/NavigationCamera.h"
#include "Templates/HeapVector.h"
#include <string>

namespace zen::runtime
{
// One pose of a scripted camera path. frame is the scene frame (1 is the first rendered frame)
// at which the camera reaches this eye and target.
struct CameraPathKey
{
    uint64_t frame{0};
    Vec3     eye{0.0f};
    Vec3     target{0.0f};
};

// Reads {"keys": [{"frame": N, "eye": [x, y, z], "target": [x, y, z]}, ...]}: at least two keys
// with strictly increasing frames and finite coordinates.
bool ReadCameraPath(const std::string& path, HeapVector<CameraPathKey>& keys, std::string& error);

// The pose at a scene frame, linear between keys and held before the first and after the last
// key. Fields other than eye and target come from base.
NavigationCameraState CameraPathState(const HeapVector<CameraPathKey>& keys, uint64_t frame, NavigationCameraState base);
} // namespace zen::runtime
