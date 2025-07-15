#pragma once
#include <fstream>
#include <sstream>
#include <string>
#include "Common/Errors.h"
#include "Common/HashMap.h"

namespace zen::platform
{
class ConfigLoader
{
public:
    static ConfigLoader& GetInstance()
    {
        static ConfigLoader instance;
        return instance;
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
        std::string path    = "";
        auto basePathIt     = m_configData.find("model_base_path");

        if (basePathIt != m_configData.end())
        {
            path = basePathIt->second + "/" + name + "/glTF/" +
                name + ".gltf";
        }
        else
        {
            LOGE("Missing configuration values for GLTF model path.");
        }

        return path;
    }

    // Delete copy constructor and assignment operator to enforce singleton
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
            outFile << "model_base_path=../../glTF-Sample-Models/2.0" << std::endl;
            outFile << "default_model=Suzanne" << std::endl;
            outFile.close();
            LOGI("Default config created at {}.", configPath);
            return;
        }

        std::string line;
        while (std::getline(file, line))
        {
            std::istringstream lineStream(line);
            std::string key, value;
            if (std::getline(lineStream, key, '=') && std::getline(lineStream, value))
            {
                m_configData[key] = value;
                LOGI("Loaded config: {}={}", key, value);
            }
        }
    }
};
} // namespace zen::platform
