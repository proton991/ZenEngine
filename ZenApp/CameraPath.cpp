#include "CameraPath.h"
#include "RuntimeJson.h"
#include <cmath>
#include <limits>

namespace zen::runtime
{
namespace
{
constexpr uint64_t kMaxCameraPathBytes = 16ull * 1024 * 1024;

bool ReadVector(simdjson::dom::element entry, const char* name, Vec3& value)
{
    simdjson::dom::array components;
    bool                 valid = !entry[name].get_array().get(components) && components.size() == 3;
    if (valid)
    {
        uint32_t index = 0;
        for (simdjson::dom::element component : components)
        {
            double number = 0.0;
            valid         = valid && !component.get_double().get(number) && std::isfinite(number)
                         && std::abs(number) <= std::numeric_limits<float>::max();
            if (valid)
            {
                value[index] = static_cast<float>(number);
            }
            ++index;
        }
    }
    return valid;
}

bool ReadKey(simdjson::dom::element entry, CameraPathKey& key)
{
    uint64_t frame = 0;
    bool     valid = !entry["frame"].get_uint64().get(frame) && frame > 0 && ReadVector(entry, "eye", key.eye)
                  && ReadVector(entry, "target", key.target);
    key.frame      = frame;
    return valid;
}
} // namespace

bool ReadCameraPath(const std::string& path, HeapVector<CameraPathKey>& keys, std::string& error)
{
    simdjson::dom::parser   parser;
    simdjson::padded_string json;
    simdjson::dom::element  document;
    simdjson::dom::array    entries;
    keys.clear();
    bool valid = LoadJsonDocument(std::filesystem::u8path(path), kMaxCameraPathBytes, parser, json, document)
              && !document["keys"].get_array().get(entries);
    if (valid)
    {
        for (simdjson::dom::element entry : entries)
        {
            CameraPathKey key;
            valid = valid && ReadKey(entry, key) && (keys.empty() || key.frame > keys.back().frame);
            if (valid)
            {
                keys.push_back(key);
            }
        }
    }
    valid = valid && keys.size() >= 2;
    if (!valid)
    {
        keys.clear();
        error = "Camera path needs at least two keys with increasing frames and finite eye/target: " + path;
    }
    return valid;
}

NavigationCameraState CameraPathState(const HeapVector<CameraPathKey>& keys, uint64_t frame, NavigationCameraState base)
{
    size_t next = 0;
    while (next < keys.size() && keys[next].frame < frame)
    {
        ++next;
    }
    if (next == 0 || next == keys.size())
    {
        const CameraPathKey& held = keys[next == 0 ? 0 : keys.size() - 1];
        base.eye                  = held.eye;
        base.target               = held.target;
    }
    else
    {
        const CameraPathKey& a = keys[next - 1];
        const CameraPathKey& b = keys[next];
        const float          t = float(double(frame - a.frame) / double(b.frame - a.frame));
        base.eye               = glm::mix(a.eye, b.eye, t);
        base.target            = glm::mix(a.target, b.target, t);
    }
    return base;
}
} // namespace zen::runtime
