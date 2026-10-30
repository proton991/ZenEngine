#include "Runtime/RuntimeLaunchSettings.h"
#include "Math/Finite.h"
#include <simdjson.h>
#include <atomic>
#include <charconv>
#include <chrono>
#include <fstream>
#include <limits>
#include <type_traits>
#if defined(ZEN_WIN32)
#    include <Windows.h>
#endif

namespace zen::runtime
{
namespace
{
// One field list drives both directions; nested values remain ordinary engine types.
template <class Archive> void Fields(Archive& archive, rc::SceneLight& value)
{
    archive.Field("type", value.type);
    archive.Field("position", value.position);
    archive.Field("direction", value.direction);
    archive.Field("color", value.color);
    archive.Field("intensity", value.intensity);
    archive.Field("range", value.range);
    archive.Field("innerAngleDegrees", value.innerAngleDegrees);
    archive.Field("outerAngleDegrees", value.outerAngleDegrees);
    archive.Field("enabled", value.enabled);
    archive.Field("castsShadows", value.castsShadows);
}

template <class Archive> void Fields(Archive& archive, rc::RenderingLight& value)
{
    archive.Field("id", value.id);
    archive.Field("origin", value.origin);
    archive.Field("light", value.light);
}

template <class Archive> void Fields(Archive& archive, rc::CameraLightSettings& value)
{
    archive.Field("enabled", value.enabled);
    archive.Field("color", value.color);
    archive.Field("intensity", value.intensity);
    archive.Field("range", value.range);
    archive.Field("radius", value.radius);
    archive.Field("followDistance", value.followDistance);
    archive.Field("followCamera", value.followCamera);
    archive.Field("position", value.position);
}

template <class Archive> void Fields(Archive& archive, rc::EnvironmentSettings& value)
{
    archive.Field("texturePath", value.texturePath);
    archive.Field("intensity", value.intensity);
    archive.Field("rotationDegrees", value.rotationDegrees);
    archive.Field("lighting", value.lighting);
    archive.Field("skybox", value.skybox);
}

template <class Archive> void Fields(Archive& archive, rc::DebugSelection& value)
{
    archive.Field("output", value.output);
    archive.Field("linearDepth", value.linearDepth);
    archive.Field("minimum", value.minimum);
    archive.Field("maximum", value.maximum);
    archive.Field("lightId", value.lightId);
    archive.Field("face", value.face);
    archive.Field("axis", value.axis);
    archive.Field("slice", value.slice);
    archive.Field("mip", value.mip);
}

template <class Archive> void Fields(Archive& archive, rc::VoxelGISettings& value)
{
    archive.Field("rayProvider", value.rayProvider);
    archive.Field("featureProviders", value.featureProviders);
    archive.Field("bounceSource", value.bounceSource);
    archive.Field("samples", value.samples);
    archive.Field("adaptiveSampling", value.adaptiveSampling);
    archive.Field("accelerationStructureBudgetMB", value.accelerationStructureBudgetMB);
    archive.Field("referenceSamples", value.referenceSamples);
    archive.Field("historyFrames", value.historyFrames);
    archive.Field("temporal", value.temporal);
    archive.Field("previousFrameRadiance", value.previousFrameRadiance);
    archive.Field("filter", value.filter);
    archive.Field("indirectDiffuse", value.indirectDiffuse);
    archive.Field("specularOcclusion", value.specularOcclusion);
    archive.Field("diffuseResolution", value.diffuseResolution);
    archive.Field("reflections", value.reflections);
    archive.Field("fieldResolution", value.fieldResolution);
    archive.Field("gather", value.gather);
    archive.Field("probeSpacing", value.probeSpacing);
    archive.Field("indirectIntensity", value.indirectIntensity);
    archive.Field("coneAngleDegrees", value.coneAngleDegrees);
    archive.Field("stepScale", value.stepScale);
    archive.Field("normalBiasVoxels", value.normalBiasVoxels);
    archive.Field("maxDistanceGridLengths", value.maxDistanceGridLengths);
    archive.Field("coneCount", value.coneCount);
    archive.Field("maxSteps", value.maxSteps);
    archive.Field("shadows", value.shadows);
    archive.Field("rtShadows", value.rtShadows);
    archive.Field("analyticLighting", value.analyticLighting);
    archive.Field("environmentLighting", value.environmentLighting);
    archive.Field("emissiveLighting", value.emissiveLighting);
}

template <class Archive> void Fields(Archive& archive, rc::VoxelGIRuntimeSettings& value)
{
    archive.Field("resolution", value.resolution);
    archive.Field("cone", value.cone);
    archive.Field("voxelizer", value.voxelizer);
    archive.Field("asyncCompute", value.asyncCompute);
    archive.Field("averagedReflectance", value.averagedReflectance);
    archive.Field("reflectanceBudgetBytes", value.reflectanceBudgetBytes);
    archive.Field("shadowMapResolution", value.shadowMapResolution);
}

template <class Archive> void Fields(Archive& archive, rc::RenderingSettings& value)
{
    archive.Field("scenePath", value.scenePath);
    archive.Field("normalizationCenter", value.normalizationCenter);
    archive.Field("normalizationScale", value.normalizationScale);
    archive.Field("algorithm", value.algorithm);
    archive.Field("lights", value.lights);
    archive.Field("cameraLight", value.cameraLight);
    archive.Field("environment", value.environment);
    archive.Field("gi", value.gi);
    archive.Field("debug", value.debug);
    archive.Field("lightMarkers", value.lightMarkers);
    archive.Field("lightMarkerSize", value.lightMarkerSize);
}

template <class Archive> void Fields(Archive& archive, NavigationCameraState& value)
{
    archive.Field("eye", value.eye);
    archive.Field("target", value.target);
    archive.Field("sceneScale", value.sceneScale);
    archive.Field("aspect", value.aspect);
    archive.Field("verticalFovDegrees", value.verticalFovDegrees);
    archive.Field("nearPlane", value.nearPlane);
    archive.Field("farPlane", value.farPlane);
    archive.Field("orthographic", value.orthographic);
}

template <class Archive> void Fields(Archive& archive, RuntimeRunSettings& value)
{
    archive.Field("windowWidth", value.windowWidth);
    archive.Field("windowHeight", value.windowHeight);
    archive.Field("renderWidth", value.renderWidth);
    archive.Field("renderHeight", value.renderHeight);
    archive.Field("followWindow", value.followWindow);
    archive.Field("vsync", value.vsync);
    archive.Field("presentMode", value.presentMode);
    archive.Field("rhiMode", value.rhiMode);
    archive.Field("validation", value.validation);
    archive.Field("gpuTimings", value.gpuTimings);
    archive.Field("cameraMoveSpeed", value.cameraMoveSpeed);
    archive.Field("framesInFlight", value.framesInFlight);
    archive.Field("workerThreads", value.workerThreads);
    archive.Field("shadowDepthBias", value.shadowDepthBias);
    archive.Field("shadowSlopeBias", value.shadowSlopeBias);
}

template <class Archive> void Fields(Archive& archive, RuntimeLaunchSettings& value)
{
    archive.Field("version", value.version);
    archive.Field("packagePath", value.packagePath);
    archive.Field("sourcePath", value.sourcePath);
    archive.Field("processingProfile", value.processingProfile);
    archive.Field("engineConfiguration", value.engineConfiguration);
    archive.Field("configurationDirectory", value.configurationDirectory);
    archive.Field("defaultEnvironment", value.defaultEnvironment);
    archive.Field("rendering", value.rendering);
    archive.Field("camera", value.camera);
    archive.Field("run", value.run);
    archive.Field("animationEnabled", value.animationEnabled);
    archive.Field("animationClip", value.animationClip);
    archive.Field("animationLoop", value.animationLoop);
}

class JsonWriter
{
public:
    explicit JsonWriter(std::string& output) : m_output(output) {}

