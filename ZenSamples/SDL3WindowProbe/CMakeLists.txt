cmake_minimum_required(VERSION 3.24)
project(ZenSDL3WindowProbe LANGUAGES CXX)

# Standalone evaluation: deliberately not included by the engine build.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(ZEN_IMGUI_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../External/imgui" CACHE PATH "Existing v1.92.9b-docking ImGui source directory")
if(NOT EXISTS "${ZEN_IMGUI_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp")
  message(FATAL_ERROR "Set ZEN_IMGUI_SOURCE_DIR to the engine's existing ImGui checkout")
endif()
file(STRINGS "${ZEN_IMGUI_SOURCE_DIR}/imgui.h" IMGUI_VERSION_LINE REGEX "^#define IMGUI_VERSION +\"1\\.92\\.9b\"")
if(NOT IMGUI_VERSION_LINE)
  message(FATAL_ERROR "Use the engine's pinned ImGui 1.92.9b source, not an old build cache")
endif()

set(SDL_CPU_X64 TRUE) # Official VC package requires an architecture selection.
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8 OR NOT WIN32)
  message(FATAL_ERROR "This native acceptance probe currently targets Windows x64")
endif()
find_package(SDL3 3.4.18 EXACT CONFIG REQUIRED)
find_package(Vulkan REQUIRED)

add_executable(SDL3WindowProbe
  Probe.cpp
  "${ZEN_IMGUI_SOURCE_DIR}/imgui.cpp"
  "${ZEN_IMGUI_SOURCE_DIR}/imgui_draw.cpp"
  "${ZEN_IMGUI_SOURCE_DIR}/imgui_tables.cpp"
  "${ZEN_IMGUI_SOURCE_DIR}/imgui_widgets.cpp"
  "${ZEN_IMGUI_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp")
target_include_directories(SDL3WindowProbe PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../../ZenCore/Include"
  "${ZEN_IMGUI_SOURCE_DIR}")
target_link_libraries(SDL3WindowProbe PRIVATE SDL3::SDL3 Vulkan::Vulkan)
target_compile_definitions(SDL3WindowProbe PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN
  IMGUI_USER_CONFIG="${CMAKE_CURRENT_SOURCE_DIR}/../../ZenUI/ImGui/Include/ImGui/ImGuiConfig.h")
add_custom_command(TARGET SDL3WindowProbe POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "$<TARGET_FILE:SDL3::SDL3>" "$<TARGET_FILE_DIR:SDL3WindowProbe>"
  VERBATIM)

enable_testing()
add_test(NAME SDL3WindowProbe COMMAND SDL3WindowProbe)
set_tests_properties(SDL3WindowProbe PROPERTIES TIMEOUT 30)
