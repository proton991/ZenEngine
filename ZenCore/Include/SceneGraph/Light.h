#pragma once
#include "Component.h"
#include "Math/Math.h"



namespace zen::sg
{
struct LightProperties
{
    Vec3 position{0.0f};
    Vec4 color{1.0f};
    Vec4 direction{0.0f, 0.0f, -1.0f, 0.0f};
    float intensity{1.0f};
    // Zero means unlimited range, as in a glTF light with no range property.
    float range{0.0f};
    float innerConeAngle{0.0f};
    float outerConeAngle{glm::quarter_pi<float>()};
};
enum LightType
{
    Directional = 0,
    Point       = 1,
    Spot        = 2,
    // Insert new light type here
    Max
};
// Only support static light for now
class Light : public Component
{
public:
    uint32_t sourceIndex{UINT32_MAX};

    float unitScale{1.0f};

    Light(std::string name) : Component(std::move(name)) {}

    TypeId GetTypeId() const override
    {
        return typeid(Light);
    }

    static UniquePtr<Light> CreateDirLight(std::string name, const LightProperties& properties)
    {
        UniquePtr<Light> light = MakeUnique<Light>(std::move(name));
        light->SetProperties(properties);
        light->SetType(LightType::Directional);
        return light;
    }

    static UniquePtr<Light> CreatePointLight(std::string name, const LightProperties& properties)
    {
        UniquePtr<Light> light = MakeUnique<Light>(std::move(name));
        light->SetProperties(properties);
        light->SetType(LightType::Point);
        return light;
    }

    void SetType(LightType type)
    {
        m_type = type;
    }

    void SetProperties(const LightProperties& properties)
    {
        m_properties = properties;
    }

    const LightProperties& GetProperties() const
    {
        return m_properties;
    }

    LightType GetType() const
    {
        return m_type;
    }

private:
    LightProperties m_properties;
    LightType m_type{LightType::Point};
};
} // namespace zen::sg