    template <class T> void Field(const char* name, T& value)
    {
        m_output += m_first ? "" : ",";
        m_first   = false;
        String(name);
        m_output += ":";
        Value(value);
    }

    void String(std::string_view text)
    {
        constexpr char hex[]  = "0123456789abcdef";
        m_output             += '"';
        for (unsigned char character : text)
        {
            if (character == '"' || character == '\\')
            {
                m_output += '\\';
                m_output += static_cast<char>(character);
            }
            else if (character < 0x20)
            {
                m_output += "\\u00";
                m_output += hex[character >> 4];
                m_output += hex[character & 15];
            }
            else
            {
                m_output += static_cast<char>(character);
            }
        }
        m_output += '"';
    }

    template <class T> void Value(T& value)
    {
        if constexpr (std::is_same_v<T, std::string>)
        {
            String(value);
        }
        else if constexpr (std::is_same_v<T, bool>)
        {
            m_output += value ? "true" : "false";
        }
        else if constexpr (std::is_enum_v<T>)
        {
            uint64_t number = static_cast<uint64_t>(value);
            Value(number);
        }
        else if constexpr (std::is_arithmetic_v<T>)
        {
            char                       buffer[64];
            const std::to_chars_result result = std::to_chars(buffer, buffer + sizeof(buffer), value);
            if (result.ec == std::errc{})
            {
                m_output.append(buffer, result.ptr);
            }
        }
        else
        {
            m_output += "{";
            JsonWriter nested(m_output);
            Fields(nested, value);
            m_output += "}";
        }
    }

