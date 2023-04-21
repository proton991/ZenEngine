#pragma once
#define ZEN_NO_COPY_MOVE(ClassName)                \
  ClassName(const ClassName&)            = delete; \
  ClassName(ClassName&&)                 = delete; \
  ClassName& operator=(const ClassName&) = delete; \
  ClassName& operator=(ClassName&&)      = delete;

#define ZEN_NO_COPY(ClassName)                     \
  ClassName(const ClassName&)            = delete; \
  ClassName& operator=(const ClassName&) = delete;

#define ZEN_NO_MOVE(ClassName)                \
  ClassName(ClassName&&)            = delete; \
  ClassName& operator=(ClassName&&) = delete;