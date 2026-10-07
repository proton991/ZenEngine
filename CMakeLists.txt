cmake_minimum_required(VERSION 3.23)
project(ZenEngine VERSION 0.2)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin" CACHE PATH "Runtime output directory")
option(ZEN_BUILD_RUNTIME_UI "Build the optional runtime debug UI and shared UI integration" ON)
option(ZEN_BUILD_EDITOR "Build the ZenEditor scene viewer" ON)
# Keep GLFW selectable until native macOS acceptance is complete.
set(ZEN_WINDOW_BACKEND "SDL3" CACHE STRING "Window backend: SDL3 or GLFW")
set_property(CACHE ZEN_WINDOW_BACKEND PROPERTY STRINGS SDL3 GLFW)
if (NOT ZEN_WINDOW_BACKEND MATCHES "^(SDL3|GLFW)$")
  message(FATAL_ERROR "ZEN_WINDOW_BACKEND must be SDL3 or GLFW")
endif ()
# The SDL backend's Cocoa title-bar adapter is Objective-C++. Enable it here, above
# every target that links ZenCore.
if (APPLE AND ZEN_WINDOW_BACKEND STREQUAL "SDL3")
  enable_language(OBJCXX)
endif ()
include(CTest)

find_package(Vulkan REQUIRED)
set(VULKAN_INCLUDE_DIR "${Vulkan_INCLUDE_DIR}")
message(STATUS "${PROJECT_NAME} version: ${PROJECT_VERSION}")
message(STATUS "Vulkan include directory: ${VULKAN_INCLUDE_DIR}")
message(STATUS "Vulkan lib directory: ${Vulkan_LIBRARIES}")
find_program(GLSL_VALIDATOR glslangValidator HINTS /usr/bin /usr/local/bin $ENV{VULKAN_SDK}/Bin/ $ENV{VULKAN_SDK}/Bin32/)
if (NOT GLSL_VALIDATOR)
  message(FATAL_ERROR "glslangValidator not found!")
else ()
  message(STATUS "Found glslangValidator: ${GLSL_VALIDATOR}")
endif ()

############################################################
#                    Add Sub Directories                   #
############################################################
add_subdirectory(External)
add_subdirectory(ZenCore)
# Libraries before their consumers: the samples link the optional runtime UI.
if (ZEN_BUILD_RUNTIME_UI OR ZEN_BUILD_EDITOR)
  add_subdirectory(ZenUI)
endif ()
add_subdirectory(ZenSamples)
if (ZEN_BUILD_EDITOR)
  add_subdirectory(ZenEditor)
endif ()

# Platform Definitions
if(${CMAKE_SYSTEM_NAME} STREQUAL "Windows")
  message(STATUS "Detected Windows platform")
  target_compile_definitions(ZenCore PUBLIC ZEN_WIN32)
endif()

if(${CMAKE_SYSTEM_NAME} STREQUAL "Darwin")
  message(STATUS "Detected MacOS platform")
  target_compile_definitions(ZenCore PUBLIC ZEN_MACOS)
endif()
