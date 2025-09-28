#pragma once
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

namespace zen
{
using Vec2  = glm::vec2;
using Vec3  = glm::vec3;
using Vec4  = glm::vec4;
using Mat3  = glm::mat3;
using Mat4  = glm::mat4;
using Quat  = glm::quat;
using Vec3i = glm::i32vec3;

template <class T> struct Rect2
{
    T minX, maxX;
    T minY, maxY;

    Rect2() : minX(0), maxX(0), minY(0), maxY(0) {}

    Rect2(T width, T height) : minX(0), maxX(width), minY(0), maxY(height) {}

    Rect2(T _minX, T _maxX, T _minY, T _maxY) : minX(_minX), maxX(_maxX), minY(_minY), maxY(_maxY)
    {}

    bool operator==(const Rect2& b) const
    {
        return minX == b.minX && minY == b.minY && maxX == b.maxX && maxY == b.maxY;
    }

    bool operator!=(const Rect2& b) const
    {
        return !(*this == b);
    }

    [[nodiscard]] T Width() const
    {
        return maxX - minX;
    }

    [[nodiscard]] T Height() const
    {
        return maxY - minY;
    }
};

using Rect2i = Rect2<int>;
using Rect2f = Rect2<float>;

struct Rect3f
{
    float minX, maxX;
    float minY, maxY;
    float minZ, maxZ;

    Rect3f() : minX(0.f), maxX(0.f), minY(0.f), maxY(0.f), minZ(0.f), maxZ(1.f) {}

    Rect3f(float width, float height) :
        minX(0.f), maxX(width), minY(0.f), maxY(height), minZ(0.f), maxZ(1.f)
    {}

    Rect3f(float _minX, float _maxX, float _minY, float _maxY, float _minZ, float _maxZ) :
        minX(_minX), maxX(_maxX), minY(_minY), maxY(_maxY), minZ(_minZ), maxZ(_maxZ)
    {}

    bool operator==(const Rect3f& b) const
    {
        return minX == b.minX && minY == b.minY && minZ == b.minZ && maxX == b.maxX &&
            maxY == b.maxY && maxZ == b.maxZ;
    }

    bool operator!=(const Rect3f& b) const
    {
        return !(*this == b);
    }

    [[nodiscard]] float Width() const
    {
        return maxX - minX;
    }

    [[nodiscard]] float Height() const
    {
        return maxY - minY;
    }
};
} // namespace zen