#include "Math/Math.h"
#include "LightPath.h"
#include "RuntimeJson.h"
#include <cmath>
#include <limits>
#include <algorithm>

namespace zen::runtime
{
namespace
{
bool ReadNumber(simdjson::dom::element entry, const char* field, float& value)
{
    double     number = 0;
    const bool valid  = !entry[field].get_double().get(number) && std::isfinite(number)
                     && std::abs(number) <= std::numeric_limits<float>::max();
    if (valid)
    {
        value = static_cast<float>(number);
    }
    return valid;
}

bool ReadTarget(simdjson::dom::element document, LightPath& candidate)
{
    std::string_view           name;
    const simdjson::error_code field = document["target"].get_string().get(name);
    bool                       valid = field == simdjson::NO_SUCH_FIELD || field == simdjson::SUCCESS;
    if (field == simdjson::NO_SUCH_FIELD || name == "light")
    {
        candidate.target = field == simdjson::NO_SUCH_FIELD ? PathTarget::LightPosition : PathTarget::Light;
        valid            = valid && !document["lightId"].get_uint64().get(candidate.id) && candidate.id != 0;
    }
    else if (name == "animation")
    {
        candidate.target = PathTarget::Animation;
    }
    else if (name == "cameraLight")
    {
        candidate.target = PathTarget::CameraLight;
    }
    else if (name == "environment")
    {
        candidate.target = PathTarget::Environment;
    }
    else if (name == "instance" || name == "material")
    {
        candidate.target = name == "instance" ? PathTarget::Instance : PathTarget::Material;
        uint64_t index   = 0;
        valid            = !document["index"].get_uint64().get(index) && index <= UINT32_MAX;
        candidate.index  = static_cast<uint32_t>(index);
    }
    else
    {
        valid = false;
    }
    return valid;
}

bool ReadPose(simdjson::dom::element entry, PathTarget target, LightPathKey& key)
{
    bool valid = false;
    switch (target)
    {
        case PathTarget::LightPosition:
        case PathTarget::Instance: valid = ReadJsonVector(entry, "position", key.position); break;
        case PathTarget::CameraLight:
        {
            const simdjson::error_code enabled = entry["enabled"].get_bool().get(key.enabled);
            valid                              = ReadJsonVector(entry, "position", key.position)
                                              && (enabled == simdjson::SUCCESS || enabled == simdjson::NO_SUCH_FIELD);
            break;
        }
        case PathTarget::Light:
            valid = ReadJsonVector(entry, "position", key.position) && ReadJsonVector(entry, "color", key.color)
                 && ReadNumber(entry, "intensity", key.intensity) && key.intensity >= 0
                 && glm::all(glm::greaterThanEqual(key.color, Vec3(0))) && !entry["enabled"].get_bool().get(key.enabled);
            break;
        case PathTarget::Environment:
            valid = ReadNumber(entry, "intensity", key.intensity) && key.intensity >= 0
                 && ReadNumber(entry, "rotationDegrees", key.rotationDegrees);
            break;
        case PathTarget::Material:
            valid = ReadJsonVector(entry, "color", key.color) && ReadJsonVector(entry, "emission", key.emission)
                 && glm::all(glm::greaterThanEqual(key.color, Vec3(0))) && glm::all(glm::lessThanEqual(key.color, Vec3(1)))
                 && glm::all(glm::greaterThanEqual(key.emission, Vec3(0)));
            break;
        case PathTarget::Animation: valid = ReadNumber(entry, "seconds", key.seconds) && key.seconds >= 0; break;
    }
    return valid;
}
} // namespace

bool ReadLightPath(const std::string& path, LightPath& result, std::string& error)
{
    simdjson::dom::parser   parser;
    simdjson::padded_string json;
    simdjson::dom::element  document;
    simdjson::dom::array    entries;
    LightPath               candidate;
    bool                    valid = LoadJsonDocument(std::filesystem::u8path(path), 16ull * 1024 * 1024, parser, json, document)
                                 && ReadTarget(document, candidate) && !document["keys"].get_array().get(entries);
    if (valid)
    {
        for (simdjson::dom::element entry : entries)
        {
            LightPathKey key;
            valid = valid && !entry["frame"].get_uint64().get(key.frame) && key.frame > 0
                 && (candidate.keys.empty() || candidate.keys.back().frame < key.frame)
                 && ReadPose(entry, candidate.target, key);
            if (valid)
            {
                candidate.keys.push_back(key);
            }
        }
    }
    valid = valid && !candidate.keys.empty();
    if (valid)
    {
        result = std::move(candidate);
        error.clear();
    }
    else
    {
        error = "Invalid scene path target, pose, or frame sequence: " + path;
    }
    return valid;
}

const LightPathKey& LightPathPose(const LightPath& path, uint64_t frame)
{
    const auto next = std::upper_bound(path.keys.begin(), path.keys.end(), frame,
                                       [](uint64_t value, const LightPathKey& key) { return value < key.frame; });
    return next == path.keys.begin() ? path.keys.front() : *(next - 1);
}
} // namespace zen::runtime
