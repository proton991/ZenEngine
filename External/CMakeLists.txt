# vulkan
find_package(Vulkan)
if (Vulkan_FOUND)
  message(STATUS "Vulkan include directory: ${Vulkan_INCLUDE_DIRS}")
  add_library(vulkan INTERFACE)
  target_sources(vulkan INTERFACE ${Vulkan_INCLUDE_DIRS}/vulkan/vulkan.h)
  target_include_directories(vulkan INTERFACE ${Vulkan_INCLUDE_DIRS})
  target_compile_definitions(vulkan INTERFACE VK_NO_PROTOTYPES)
else ()
  message(FATAL_ERROR "Vulkan SDK Not Installed!")
endif ()


# volk
set(VOLK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/volk")
set(VOLK_FILES
    "${VOLK_DIR}/volk.c"
    "${VOLK_DIR}/volk.h")

add_library(volk STATIC ${VOLK_FILES})
set_target_properties(volk PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(volk PUBLIC ${VOLK_DIR})
target_link_libraries(volk PUBLIC vulkan)

# vma
add_library(vma INTERFACE)
set(VMA_DIR ${CMAKE_CURRENT_SOURCE_DIR}/vma)
target_sources(vma INTERFACE ${VMA_DIR}/include/vk_mem_alloc.h)
target_include_directories(vma INTERFACE ${VMA_DIR}/include)
target_link_libraries(vma INTERFACE vulkan)

# spdlog
add_subdirectory(spdlog)
#add_library(spdlog INTERFACE)
#target_include_directories(spdlog INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/spdlog/include)

# spirv-reflect
set(SPIRV_REFLECT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/spirv-reflect")
set(SPIRV_REFLECT_FILES
    "${SPIRV_REFLECT_DIR}/spirv_reflect.c"
    "${SPIRV_REFLECT_DIR}/spirv_reflect.h")
add_library(spriv-reflect STATIC ${SPIRV_REFLECT_FILES})
target_include_directories(spriv-reflect PUBLIC ${SPIRV_REFLECT_DIR})
# spirv-cross
add_subdirectory(spirv-cross)

# glm
add_library(glm INTERFACE)
set(GLM_DIR ${CMAKE_CURRENT_SOURCE_DIR}/glm)
target_sources(glm INTERFACE ${GLM_DIR}/glm/glm.hpp)
target_include_directories(glm INTERFACE ${GLM_DIR})

# glm
add_library(gli INTERFACE)
set(GLI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/gli)
target_sources(glm INTERFACE ${GLI_DIR}/gli/gli.hpp)
target_include_directories(gli INTERFACE ${GLI_DIR})

target_compile_definitions(glm INTERFACE
    GLM_FORCE_SWIZZLE
    GLM_FORCE_RADIANS
    GLM_FORCE_CTOR_INIT
    GLM_ENABLE_EXPERIMENTAL
    )
if (NOT CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
  target_compile_definitions(glm INTERFACE GLM_FORCE_CXX14)
endif ()

# glfw
add_subdirectory(glfw)

# google test
add_subdirectory(googletest)

add_library(span INTERFACE)
set(SPAN_DIR ${CMAKE_CURRENT_SOURCE_DIR}/span/include)
target_sources(span INTERFACE ${SPAN_DIR}/tcb/span.hpp)
target_include_directories(span INTERFACE ${SPAN_DIR})

# stb
add_library(stb INTERFACE)
target_include_directories(stb INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/stb")

# tinygltf
add_library(tinygltf INTERFACE)
target_include_directories(tinygltf INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/tinygltf")

# fastgltf
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/fastgltf)