#pragma once
#include <functional>
#include <glm/gtx/hash.hpp>
namespace {
template <class T>
inline void HashCombine(size_t& seed, const T& v) {
  std::hash<T> hasher;
  glm::detail::hash_combine(seed, hasher(v));
}

template <class T>
inline std::vector<uint8_t> ToBytes(const T& value) {
  return {reinterpret_cast<uint8_t*>(&value), reinterpret_cast<uint8_t*>(&value) + sizeof(T)};
}

template <class T>
uint32_t ToU32(T value) {
  static_assert(std::is_arithmetic<T>::value, "T must be numeric");

  if (static_cast<uintmax_t>(value) >
      static_cast<uintmax_t>(std::numeric_limits<uint32_t>::max())) {
    throw std::runtime_error("to_u32() failed, value is too big to be converted to uint32_t");
  }

  return static_cast<uint32_t>(value);
}
}  // namespace