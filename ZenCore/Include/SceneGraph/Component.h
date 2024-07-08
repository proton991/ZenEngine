#pragma once
#include <string>
#include <typeindex>

namespace zen::sg
{
using TypeId = std::type_index;
class Component
{
public:
    Component() = default;

    virtual ~Component() = default;

    Component(std::string name) : m_name(std::move(name)) {}

    Component(Component&& other) = default;

    const std::string& GetName() const
    {
        return m_name;
    }

    virtual TypeId GetTypeId() const = 0;

private:
    std::string m_name;
};
} // namespace zen::sg