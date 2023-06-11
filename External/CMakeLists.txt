###########################
#         glfw            #
###########################
add_library(glfw INTERFACE)
set(GLFW_DIR ${CMAKE_CURRENT_SOURCE_DIR}/glfw)
target_include_directories(glfw INTERFACE ${GLFW_DIR}/Include)
target_link_directories(glfw INTERFACE ${GLFW_DIR}/Lib)
target_link_libraries(glfw INTERFACE glfw3)
if (UNIX)
    target_link_libraries(glfw INTERFACE X11)
endif (UNIX)


###########################
#       glm               #
###########################
add_library(glm INTERFACE)
target_include_directories(glm INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/glm/Include)
target_compile_definitions(glm INTERFACE
        GLM_FORCE_SWIZZLE
        GLM_FORCE_RADIANS
        GLM_FORCE_CTOR_INIT
        GLM_ENABLE_EXPERIMENTAL
        )


###########################
#         spdlog          #
###########################
add_library(spdlog INTERFACE)
target_include_directories(spdlog INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/spdlog/Include)


###########################
#           vma           #
###########################
add_library(vma INTERFACE)
target_include_directories(vma INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/vma/Include)


###########################
#         glslang         #
###########################
option(ENABLE_SPVREMAPPER OFF)
option(ENABLE_GLSLANG_BINARIES OFF)
set(ENABLE_HLSL ON)
option(ENABLE_OPT OFF)
option(BUILD_TESTING OFF)
option(BUILD_EXTERNAL OFF)

add_subdirectory(glslang)

if (NOT TARGET glslang-default-resource-limits)
    add_library(glslang-default-resource-limits
            glslang/StandAlone/ResourceLimits.cpp)

    set_target_properties(glslang-default-resource-limits PROPERTIES FOLDER "ThirdParty" POSITION_INDEPENDENT_CODE ON)

    target_include_directories(glslang-default-resource-limits
            PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/glslang)
endif ()

if (NOT MSVC)
    target_compile_options(glslang PRIVATE
            "-Wno-logical-op-parentheses"
            "-Wno-unused-parameter")

    target_compile_options(SPIRV PRIVATE
            "-Wno-logical-op-parentheses"
            "-Wno-unused-parameter")
endif ()

set_target_properties(glslang PROPERTIES FOLDER "ThirdParty" POSITION_INDEPENDENT_CODE ON)
set_target_properties(OGLCompiler PROPERTIES FOLDER "ThirdParty" POSITION_INDEPENDENT_CODE ON)
set_target_properties(OSDependent PROPERTIES FOLDER "ThirdParty" POSITION_INDEPENDENT_CODE ON)
set_target_properties(SPIRV PROPERTIES FOLDER "ThirdParty" POSITION_INDEPENDENT_CODE ON)

###########################
#       spirv-cross       #
###########################
# spirv-cross
add_subdirectory(spirv-cross)

set_target_properties(spirv-cross PROPERTIES FOLDER "ThirdParty" POSITION_INDEPENDENT_CODE ON)
set_target_properties(spirv-cross-core PROPERTIES FOLDER "ThirdParty" POSITION_INDEPENDENT_CODE ON)
set_target_properties(spirv-cross-glsl PROPERTIES FOLDER "ThirdParty" POSITION_INDEPENDENT_CODE ON)
set_target_properties(spirv-cross-cpp PROPERTIES FOLDER "ThirdParty" POSITION_INDEPENDENT_CODE ON)
set_target_properties(spirv-cross-hlsl PROPERTIES FOLDER "ThirdParty" POSITION_INDEPENDENT_CODE ON)
set_target_properties(spirv-cross-msl PROPERTIES FOLDER "ThirdParty" POSITION_INDEPENDENT_CODE ON)
set_target_properties(spirv-cross-reflect PROPERTIES FOLDER "ThirdParty" POSITION_INDEPENDENT_CODE ON)
set_target_properties(spirv-cross-util PROPERTIES FOLDER "ThirdParty" POSITION_INDEPENDENT_CODE ON)