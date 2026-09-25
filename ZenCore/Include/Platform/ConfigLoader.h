#pragma once
#include <fstream>
#include <filesystem>
#include <string>
#include <charconv>
#include <cmath>
#include <sstream>
#include "Math/Math.h"
#include "Utils/Errors.h"
#include "Templates/HashMap.h"

namespace zen::platform
{
enum class VoxelizerMode
{
    eAuto,
    eCompute,
    eGeometry
};

enum class AsyncComputeMode
{
    eDisabled,
    eAuto
};

class ConfigLoader
{
public:
    static ConfigLoader& GetInstance()
    {
        static ConfigLoader instance;
        return instance;
    }

    explicit ConfigLoader(std::istream& config)
    {
        LoadConfig(config);
    }

    VoxelizerMode GetVoxelizerMode() const
    {
        VoxelizerMode mode = VoxelizerMode::eAuto;
        auto it            = m_configData.find("voxelizer");
        if (it != m_configData.end() && it->second != "auto")
        {
            if (it->second == "comp")
            {
                mode = VoxelizerMode::eCompute;
            }
            else if (it->second == "geom")
            {
                mode = VoxelizerMode::eGeometry;
            }
            else
            {
                LOGW("Invalid voxelizer '{}'; expected auto, comp or geom. Using auto.",
                     it->second);
            }
        }
        return mode;
    }

    AsyncComputeMode GetAsyncComputeMode() const
    {
        AsyncComputeMode mode = AsyncComputeMode::eDisabled;
        const auto entry      = m_configData.find("async_compute");
        if (entry != m_configData.end())
        {
            if (entry->second == "auto")
            {
                mode = AsyncComputeMode::eAuto;
            }
            else if (entry->second != "off")
            {
                LOGW("Invalid async_compute '{}'; expected off or auto. Using off.", entry->second);
            }
        }
        return mode;
    }

    std::string GetSkyboxModelPath() const
    {
        return GetConfiguredModelPath("skybox_model");
    }

    std::string GetDefaultGLTFModelPath() const
    {
        return HasKey("default_model_path") ?
            ResolveModelPath(GetString("default_model_path", "")) :
            GetConfiguredModelPath("default_model");
    }

    std::string GetGLTFModelPath(const std::string& name) const
    {
        std::string path = "";
        auto basePathIt  = m_configData.find("model_base_path");

        if (basePathIt != m_configData.end())
        {
            path = ResolveModelPath(basePathIt->second + "/" + name + "/glTF/" + name + ".gltf");
        }
        else
        {
            LOGE("Missing configuration values for GLTF model path.");
        }

        return path;
    }

    ConfigLoader(const ConfigLoader&)            = delete;
    ConfigLoader& operator=(const ConfigLoader&) = delete;

    uint32_t GetVoxelResolution() const
    {
        uint32_t resolution = GetString("voxel_gi_method", "auto") == "dynamic_voxel" ? 64 : 256;
        if (!ReadNumber("voxel_resolution", resolution) ||
            (resolution != 64 && resolution != 128 && resolution != 256))
        {
            LOGW("Invalid voxel_resolution; expected 64, 128 or 256. Using 256.");
            resolution = 256;
        }
        return resolution;
    }

    bool HasKey(const std::string& key) const
    {
        return m_configData.find(key) != m_configData.end();
    }

    std::string GetString(const std::string& key, const std::string& fallback) const
    {
        const auto entry = m_configData.find(key);
        return entry == m_configData.end() ? fallback : entry->second;
    }

    // Missing optional values preserve the caller's default. Invalid values do not modify it.
    template <typename T> bool ReadNumber(const std::string& key, T& value) const
    {
        bool valid       = true;
        const auto entry = m_configData.find(key);
        if (entry != m_configData.end())
        {
            T parsed{};
            const std::string& text = entry->second;
            const std::from_chars_result result =
                std::from_chars(text.data(), text.data() + text.size(), parsed);
            valid = result.ec == std::errc{} && result.ptr == text.data() + text.size() &&
                std::isfinite(static_cast<double>(parsed));
            if (valid)
            {
                value = parsed;
            }
            else
            {
                LOGW("Invalid numeric config '{}={}'", key, text);
            }
        }
        return valid;
    }