    void Value(Vec3& value)
    {
        m_output += "[";
        for (int component = 0; component < 3; ++component)
        {
            m_output += component == 0 ? "" : ",";
            Value(value[component]);
        }
        m_output += "]";
    }

    template <class T, size_t N> void Value(std::array<T, N>& values)
    {
        Array(values);
    }

    template <class T> void Value(HeapVector<T>& values)
    {
        Array(values);
    }

private:
    template <class T> void Array(T& values)
    {
        m_output   += "[";
        bool first  = true;
        for (typename T::value_type& value : values)
        {
            m_output += first ? "" : ",";
            first     = false;
            Value(value);
        }
        m_output += "]";
    }

    std::string& m_output;
    bool         m_first{true};
};

class JsonReader
{
public:
    JsonReader(simdjson::dom::element element, std::string& error, std::string path = "setup") :
        m_error(error), m_path(std::move(path))
    {
        if (element.get_object().get(m_object) != simdjson::SUCCESS)
        {
            Fail(m_path, "expected an object");
        }
    }

    template <class T> void Field(const char* name, T& value)
    {
        ++m_fields;
        if (m_error.empty())
        {
            simdjson::dom::element element;
            const std::string      path  = m_path + "." + name;
            size_t                 count = 0;
            for (simdjson::dom::key_value_pair entry : m_object)
            {
                if (entry.key == name)
                {
                    element = entry.value;
                    ++count;
                }
            }
            if (count != 1)
            {
                Fail(path, count == 0 ? "missing field" : "duplicate field");
            }
            else
            {
                Value(element, value, path);
            }
        }
    }

    void Finish()
    {
        if (m_error.empty() && m_fields != m_object.size())
        {
            Fail(m_path, "unknown fields in this setup version");
        }
    }

private:
    void Fail(const std::string& path, const char* reason)
    {
        if (m_error.empty())
        {
            m_error = path + ": " + reason;
        }
    }

    template <class T> void Value(simdjson::dom::element element, T& value, const std::string& path)
    {
        bool valid = true;
        if constexpr (std::is_same_v<T, std::string>)
        {
            std::string_view text;
            valid = element.get_string().get(text) == simdjson::SUCCESS && text.find('\0') == std::string_view::npos;
            if (valid)
            {
                value.assign(text);
            }
        }
        else if constexpr (std::is_same_v<T, bool>)
        {
            valid = element.get_bool().get(value) == simdjson::SUCCESS;
        }
        else if constexpr (std::is_enum_v<T>)
        {
            uint64_t number = 0;
            valid           = element.get_uint64().get(number) == simdjson::SUCCESS
                           && number <= std::numeric_limits<std::underlying_type_t<T>>::max();
            if (valid)
            {
                value = static_cast<T>(number);
            }
        }
        else if constexpr (std::is_integral_v<T>)
        {
            uint64_t number = 0;
            valid           = element.get_uint64().get(number) == simdjson::SUCCESS && number <= std::numeric_limits<T>::max();
            if (valid)
            {
                value = static_cast<T>(number);
            }
        }
        else if constexpr (std::is_floating_point_v<T>)
        {
            double number = 0;
            valid         = element.get_double().get(number) == simdjson::SUCCESS && IsFinite(number)
                         && std::abs(number) <= std::numeric_limits<T>::max();
            if (valid)
            {
                value = static_cast<T>(number);
            }
        }
        else
        {
            JsonReader nested(element, m_error, path);
            Fields(nested, value);
            nested.Finish();
        }
        if (!valid)
        {
            Fail(path, "invalid value or numeric range");
        }
    }

