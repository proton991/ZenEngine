#pragma once
#include <unordered_map>

namespace zen
{
template <class K, class V> using HashMap = std::unordered_map<K, V>;
}