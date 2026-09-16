#pragma once
#include <fstream>
#include <string>
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
        auto it = m_configData.find("voxelizer");
        if (it == m_configData.end() || it->second == "auto")
        {
            return VoxelizerMode::eAuto;
        }
        if (it->second == "comp")
        {
            return VoxelizerMode::eCompute;
        }
        if (it->second == "geom")
        {
            return VoxelizerMode::eGeometry;
        }

        LOGW("Invalid voxelizer '{}'; expected auto, comp or geom. Using auto.", it->second);
        return VoxelizerMode::eAuto;
    }

    std::string GetSkyboxModelPath() const
    {
        std::string path    = "";
        auto basePathIt     = m_configData.find("model_base_path");
        auto defaultModelIt = m_configData.find("skybox_model");

        if (basePathIt != m_configData.end() && defaultModelIt != m_configData.end())
        {
            path = basePathIt->second + "/" + defaultModelIt->second + "/glTF/" +
                defaultModelIt->second + ".gltf";
        }
        else
        {
            LOGE("Missing configuration values for GLTF model path.");
        }

        return path;
    }

    std::string GetDefaultGLTFModelPath() const
    {
        std::string path    = "";
        auto basePathIt     = m_configData.find("model_base_path");
        auto defaultModelIt = m_configData.find("default_model");

        if (basePathIt != m_configData.end() && defaultModelIt != m_configData.end())
        {
            path = basePathIt->second + "/" + defaultModelIt->second + "/glTF/" +
                defaultModelIt->second + ".gltf";
        }
        else
        {
            LOGE("Missing configuration values for GLTF model path.");
        }

        return path;
    }

    std::string GetGLTFModelPath(const std::string& name) const
    {
        std::string path = "";
        auto basePathIt  = m_configData.find("model_base_path");

        if (basePathIt != m_configData.end())
        {
            path = basePathIt->second + "/" + name + "/glTF/" + name + ".gltf";
        }
        else
        {
            LOGE("Missing configuration values for GLTF model path.");
        }

        return path;
    }

    ConfigLoader(const ConfigLoader&)            = delete;
    ConfigLoader& operator=(const ConfigLoader&) = delete;

private:
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
            outFile.close();
            LOGI("Default config created at {}.", configPath);
            return;
        }

        LoadConfig(file);
    }

    static std::string Trim(const std::string& text)
    {
        const size_t first = text.find_first_not_of(" \t\r\n");
        if (first == std::string::npos)
        {
            return {};
        }
        return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
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
