#include "LightPath.h"
#include "RuntimeJson.h"

namespace zen::runtime
{
bool ReadLightPath(const std::string& path, LightPath& result, std::string& error)
{
    simdjson::dom::parser   parser;
    simdjson::padded_string json;
    simdjson::dom::element  document;
    simdjson::dom::array    entries;
    LightPath               candidate;
    bool                    valid = LoadJsonDocument(std::filesystem::u8path(path), 16ull * 1024 * 1024, parser, json, document)
                                 && !document["lightId"].get_uint64().get(candidate.id) && candidate.id != 0
                                 && !document["keys"].get_array().get(entries);
    if (valid)
    {
        for (simdjson::dom::element entry : entries)
        {
            LightPathKey key;
            valid = valid && !entry["frame"].get_uint64().get(key.frame) && key.frame > 0
                 && (candidate.keys.empty() || candidate.keys.back().frame < key.frame)
                 && ReadJsonVector(entry, "position", key.position);
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
    }
    else
    {
        error = "Light path needs a nonzero lightId and increasing positive frames with finite positions: " + path;
    }
    return valid;
}

Vec3 LightPathPosition(const LightPath& path, uint64_t frame)
{
    size_t index = 0;
    while (index + 1 < path.keys.size() && path.keys[index + 1].frame <= frame)
    {
        ++index;
    }
    return path.keys[index].position;
}
} // namespace zen::runtime
