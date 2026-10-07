# Each library has its own include root, so a consumer of the neutral renderer cannot
# include ImGui or runtime-panel headers it does not link.

# Toolkit-independent draw packets, texture registry and RDG renderer.
add_library(ZenUI STATIC Renderer/Source/UIRenderer.cpp Renderer/Source/UIDrawPacket.cpp)
target_include_directories(ZenUI PUBLIC Renderer/Include)
target_link_libraries(ZenUI PUBLIC ZenCore)

# ImGui adapter: context, platform bridge, fonts and draw-packet conversion.
add_library(ZenImGui STATIC ImGui/Source/UIContext.cpp ImGui/Source/ImGuiDrawPacket.cpp ImGui/Source/ImGuiRenderer.cpp)
target_include_directories(ZenImGui PUBLIC ImGui/Include)
target_link_libraries(ZenImGui PUBLIC ZenUI zen_imgui PRIVATE ZenWindowBackendInternal)

if (ZEN_BUILD_RUNTIME_UI)
  # Runtime panels are independent of the editor model and workspace.
  add_library(ZenRuntimeUI STATIC Runtime/Source/RuntimeDebugUI.cpp Runtime/Source/RuntimeDebugUISettings.cpp
    Runtime/Source/RuntimeDebugUIScene.cpp Runtime/Source/RuntimeDebugUIModels.cpp Runtime/Source/RuntimeSceneControls.cpp)
  target_include_directories(ZenRuntimeUI PUBLIC Runtime/Include)
  target_link_libraries(ZenRuntimeUI PUBLIC ZenImGui)
endif ()

if (BUILD_TESTING)
  add_executable(UIRenderingTest Tests/UIRenderingTests.cpp)
  target_link_libraries(UIRenderingTest PRIVATE ZenUI gtest_main)
  add_test(NAME UIRenderingContractTest COMMAND UIRenderingTest --gtest_filter=UIRenderingContract.*)
  add_test(NAME UIRenderingNativeTest COMMAND UIRenderingTest --gtest_filter=*UIRenderingNative*)
  set_tests_properties(UIRenderingNativeTest PROPERTIES LABELS "vulkan;integration" TIMEOUT 120)

  add_executable(UIDrawPacketTest Tests/UIDrawPacketTests.cpp)
  target_link_libraries(UIDrawPacketTest PRIVATE ZenImGui gtest_main)
  add_test(NAME UIDrawPacketTest COMMAND UIDrawPacketTest)
  set_tests_properties(UIDrawPacketTest PROPERTIES LABELS "unit" TIMEOUT 120)

  if (ZEN_BUILD_RUNTIME_UI)
    add_executable(RuntimeUIIntegrationTest Tests/RuntimeUIIntegrationTests.cpp)
    target_link_libraries(RuntimeUIIntegrationTest PRIVATE ZenRuntimeUI gtest_main)
    add_test(NAME RuntimeUIIntegrationTest COMMAND RuntimeUIIntegrationTest)
    set_tests_properties(RuntimeUIIntegrationTest PROPERTIES LABELS "vulkan;integration" TIMEOUT 120)
  endif ()

  if (ZEN_WINDOW_BACKEND STREQUAL "SDL3")
    add_executable(UIPlatformTest Tests/UIPlatformTests.cpp)
    target_link_libraries(UIPlatformTest PRIVATE ZenImGui ZenWindowBackendInternal gtest_main)
    if (WIN32)
      target_link_libraries(UIPlatformTest PRIVATE dwmapi)
    endif ()
    add_test(NAME UIPlatformTest COMMAND UIPlatformTest)
    set_tests_properties(UIPlatformTest PROPERTIES LABELS "native" TIMEOUT 60)
  endif ()
endif ()
