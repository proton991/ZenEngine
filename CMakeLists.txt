cmake_minimum_required(VERSION 3.23)
project(ZenEngine VERSION 0.1)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)

set(VULKAN_INCLUDE_DIR $ENV{VULKAN_SDK}/Include)
find_package(Vulkan REQUIRED)
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
add_subdirectory(ZenCore)
add_subdirectory(ZenSamples)
add_subdirectory(External)