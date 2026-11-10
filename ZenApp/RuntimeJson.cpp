#include "RuntimeJson.h"
#include <fstream>
#include <string>

namespace zen::runtime
{
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
