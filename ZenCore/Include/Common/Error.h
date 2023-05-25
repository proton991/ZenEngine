#pragma once
#include "Logging.h"
#ifdef ZEN_DEBUG
#define VK_ASSERT(x)                                      \
  do {                                                    \
    if (!bool(x)) {                                       \
      LOGE("Vulkan error at {}:{}.", __FILE__, __LINE__); \
      abort();                                            \
    }                                                     \
  } while (0)
// clang-format off
#define VK_CHECK(x, op)                       \
  do {                                        \
    VkResult err = static_cast<VkResult>(x);  \
    if (err != VK_SUCCESS) {                  \
      LOGE("Failed to {}", op);               \
      abort();                                \
    }                                         \
  }                                           \
  while (0)
#else
#define VK_ASSERT(x) ((void)0)
#define VK_CHECK(err, op) ((void)0)
#endif
// clang-format on
