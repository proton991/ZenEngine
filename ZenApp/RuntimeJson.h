#pragma once
#include <simdjson.h>
#include <filesystem>
#include "Math/Math.h"

namespace zen::runtime
{
bool ReadJsonVector(simdjson::dom::element entry, const char* name, Vec3& value);
// Parses a JSON file of at most maxBytes. It reads through the wide path, because simdjson's own
// loader opens a narrow, code-page path. The document refers to json and parser, which must outlive it.
bool LoadJsonDocument(const std::filesystem::path& path,
                      uint64_t                     maxBytes,
                      simdjson::dom::parser&       parser,
                      simdjson::padded_string&     json,
                      simdjson::dom::element&      document);
} // namespace zen::runtime
