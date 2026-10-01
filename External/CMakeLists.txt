include(FetchContent)
# Show FetchContent's download, checksum verification and extraction progress.
set(FETCHCONTENT_QUIET OFF)
if (POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif ()

# Preserve source folder names; build/download state stays in each build tree.
# Archives are pinned and verified, following the Dear ImGui setup.
function(zen_declare_dependency NAME FOLDER URL SHA256)
    get_filename_component(REVISION "${URL}" NAME)
    message(STATUS "External dependency ${FOLDER} (${REVISION}): ${URL}")
    FetchContent_Declare(${NAME}
        URL "${URL}"
        URL_HASH "SHA256=${SHA256}"
        SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/${FOLDER}"
        BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/${FOLDER}"
        ${ARGN})
endfunction()

# SOURCE_SUBDIR skips upstream builds for libraries wrapped by engine targets.
# volk: 1.4.350
zen_declare_dependency(volk volk
    https://codeload.github.com/zeux/volk/zip/refs/tags/1.4.350
    da737c3ea86880a4a9bbede7696e46955425d7ed373b7b2225c5bf2f23343e60
    SOURCE_SUBDIR zen-no-cmake)
# vma: v3.4.0
zen_declare_dependency(vma vma
    https://codeload.github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/zip/refs/tags/v3.4.0
    ce9a61ea5e72284ac9ccc152c214eef381ab57bfd7383f20232e5a652e3afda6
    SOURCE_SUBDIR zen-no-cmake)
# spdlog: v1.17.0
zen_declare_dependency(spdlog spdlog
    https://codeload.github.com/gabime/spdlog/zip/refs/tags/v1.17.0
    b11912a82d149792fef33fabd0503b13d54aeac25c1464755461d4108ea71fc2)
# spirv-reflect: vulkan-sdk-1.4.363.0
zen_declare_dependency(spirv_reflect spirv-reflect
    https://codeload.github.com/KhronosGroup/SPIRV-Reflect/zip/refs/tags/vulkan-sdk-1.4.363.0
    a4654a90fe938a3cba55952776f50f0f0a005143371ef6d076578ab1f41afce1
    SOURCE_SUBDIR zen-no-cmake)
# glm: 1.0.3
zen_declare_dependency(glm glm
    https://codeload.github.com/g-truc/glm/zip/refs/tags/1.0.3
    3aa4347b8f13cba882df1c7b61a6ca910c75a875c56ec3d75d7dc9ae8eac34df
    SOURCE_SUBDIR zen-no-cmake)
# GLI has no recent release; retain the upstream commit with GLM 1.x support.
# gli: 3542f8830178061e0661f5df1e89d36ba7d7b0ab
zen_declare_dependency(gli gli
    https://codeload.github.com/g-truc/gli/zip/3542f8830178061e0661f5df1e89d36ba7d7b0ab
    482302cff80aeed356a57ecf5b8ef04c0ef2334648e15d48aa7bdc43df88f38d
    SOURCE_SUBDIR zen-no-cmake)
# glfw: 3.5.1
zen_declare_dependency(glfw glfw
    https://codeload.github.com/glfw/glfw/zip/refs/tags/3.5.1
    e9a80355e8a0c59b15ae8576c2c3aeae792c2b1082ec426dc93bde70d5017fda)
# stb: 2c980bb59875b0d32144a71867fbdebb2f77cd20
zen_declare_dependency(stb stb
    https://codeload.github.com/nothings/stb/zip/2c980bb59875b0d32144a71867fbdebb2f77cd20
    8e59f72b0780690cda64726804269f638a3be77b9d1506ea95f443f7964bccf0
    SOURCE_SUBDIR zen-no-cmake)
# Use fastgltf's supported simdjson version with a checked archive too.
# simdjson: v4.6.11
zen_declare_dependency(simdjson simdjson
    https://codeload.github.com/simdjson/simdjson/zip/refs/tags/v4.6.11
    5a3bc470d53b0324dfcbef952fdbd004655d7a4f7a6558fb94122cb9f233f2d0)
# fastgltf: v0.9.1
zen_declare_dependency(fastgltf fastgltf
    https://codeload.github.com/spnda/fastgltf/zip/refs/tags/v0.9.1
    fb3bd2421064cd4d9ec0a774c643c0e174595337dc06ba75b73ec73542caba2d)

# glTF compression and image formats are decoded to the engine's ordinary buffers.
zen_declare_dependency(draco draco
    https://codeload.github.com/google/draco/zip/refs/tags/1.5.7
    27b72ba2d5ff3d0a9814ad40d4cb88f8dc89a35491c0866d952473f8f9416b77)
zen_declare_dependency(meshoptimizer meshoptimizer
    https://codeload.github.com/zeux/meshoptimizer/zip/refs/tags/v1.1
    6aecc2d3b4328f1f5f4127fb16a144de80cc9eb35c32387807c8c04b0b6dbbf3)
zen_declare_dependency(libwebp libwebp
    https://codeload.github.com/webmproject/libwebp/zip/refs/tags/v1.6.0
    7a89bb7a6a0c30b5c0f521c2673635381b4e7c916585f4a1f702d3b47f15d5e5)
zen_declare_dependency(basis_universal basis_universal
    https://codeload.github.com/BinomialLLC/basis_universal/zip/refs/tags/v1_60
    f79c13343ddab36f3d37629775b9ca8e2fbf55a758d505e2150a8b7faee17d2a
    SOURCE_SUBDIR zen-no-cmake)

set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "Build GLFW examples" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "Build GLFW tests" FORCE)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "Build GLFW docs" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "Install GLFW" FORCE)
set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "Build spdlog examples" FORCE)
set(SPDLOG_BUILD_EXAMPLE_HO OFF CACHE BOOL "Build header-only spdlog examples" FORCE)
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "Build spdlog tests" FORCE)
set(SPDLOG_BUILD_TESTS_HO OFF CACHE BOOL "Build header-only spdlog tests" FORCE)
set(SPDLOG_BUILD_BENCH OFF CACHE BOOL "Build spdlog benchmarks" FORCE)
set(SPDLOG_INSTALL OFF CACHE BOOL "Install spdlog" FORCE)
set(SIMDJSON_DEVELOPER_MODE OFF CACHE BOOL "Build simdjson tests and tools" FORCE)
set(SIMDJSON_INSTALL OFF CACHE BOOL "Install simdjson" FORCE)
set(FASTGLTF_ENABLE_TESTS OFF CACHE BOOL "Build fastgltf tests" FORCE)
set(FASTGLTF_ENABLE_EXAMPLES OFF CACHE BOOL "Build fastgltf examples" FORCE)
set(FASTGLTF_ENABLE_DOCS OFF CACHE BOOL "Build fastgltf docs" FORCE)
set(FASTGLTF_ENABLE_INSTALL OFF CACHE BOOL "Install fastgltf" FORCE)
FetchContent_MakeAvailable(volk vma spdlog spirv_reflect glm gli glfw stb simdjson fastgltf)

