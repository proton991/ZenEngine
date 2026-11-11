#include "RuntimeJson.h"
#include <fstream>
#include <string>
#include <cmath>
#include <limits>

namespace zen::runtime
{
bool ReadJsonVector(simdjson::dom::element entry, const char* name, Vec3& value)
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
bool LoadJsonDocument(const std::filesystem::path& path,
                      uint64_t                     maxBytes,
                      simdjson::dom::parser&       parser,
                      simdjson::padded_string&     json,
                      simdjson::dom::element&      document)
{
    std::error_code fileError;
    const uintmax_t size  = std::filesystem::file_size(path, fileError);
    bool            valid = !fileError && size != 0 && size <= maxBytes;
    if (valid)
    {
        std::string   text(static_cast<size_t>(size), '\0');
        std::ifstream file(path, std::ios::binary);
        valid = bool(file.read(text.data(), static_cast<std::streamsize>(text.size())));
        if (valid)
        {
            json  = simdjson::padded_string(text);
            valid = !parser.parse(json).get(document);
        }
    }
    return valid;
}
} // namespace zen::runtime