    void Value(simdjson::dom::element element, Vec3& value, const std::string& path)
    {
        std::array<float, 3> components{};
        Value(element, components, path);
        value = Vec3(components[0], components[1], components[2]);
    }

    template <class T, size_t N> void Value(simdjson::dom::element element, std::array<T, N>& values, const std::string& path)
    {
        simdjson::dom::array array;
        if (element.get_array().get(array) == simdjson::SUCCESS && array.size() == N)
        {
            size_t index = 0;
            for (simdjson::dom::element item : array)
            {
                Value(item, values[index], path + "[" + std::to_string(index) + "]");
                ++index;
            }
        }
        else
        {
            Fail(path, "incorrect array size");
        }
    }

    template <class T> void Value(simdjson::dom::element element, HeapVector<T>& values, const std::string& path)
    {
        simdjson::dom::array array;
        if (element.get_array().get(array) == simdjson::SUCCESS && array.size() <= rc::MaxSceneLights)
        {
            values.resize(array.size());
            size_t index = 0;
            for (simdjson::dom::element item : array)
            {
                Value(item, values[index], path + "[" + std::to_string(index) + "]");
                ++index;
            }
        }
        else
        {
            Fail(path, "invalid light array or capacity");
        }
    }

    simdjson::dom::object m_object;
    std::string&          m_error;
    std::string           m_path;
    size_t                m_fields{0};
};

bool AbsolutePath(const std::string& path)
{
    return !path.empty() && path.find('\0') == std::string::npos && std::filesystem::u8path(path).is_absolute();
}

bool ReplaceSetupFile(const std::filesystem::path& temporary, const std::filesystem::path& destination)
{
#if defined(ZEN_WIN32)
    const bool valid =
        MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    std::error_code error;
    std::filesystem::rename(temporary, destination, error);
    const bool valid = !error;
#endif
    return valid;
}
} // namespace

bool ValidateRuntimeLaunchSettings(const RuntimeLaunchSettings& settings, std::string& error)
{
    error.clear();
    const RuntimeRunSettings& run = settings.run;
    if (settings.version != RuntimeSetupVersion)
    {
        error = "Unsupported runtime setup version.";
    }
    else if (!AbsolutePath(settings.packagePath) || std::filesystem::u8path(settings.packagePath).extension() != ".zenasset"
             || !AbsolutePath(settings.configurationDirectory))
    {
        error = "Setup needs absolute package/configuration paths.";
    }
    else if (!settings.rendering.environment.texturePath.empty() && !AbsolutePath(settings.rendering.environment.texturePath))
    {
        error = "Runtime environment paths must be absolute.";
    }
    else if (!ValidateNavigationCameraState(settings.camera))
    {
        error = "Invalid runtime camera pose or projection.";
    }
    else if (run.windowWidth == 0 || run.windowHeight == 0 || run.renderWidth == 0 || run.renderHeight == 0
             || run.windowWidth > 16384 || run.windowHeight > 16384 || run.renderWidth > 16384 || run.renderHeight > 16384
             || run.presentMode > RHIPresentMode::eImmediate || run.rhiMode > RHIExecutionMode::eThreaded
             || !IsFinite(run.cameraMoveSpeed) || run.cameraMoveSpeed < kMinNavigationCameraMoveSpeed
             || run.cameraMoveSpeed > kMaxNavigationCameraMoveSpeed || run.framesInFlight == 0 || run.framesInFlight > 4
             || run.workerThreads == 0 || run.workerThreads > 256 || !IsFinite(run.shadowDepthBias)
             || !IsFinite(run.shadowSlopeBias))
    {
        error = "Invalid runtime window, frame, timing or renderer options.";
    }
    else
    {
        rc::ValidateRenderingSettings(settings.rendering, error);
    }
    const std::string* strings[] = {&settings.packagePath,
                                    &settings.sourcePath,
                                    &settings.processingProfile,
                                    &settings.engineConfiguration,
                                    &settings.configurationDirectory,
                                    &settings.defaultEnvironment,
                                    &settings.rendering.scenePath,
                                    &settings.rendering.environment.texturePath};
    for (const std::string* text : strings)
    {
        if (error.empty() && (text->find('\0') != std::string::npos || !simdjson::validate_utf8(text->data(), text->size())))
        {
            error = "Setup strings must be valid UTF-8 without embedded NUL characters.";
        }
    }
    return error.empty();
}

bool EncodeRuntimeLaunchSettings(const RuntimeLaunchSettings& settings, std::string& json, std::string& error)
{
    bool valid = ValidateRuntimeLaunchSettings(settings, error);
    if (valid)
    {
        RuntimeLaunchSettings copy = settings;
        std::string           encoded;
        JsonWriter            writer(encoded);
        writer.Value(copy);
        valid = encoded.size() <= MaxRuntimeSetupBytes && simdjson::validate_utf8(encoded.data(), encoded.size());
        if (valid)
        {
            json = std::move(encoded);
        }
        else
        {
            error = "Setup exceeds its size limit or contains invalid UTF-8.";
        }
    }
    return valid;
}

bool DecodeRuntimeLaunchSettings(std::string_view json, RuntimeLaunchSettings& settings, std::string& error)
{
    error.clear();
    simdjson::dom::parser  parser;
    simdjson::dom::element element;
    RuntimeLaunchSettings  candidate;
    bool                   valid = !json.empty() && json.size() <= MaxRuntimeSetupBytes;
    if (valid)
    {
        valid = parser.parse(json.data(), json.size()).get(element) == simdjson::SUCCESS;
    }
    if (valid)
    {
        JsonReader reader(element, error);
        Fields(reader, candidate);
        reader.Finish();
        valid = error.empty() && ValidateRuntimeLaunchSettings(candidate, error);
    }
    if (valid)
    {
        settings = std::move(candidate);
    }
    else if (error.empty())
    {
        error = "Malformed JSON or runtime setup exceeds its size limit.";
    }
    return valid;
}

bool ReadRuntimeLaunchSettings(const std::filesystem::path& path, RuntimeLaunchSettings& settings, std::string& error)
{
    std::error_code fileError;
    const uintmax_t size  = std::filesystem::file_size(path, fileError);
    bool            valid = !fileError && size != 0 && size <= MaxRuntimeSetupBytes;
    if (valid)
    {
        std::string   json(static_cast<size_t>(size), '\0');
        std::ifstream file(path, std::ios::binary);
        valid = bool(file.read(json.data(), static_cast<std::streamsize>(json.size())));
        if (valid)
        {
            valid = DecodeRuntimeLaunchSettings(json, settings, error);
        }
        else
        {
            error = "Could not read the complete runtime setup.";
        }
    }
    else
    {
        error = "Runtime setup is missing, empty or exceeds its size limit.";
    }
    return valid;
}

bool WriteRuntimeLaunchSettings(const std::filesystem::path& path, const RuntimeLaunchSettings& settings, std::string& error)
{
    std::string json;
    bool        valid = EncodeRuntimeLaunchSettings(settings, json, error);
    if (valid)
    {
        static std::atomic<uint64_t> sequence{0};
        const int64_t                stamp      = std::chrono::steady_clock::now().time_since_epoch().count();
        std::filesystem::path        temporary  = path;
        temporary                              += ".tmp-" + std::to_string(stamp) + "-" + std::to_string(sequence.fetch_add(1));
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        file.write(json.data(), static_cast<std::streamsize>(json.size()));
        file.flush();
        valid = bool(file);
        file.close();
        valid = valid && !file.fail() && ReplaceSetupFile(temporary, path);
        if (!valid)
        {
            std::error_code cleanupError;
            std::filesystem::remove(temporary, cleanupError);
            error = "Could not atomically publish the runtime setup.";
        }
    }
    return valid;
}
} // namespace zen::runtime