set(DRACO_TESTS OFF CACHE BOOL "Build Draco tests" FORCE)
set(DRACO_INSTALL OFF CACHE BOOL "Install Draco" FORCE)
set(DRACO_JS_GLUE OFF CACHE BOOL "Build Draco JavaScript bindings" FORCE)
set(MESHOPT_BUILD_DEMO OFF CACHE BOOL "Build meshoptimizer demo" FORCE)
set(MESHOPT_BUILD_GLTFPACK OFF CACHE BOOL "Build gltfpack" FORCE)
set(MESHOPT_INSTALL OFF CACHE BOOL "Install meshoptimizer" FORCE)
foreach(WEBP_TOOL ANIM_UTILS CWEBP DWEBP GIF2WEBP IMG2WEBP VWEBP WEBPINFO LIBWEBPMUX WEBPMUX EXTRAS)
    set(WEBP_BUILD_${WEBP_TOOL} OFF CACHE BOOL "Build WebP utilities" FORCE)
endforeach()
FetchContent_MakeAvailable(draco meshoptimizer libwebp basis_universal)

add_library(zen_basisu_transcoder STATIC
    "${basis_universal_SOURCE_DIR}/transcoder/basisu_transcoder.cpp"
    "${basis_universal_SOURCE_DIR}/zstd/zstddeclib.c")
target_include_directories(zen_basisu_transcoder PUBLIC "${basis_universal_SOURCE_DIR}/transcoder")
target_compile_definitions(zen_basisu_transcoder PUBLIC BASISD_SUPPORT_KTX2_ZSTD=1)
add_library(zen_gltf_decoders INTERFACE)
target_include_directories(zen_gltf_decoders INTERFACE
    "${libwebp_SOURCE_DIR}/src" "${draco_SOURCE_DIR}/src" "${CMAKE_BINARY_DIR}")
target_link_libraries(zen_gltf_decoders INTERFACE draco::draco meshoptimizer webpdecoder zen_basisu_transcoder simdjson)

add_library(vulkan INTERFACE)
target_include_directories(vulkan INTERFACE "${Vulkan_INCLUDE_DIR}")
target_compile_definitions(vulkan INTERFACE VK_NO_PROTOTYPES)

