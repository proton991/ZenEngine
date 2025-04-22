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
} // namespace zen