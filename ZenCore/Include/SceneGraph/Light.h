#pragma once
#include "Component.h"
#include "Common/Math.h"
#include "Common/UniquePtr.h"


namespace zen::sg
{
struct LightProperties
{
    Vec3 position;
    Vec4 color;
    Vec4 direction;
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
    Light(std::string name) : Component(std::move(name)) {}

    TypeId GetTypeId() const override
    {
        return typeid(Light);
    }

    static UniquePtr<Light> CreateDirLight(std::string name, const LightProperties& properties)
    {
        auto light = MakeUnique<Light>(std::move(name));
        light->SetProperties(properties);
        light->SetType(LightType::Directional);
        return light;
    }

    static UniquePtr<Light> CreatePointLight(std::string name, const LightProperties& properties)
    {
        auto light = MakeUnique<Light>(std::move(name));
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

    const LightType GetType() const
    {
        return m_type;
    }

private:
    LightProperties m_properties;
    LightType m_type;
};
} // namespace zen::sg