# Reusable platform/context + RDG draw backend. A future editor links this target.
add_library(ZenUI STATIC Source/UIContext.cpp Source/UIRenderer.cpp Source/UIDrawPacket.cpp)
target_include_directories(ZenUI PUBLIC Include)
target_link_libraries(ZenUI PUBLIC ZenCore zen_imgui)

# Runtime panels are independent of any future editor model or workspace.
add_library(ZenRuntimeUI STATIC Source/RuntimeDebugUI.cpp Source/RuntimeDebugUISettings.cpp
  Source/RuntimeDebugUIScene.cpp Source/RuntimeSceneControls.cpp)
target_link_libraries(ZenRuntimeUI PUBLIC ZenUI)
target_link_libraries(scene_renderer_demo ZenRuntimeUI)
target_compile_definitions(scene_renderer_demo PRIVATE ZEN_RUNTIME_UI=1)
target_sources(scene_renderer_demo PRIVATE
  ${PROJECT_SOURCE_DIR}/ZenSamples/VulkanRHIDemo/SceneRenderer/SceneRendererDemoUI.cpp)

if (BUILD_TESTING)
  add_executable(UIDrawPacketTest Tests/UIDrawPacketTests.cpp)
  target_link_libraries(UIDrawPacketTest PRIVATE ZenUI gtest_main)
  add_test(NAME UIDrawPacketTest COMMAND UIDrawPacketTest)
  set_tests_properties(UIDrawPacketTest PROPERTIES LABELS "unit" TIMEOUT 120)

  add_executable(RuntimeUIIntegrationTest Tests/RuntimeUIIntegrationTests.cpp)
  target_link_libraries(RuntimeUIIntegrationTest PRIVATE ZenRuntimeUI gtest_main)
  add_test(NAME RuntimeUIIntegrationTest COMMAND RuntimeUIIntegrationTest)
  set_tests_properties(RuntimeUIIntegrationTest PROPERTIES LABELS "vulkan;integration" TIMEOUT 120)
endif ()
