# Included by ZenTest and ZenSample when configured as the top-level project. The engine is
# then a subdirectory, so a language it enables would not reach their sibling directories.
if(APPLE)
  enable_language(OBJCXX)
  set(CMAKE_OBJCXX_STANDARD 20)
  set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
endif()
set(ZEN_EXTERNAL_SUBPROJECT ON)
include(CTest)
add_subdirectory("${ZEN_ENGINE_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/ZenEngine")