add_library(volk STATIC "${volk_SOURCE_DIR}/volk.c")
set_target_properties(volk PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(volk PUBLIC "${volk_SOURCE_DIR}")
target_link_libraries(volk PUBLIC vulkan ${CMAKE_DL_LIBS})

add_library(vma INTERFACE)
target_include_directories(vma INTERFACE "${vma_SOURCE_DIR}/include")
target_link_libraries(vma INTERFACE vulkan)

add_library(spirv-reflect STATIC "${spirv_reflect_SOURCE_DIR}/spirv_reflect.c")
target_include_directories(spirv-reflect PUBLIC "${spirv_reflect_SOURCE_DIR}")

add_library(glm INTERFACE)
target_include_directories(glm INTERFACE "${glm_SOURCE_DIR}")
target_compile_definitions(glm INTERFACE
    GLM_FORCE_SWIZZLE GLM_FORCE_RADIANS GLM_FORCE_CTOR_INIT GLM_ENABLE_EXPERIMENTAL)
if (NOT CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
    target_compile_definitions(glm INTERFACE GLM_FORCE_CXX14)
endif ()

add_library(gli INTERFACE)
target_include_directories(gli INTERFACE "${gli_SOURCE_DIR}")
target_link_libraries(gli INTERFACE glm)

add_library(stb INTERFACE)
target_include_directories(stb INTERFACE "${stb_SOURCE_DIR}")

if (BUILD_TESTING)
    # googletest: v1.18.0
    zen_declare_dependency(googletest googletest
        https://codeload.github.com/google/googletest/zip/refs/tags/v1.18.0
        63b9c77751a5b8f492486005f67533fdc58682b67476fcb91650be5958d5195a)
    set(BUILD_GMOCK OFF CACHE BOOL "Build GoogleMock" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "Install GoogleTest" FORCE)
    set(gtest_force_shared_crt ON CACHE BOOL "Use the engine's DLL runtime" FORCE)
    FetchContent_MakeAvailable(googletest)
endif ()

if (ZEN_BUILD_LEGACY)
    # spirv-cross: vulkan-sdk-1.4.363.0
    zen_declare_dependency(spirv_cross spirv-cross
        https://codeload.github.com/KhronosGroup/SPIRV-Cross/zip/refs/tags/vulkan-sdk-1.4.363.0
        637ee99192da3cea88bb81c97bcb7177d7b0010f75d2afd6c97f8e7b7d2a0c90)
    # Latest compatible C++ release; TinyGLTF 3 uses a different C API.
    zen_declare_dependency(tinygltf tinygltf
        https://codeload.github.com/syoyo/tinygltf/zip/refs/tags/v2.9.7
        865ae4a220ff8bebc455f098e7c08bcb9e2701ef9eaef3519e9cc05ffd824dc9
        SOURCE_SUBDIR zen-no-cmake)
    set(SPIRV_CROSS_CLI OFF CACHE BOOL "Build SPIRV-Cross tools" FORCE)
    set(SPIRV_CROSS_ENABLE_TESTS OFF CACHE BOOL "Build SPIRV-Cross tests" FORCE)
    set(SPIRV_CROSS_ENABLE_HLSL OFF CACHE BOOL "Build SPIRV-Cross HLSL backend" FORCE)
    set(SPIRV_CROSS_ENABLE_MSL OFF CACHE BOOL "Build SPIRV-Cross MSL backend" FORCE)
    set(SPIRV_CROSS_ENABLE_CPP OFF CACHE BOOL "Build SPIRV-Cross C++ backend" FORCE)
    set(SPIRV_CROSS_ENABLE_REFLECT OFF CACHE BOOL "Build SPIRV-Cross JSON reflection" FORCE)
    set(SPIRV_CROSS_ENABLE_C_API OFF CACHE BOOL "Build SPIRV-Cross C API" FORCE)
    set(SPIRV_CROSS_ENABLE_UTIL OFF CACHE BOOL "Build SPIRV-Cross utilities" FORCE)
    FetchContent_MakeAvailable(spirv_cross tinygltf)
    add_library(tinygltf INTERFACE)
    target_include_directories(tinygltf INTERFACE "${tinygltf_SOURCE_DIR}")
    target_link_libraries(tinygltf INTERFACE stb)
endif ()

if (ZEN_BUILD_RUNTIME_UI)
    # imgui: v1.92.9b-docking
    zen_declare_dependency(zen_imgui imgui
        https://codeload.github.com/ocornut/imgui/zip/refs/tags/v1.92.9b-docking
        0434445157a575f452ff0f2d1681fdd90ea8939e0c6983e6f8e47c51fba1bccd
        SOURCE_SUBDIR zen-no-cmake)
    FetchContent_MakeAvailable(zen_imgui)
    # Core and GLFW backend come from the same archive. No Vulkan backend.
    add_library(zen_imgui STATIC
        "${zen_imgui_SOURCE_DIR}/imgui.cpp"
        "${zen_imgui_SOURCE_DIR}/imgui_draw.cpp"
        "${zen_imgui_SOURCE_DIR}/imgui_tables.cpp"
        "${zen_imgui_SOURCE_DIR}/imgui_widgets.cpp"
        "${zen_imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp")
    target_include_directories(zen_imgui PUBLIC "${zen_imgui_SOURCE_DIR}")
    target_compile_definitions(zen_imgui PUBLIC
        IMGUI_USER_CONFIG="${PROJECT_SOURCE_DIR}/ZenUI/Include/UI/ImGuiConfig.h")
    target_link_libraries(zen_imgui PRIVATE glfw)
endif ()
