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
# One window backend and one ImGui platform adapter per executable.
add_library(zen_window_backend INTERFACE)
if (ZEN_WINDOW_BACKEND STREQUAL "SDL3")
    zen_declare_dependency(sdl3 SDL3
        https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-3.4.18.zip
        9cd42377704398796071b8597cd7e21da254a43bad98c4199739647adc13fa6f)
    set(SDL_SHARED OFF CACHE BOOL "Build shared SDL" FORCE)
    set(SDL_STATIC ON CACHE BOOL "Build static SDL" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "Build SDL test library" FORCE)
    set(SDL_TESTS OFF CACHE BOOL "Build SDL tests" FORCE)
    set(SDL_EXAMPLES OFF CACHE BOOL "Build SDL examples" FORCE)
    set(SDL_INSTALL OFF CACHE BOOL "Install SDL" FORCE)
    # Retain video, events and text/cursor support; ZenEngine owns rendering/audio.
    set(SDL_AUDIO OFF CACHE BOOL "SDL audio" FORCE)
    set(SDL_RENDER OFF CACHE BOOL "SDL renderer" FORCE)
    set(SDL_GPU OFF CACHE BOOL "SDL GPU" FORCE)
    set(SDL_CAMERA OFF CACHE BOOL "SDL camera" FORCE)
    FetchContent_MakeAvailable(sdl3)
    target_link_libraries(zen_window_backend INTERFACE SDL3::SDL3-static)
    target_compile_definitions(zen_window_backend INTERFACE ZEN_WINDOW_SDL3=1 SDL_MAIN_HANDLED)
    set(ZEN_IMGUI_PLATFORM sdl3)
else ()
    set(ZEN_IMGUI_PLATFORM glfw)
    target_link_libraries(zen_window_backend INTERFACE glfw)
endif ()

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
if (ZEN_WINDOW_BACKEND STREQUAL "GLFW")
    FetchContent_MakeAvailable(glfw)
endif ()
FetchContent_MakeAvailable(volk vma spdlog spirv_reflect glm gli stb)

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


if (ZEN_BUILD_RUNTIME_UI OR ZEN_BUILD_EDITOR)
    # imgui: v1.92.9b-docking
    zen_declare_dependency(zen_imgui imgui
        https://codeload.github.com/ocornut/imgui/zip/refs/tags/v1.92.9b-docking
        0434445157a575f452ff0f2d1681fdd90ea8939e0c6983e6f8e47c51fba1bccd
        SOURCE_SUBDIR zen-no-cmake)
    FetchContent_MakeAvailable(zen_imgui)
    # Core and the selected platform adapter share one archive. No Vulkan renderer backend.
    add_library(zen_imgui STATIC
        "${zen_imgui_SOURCE_DIR}/imgui.cpp"
        "${zen_imgui_SOURCE_DIR}/imgui_draw.cpp"
        "${zen_imgui_SOURCE_DIR}/imgui_tables.cpp"
        "${zen_imgui_SOURCE_DIR}/imgui_widgets.cpp"
        "${zen_imgui_SOURCE_DIR}/backends/imgui_impl_${ZEN_IMGUI_PLATFORM}.cpp")
    target_include_directories(zen_imgui PUBLIC "${zen_imgui_SOURCE_DIR}")
    target_compile_definitions(zen_imgui PUBLIC
        IMGUI_USER_CONFIG="${PROJECT_SOURCE_DIR}/ZenUI/ImGui/Include/ImGui/ImGuiConfig.h")
    target_link_libraries(zen_imgui PRIVATE zen_window_backend)
endif ()