    bool ReadBool(const std::string& key, bool& value) const
    {
        bool valid       = true;
        const auto entry = m_configData.find(key);
        if (entry != m_configData.end())
        {
            valid = entry->second == "true" || entry->second == "false";
            if (valid)
            {
                value = entry->second == "true";
            }
            else
            {
                LOGW("Invalid boolean config '{}={}'", key, entry->second);
            }
        }
        return valid;
    }

    bool ReadVec3(const std::string& key, Vec3& value) const
    {
        bool valid       = true;
        const auto entry = m_configData.find(key);
        if (entry != m_configData.end())
        {
            Vec3 parsed(0.0f);
            std::istringstream input(entry->second);
            char separator1 = 0;
            char separator2 = 0;
            valid = static_cast<bool>(input >> parsed.x >> separator1 >> parsed.y >> separator2 >>
                                      parsed.z);
            input >> std::ws;
            valid = valid && input.eof() && separator1 == ',' && separator2 == ',' &&
                std::isfinite(parsed.x) && std::isfinite(parsed.y) && std::isfinite(parsed.z);
            if (valid)
            {
                value = parsed;
            }
            else
            {
                LOGW("Invalid vector config '{}={}'", key, entry->second);
            }
        }
        return valid;
    }

private:
    static std::string ResolveModelPath(const std::string& path)
    {
        std::string resolved = path;
        const std::filesystem::path modelPath(path);
        if (!path.empty() && modelPath.is_relative())
        {
            // Config-relative paths also preserve the former repository/bin asset layout.
            resolved = (std::filesystem::path(ZEN_CONFIG_PATH).parent_path() / modelPath)
                           .lexically_normal()
                           .generic_string();
        }
        return resolved;
    }

    std::string GetConfiguredModelPath(const char* key) const
    {
        std::string path;
        auto model = m_configData.find(key);
        if (model != m_configData.end())
        {
            path = GetGLTFModelPath(model->second);
        }
        else
        {
            LOGE("Missing configuration values for GLTF model path.");
        }
        return path;
    }

    ConfigLoader()
    {
        LoadConfig(ZEN_CONFIG_PATH);
    }

    HashMap<std::string, std::string> m_configData;

    void LoadConfig(const std::string& configPath)
    {
        std::ifstream file(configPath);
        if (!file)
        {
            LOGW("Config file not found at {}, creating default config.", configPath);
            std::ofstream outFile(configPath);
            outFile << "model_base_path=../../glTF-Sample-Assets/Models" << std::endl;
            outFile << "default_model=Suzanne" << std::endl;
            outFile << "# Voxelizer: auto, comp or geom (falls back to comp if unsupported)."
                    << std::endl;
            outFile << "voxelizer=auto" << std::endl;
            outFile
                << "# Async compute: off or auto (requires a separate queue and GPU dependencies)."
                << std::endl;
            outFile << "async_compute=off" << std::endl;
            outFile.close();
            LOGI("Default config created at {}.", configPath);
            file.clear();
            file.open(configPath);
        }
        if (file)
        {
            LoadConfig(file);
        }
    }

    static std::string Trim(const std::string& text)
    {
        const size_t first = text.find_first_not_of(" \t\r\n");
        std::string trimmed;
        if (first != std::string::npos)
        {
            trimmed = text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
        }
        return trimmed;
    }

    void LoadConfig(std::istream& config)
    {
        std::string line;
        while (std::getline(config, line))
        {
            const size_t comment = line.find('#');
            if (comment != std::string::npos)
            {
                line.erase(comment);
            }

            const size_t separator = line.find('=');
            if (separator == std::string::npos)
            {
                continue;
            }

            const std::string key   = Trim(line.substr(0, separator));
            const std::string value = Trim(line.substr(separator + 1));
            if (!key.empty())
            {
                m_configData[key] = value;
                LOGI("Loaded config: {}={}", key, value);
            }
        }
    }
};
} // namespace zen::platform
